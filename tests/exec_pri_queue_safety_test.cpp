
#include <eosio/chain/exec_pri_queue.hpp>
#include <atomic>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <thread>
#include <vector>
#include <chrono>
#include <ctime>
#include <climits>
static thread_local int new_budget=-1;
void*operator new(std::size_t n){if(new_budget==0)throw std::bad_alloc();if(new_budget>0)--new_budget;if(void*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p)noexcept{std::free(p);}
void operator delete(void*p,std::size_t)noexcept{std::free(p);}
void*operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void*p)noexcept{std::free(p);}
void operator delete[](void*p,std::size_t)noexcept{std::free(p);}
struct Production{using Queue=appbase::exec_pri_queue;using Kind=appbase::exec_queue;using Id=appbase::handler_id;};
static void demand(bool x,const char*m){if(!x)throw std::runtime_error(m);}
struct Pending{int priority;uint64_t order,id;bool dedup;unsigned queue;};
static uint64_t comparisons=0;
template<class T>static std::vector<uint64_t> ordering(unsigned seed){
 typename T::Queue q;q.init_read_threads(2);std::mt19937_64 random(seed);
 std::vector<Pending> pending;std::vector<uint64_t> actual,expected;
 auto pop=[&]{
  if(pending.empty())return;
  auto it=std::max_element(pending.begin(),pending.end(),[](auto&a,auto&b){return std::tie(a.priority,a.order)<std::tie(b.priority,b.order);});
  expected.push_back(it->id);pending.erase(it);
  q.execute_highest(T::Kind::read_only,T::Kind::read_write);
  demand(actual==expected,"Priority/FIFO/model mismatch");++comparisons;
 };
 const std::array<int,7> priorities{INT_MIN,-10,-1,0,1,10,INT_MAX};
 for(uint64_t id=1;id<=12000;++id){
  if(!pending.empty()&&(random()%10)<5){pop();continue;}
  int priority=priorities[random()%priorities.size()];unsigned kind=random()%2;bool dedup=random()%8==0;
  Pending p{priority,UINT64_MAX-id,id,dedup,kind};
  const bool skip=dedup&&std::any_of(pending.begin(),pending.end(),[&](auto&v){return v.dedup&&v.priority==priority&&v.queue==kind;});
  if(!skip)pending.push_back(p);
  q.add(dedup?T::Id::process_incoming_block:T::Id::unique,priority,kind?T::Kind::read_write:T::Kind::read_only,p.order,[&,id]{actual.push_back(id);});
  demand(q.size()==pending.size(),"Queue size/dedup mismatch");++comparisons;
 }
 while(!pending.empty())pop();
 demand(q.empty(T::Kind::read_only)&&q.empty(T::Kind::read_write),"Drain not empty");return actual;
}
template<class T>static void reentry_and_exception(){
 typename T::Queue q;std::vector<int> seen;
 q.add(0,T::Kind::read_write,9,[&]{seen.push_back(1);q.add(10,T::Kind::read_write,7,[&]{seen.push_back(3);});});
 q.add(0,T::Kind::read_write,8,[&]{seen.push_back(2);});
 while(!q.empty(T::Kind::read_write))q.execute_highest(T::Kind::read_only,T::Kind::read_write);
 demand(seen==std::vector<int>({1,3,2}),"Reentry priority changed");++comparisons;
 q.add(0,T::Kind::read_write,4,[&]{seen.push_back(4);throw std::runtime_error("expected callback failure");});
 q.add(0,T::Kind::read_write,3,[&]{seen.push_back(5);});
 bool failed=false;try{q.execute_highest(T::Kind::read_only,T::Kind::read_write);}catch(...){failed=true;}
 demand(failed&&q.size()==1,"Throwing callback not consumed exactly once");
 q.execute_highest(T::Kind::read_only,T::Kind::read_write);
 demand(seen==std::vector<int>({1,3,2,4,5}),"Callback exception lost remaining work");++comparisons;
}
struct Tracked{std::atomic<int>*alive;explicit Tracked(std::atomic<int>&a):alive(&a){++*alive;}~Tracked(){--*alive;}};
template<class T>static int clear_payloads(){
 std::atomic<int> alive=0;
 {typename T::Queue q;q.init_read_threads(2);
  for(auto kind:{T::Kind::read_only,T::Kind::read_write,T::Kind::read_exclusive}){
   auto p=std::make_shared<Tracked>(alive);q.add(0,kind,100,[keep=std::move(p)]{});
  }
  demand(alive==3,"Payload setup failed");q.clear();demand(q.empty(T::Kind::read_only)&&q.empty(T::Kind::read_write)&&q.empty(T::Kind::read_exclusive),"Clear leaves pending task");
 }
 return alive;
}
template<class T>static int oom_one(int fail_at, bool dedup, typename T::Kind kind){
 std::atomic<int> alive=0;typename T::Queue q;q.init_read_threads(2);int executed=0;
 auto p=std::make_shared<Tracked>(alive);bool failed=false;
 try{new_budget=fail_at;q.add(dedup?T::Id::process_incoming_block:T::Id::unique,0,kind,9,[keep=std::move(p),&executed]{++executed;});}
 catch(const std::bad_alloc&){failed=true;}new_budget=-1;
 if(failed)demand(q.empty(kind),"Allocation failure changed queued work");
 else while(!q.empty(kind))q.execute_highest_locked(kind);
 demand(executed==(failed?0:1),"Allocation failure callback count incorrect");++comparisons;return alive;
}
template<class T>static void concurrency(){
 typename T::Queue q;q.init_read_threads(4);q.enable_locking([]{return false;});
 constexpr unsigned producers=6,each=4096,total=producers*each;
 std::vector<std::atomic<unsigned>> seen(total);for(auto&v:seen)v=0;
 std::atomic<unsigned> completed=0;std::atomic<uint64_t> order=UINT64_MAX;
 std::vector<std::thread> threads;
 for(unsigned c=0;c<4;++c)threads.emplace_back([&]{
  while(completed.load()<total)if(!q.execute_highest_locked(T::Kind::read_exclusive))std::this_thread::yield();
 });
 for(unsigned p=0;p<producers;++p)threads.emplace_back([&,p]{
  for(unsigned j=0;j<each;++j){unsigned id=p*each+j;
   q.add(int(j%7),T::Kind::read_exclusive,order.fetch_sub(1),[&,id]{++seen[id];++completed;});
  }
 });
 for(auto&t:threads)t.join();q.disable_locking();
 demand(completed==total&&q.empty(T::Kind::read_exclusive),"Concurrent enqueue/drain lost work");
 for(auto&v:seen)demand(v==1,"Concurrent callback duplication");comparisons+=total;
 typename T::Queue blocking;blocking.init_read_threads(4);blocking.enable_locking([]{return false;});
 std::atomic<int> exited=0;threads.clear();
 for(unsigned i=0;i<4;++i)threads.emplace_back([&]{while(blocking.execute_highest_blocking_locked(T::Kind::read_only,T::Kind::read_exclusive)){};++exited;});
 std::this_thread::sleep_for(std::chrono::milliseconds(10));blocking.stop();
 for(auto&t:threads)t.join();blocking.disable_locking();demand(exited==4,"Stop failed to wake all blocked readers");++comparisons;
}

int main(){
 try{
  for(unsigned seed:{1,4345,91377})ordering<Production>(seed);
  reentry_and_exception<Production>();
  demand(clear_payloads<Production>()==0,"Clear retained queued callback payloads");
  for(auto kind:{Production::Kind::read_only,Production::Kind::read_write,Production::Kind::read_exclusive})
   for(bool dedup:{false,true})for(int fail=0;fail<5;++fail)
    demand(oom_one<Production>(fail,dedup,kind)==0,"Failed enqueue leaked callback ownership");
  concurrency<Production>();
  std::cout<<"Task queue independent priority/FIFO/dedup/lifetime/concurrency checks passed: "<<comparisons<<"\n";return 0;
 }catch(const std::exception&e){new_budget=-1;std::cerr<<e.what()<<std::endl;return 2;}
}
