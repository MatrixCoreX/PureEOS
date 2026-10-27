#include <chainbase/chainbase.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>
#include <filesystem>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include <ctime>
using namespace chainbase;
std::atomic<unsigned long long> checks{0};
void require(bool value){if(!value)throw std::runtime_error("session-cache regression");++checks;}

struct counts{unsigned start=0,push=0,squash=0,undo=0;};
template<unsigned Tag>struct small{
 counts* c;bool active=true;
 explicit small(counts& v):c(&v){++c->start;}
 small(small&& s)noexcept:c(s.c),active(std::exchange(s.active,false)){}
 ~small(){if(active)++c->undo;}
 void push(){if(active){++c->push;active=false;}}
 void squash(){if(active){++c->squash;active=false;}}
 void undo(){if(active){++c->undo;active=false;}}
};
struct alignas(2*__STDCPP_DEFAULT_NEW_ALIGNMENT__) aligned:small<44>{using small::small;};
struct large:small<45>{using small::small;char bytes[1024]{};};
template<unsigned I>std::unique_ptr<abstract_session> make(counts& c){
 return chainbase::detail::make_index_session<small<I>>([&]{return small<I>(c);});
}
template<unsigned I>std::unique_ptr<abstract_session> old(counts& c){
 return std::unique_ptr<abstract_session>(new session_impl<small<I>>(small<I>(c)));
}
using factory=std::unique_ptr<abstract_session>(*)(counts&);
template<unsigned...I>auto factories(std::integer_sequence<unsigned,I...>){return std::array<factory,sizeof...(I)>{make<I>...};}
template<unsigned...I>auto old_factories(std::integer_sequence<unsigned,I...>){return std::array<factory,sizeof...(I)>{old<I>...};}
template<unsigned...I>void release(std::integer_sequence<unsigned,I...>){(chainbase::detail::release_index_session_cache<small<I>>(),...);}
double cpu(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec/1e9;}

struct cache_object:chainbase::object<234,cache_object> {
 template<typename Constructor>cache_object(Constructor&& c,chainbase::constructor_tag){c(*this);}
 id_type id;int64_t value=0;
};
using namespace boost::multi_index;
using cache_index=multi_index_container<cache_object,indexed_by<
 ordered_unique<member<cache_object,cache_object::id_type,&cache_object::id>>>,chainbase::node_allocator<cache_object>>;
CHAINBASE_SET_INDEX_TYPE(cache_object,cache_index)
struct temp_dir {
 std::filesystem::path path;
 temp_dir(){
  const auto base=std::filesystem::temp_directory_path();
  for(unsigned i=0;i<100;++i){
   path=base/("pureeos-session-cache-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(i));
   if(std::filesystem::create_directory(path))return;
  }
  throw std::runtime_error("cannot create session test directory");
 }
 ~temp_dir(){std::error_code ec;std::filesystem::remove_all(path,ec);}
};
struct throwing_session:small<46>{
 static inline bool fail=false;
 using small::small;
 throwing_session(throwing_session&& other):small<46>(prepare(other)){}
 static small<46>&& prepare(throwing_session& other){
  if(fail)throw std::runtime_error("injected session move");
  return std::move(other);
 }
};
void actual_databases(){
 using typed=typename chainbase::generic_index<cache_index>::session;
 chainbase::detail::release_index_session_cache<typed>();
 std::array<std::exception_ptr,4> failures;
 std::vector<std::thread> threads;
 for(unsigned thread=0;thread<failures.size();++thread)threads.emplace_back([&,thread]{
  try{
   temp_dir temp;
   {
    database db(temp.path,database::read_write,8*1024*1024);db.add_index<cache_index>();
    db.create<cache_object>([&](auto& row){row.value=thread;});
    const auto get=[&]()->const cache_object&{return db.get<cache_object>(cache_object::id_type{0});};
    auto parent=db.start_undo_session(true);
    int64_t expected=thread;
    for(unsigned i=0;i<2000;++i){
     const auto before=expected;
     {
      auto transaction=db.start_undo_session(true);
      db.modify(get(),[](auto& row){++row.value;});
      {
       auto action=db.start_undo_session(true);
       db.modify(get(),[](auto& row){row.value+=100;});
       if(i%2==0)action.squash();
      }
      const auto current=before+1+(i%2==0?100:0);
      require(get().value==current);
      if(i%3==0){transaction.squash();expected=current;}
     }
     require(get().value==expected && db.revision()==1);
     if(i%199==0)chainbase::detail::release_index_session_cache<typed>();
    }
    parent.undo();
    require(get().value==thread && db.revision()==0);
    {auto disabled=db.start_undo_session(false);db.modify(get(),[](auto& row){row.value+=7;});}
    require(get().value==thread+7 && db.revision()==0);
   }
   {
    database db(temp.path,database::read_write,8*1024*1024);db.add_index<cache_index>();
    require(db.get<cache_object>(cache_object::id_type{0}).value==thread+7);
   }
  }catch(...){failures[thread]=std::current_exception();}
 });
 for(auto& thread:threads)thread.join();
 for(const auto& failure:failures)if(failure)std::rethrow_exception(failure);
 if constexpr(chainbase::detail::session_cache_supported<typed>)
  require(chainbase::detail::cached_session_impl<typed>::cached_bytes()==0);
}

int main(){
 static_assert(chainbase::detail::session_cache_supported<small<0>>);
 static_assert(!chainbase::detail::session_cache_supported<aligned>);
 static_assert(!chainbase::detail::session_cache_supported<large>);
 counts c;
 void* address=nullptr;
 {auto a=make<0>(c);address=a.get();a->push();}
 {auto a=make<0>(c);require(a.get()==address);a->squash();}
 {auto a=make<0>(c);auto b=make<0>(c);require(a.get()!=b.get());a->undo();}
 require(c.start==c.push+c.squash+c.undo);
 {auto a=chainbase::detail::make_index_session<aligned>([&]{return aligned(c);});a->push();}
 {auto a=chainbase::detail::make_index_session<large>([&]{return large(c);});a->push();}
 try {
  std::vector<std::unique_ptr<abstract_session>> v;
  for(unsigned i=0;i<30;++i)v.push_back(make<0>(c));
  v.push_back(chainbase::detail::make_index_session<small<0>>([&]()->small<0>{throw 7;}));
  return 2;
 }catch(int){}
 require(c.start==c.push+c.squash+c.undo);
 std::array<counts,4> concurrent;
 std::vector<std::thread> threads;
 for(unsigned i=0;i<concurrent.size();++i)threads.emplace_back([&,i]{
  auto& q=concurrent[i];
  for(unsigned j=0;j<50000;++j){
   auto a=make<0>(q);auto b=make<0>(q);
   if(j%3==0)a->push();else if(j%3==1)a->squash();else a->undo();
   if(j%199==0)chainbase::detail::release_index_session_cache<small<0>>();
  }
 });
 for(auto& t:threads)t.join();
 for(auto& q:concurrent)require(q.start==q.push+q.squash+q.undo);

 {
  throwing_session::fail=true;
  const auto before=c.start;
  try{
   auto value=chainbase::detail::make_index_session<throwing_session>([&]{return throwing_session(c);});
   require(false);
  }catch(const std::runtime_error&){}
  throwing_session::fail=false;
  require(c.start==before+1 && c.start==c.push+c.squash+c.undo);
  chainbase::detail::release_index_session_cache<throwing_session>();
 }
 if constexpr(chainbase::detail::session_cache_supported<small<0>>)
  require(chainbase::detail::cached_session_impl<small<0>>::cached_bytes()<=64);
 actual_databases();
 if(!std::getenv("PUREEOS_SESSION_CACHE_MICROBENCH")){
  release(std::make_integer_sequence<unsigned,40>{});
  std::cout<<checks<<" original-session-cache assertions passed\n";
  return 0;
 }
 auto next=factories(std::make_integer_sequence<unsigned,40>{});
 auto previous=old_factories(std::make_integer_sequence<unsigned,40>{});
 double oldcpu=0,newcpu=0;
 constexpr unsigned rounds=200000;
 for(bool cached:{false,true,true,false}){
  counts q;double start=cpu();
  for(unsigned n=0;n<rounds;++n){
   std::vector<std::unique_ptr<abstract_session>> v;v.reserve(40);
   for(auto f:cached?next:previous)v.push_back(f(q));
   for(auto& a:v)a->squash();
  }
  double elapsed=cpu()-start;if(cached)newcpu+=elapsed;else oldcpu+=elapsed;
  require(q.start==40*rounds && q.squash==q.start);
 }
 release(std::make_integer_sequence<unsigned,40>{});
 std::cout<<"{\"status\":\"bounded-raw-cache-lifetimes-concurrency-and-ABBA-passed\",\"normal_CPU_seconds\":"<<oldcpu<<",\"cached_CPU_seconds\":"<<newcpu<<",\"scope\":\"Original typed session wrappers and unchanged unique_ptr ABI;component microdiagnostic,not node TPS\"}\n";
}
