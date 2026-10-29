
#include <eosio/chain/parallel_native_capture.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/thread_utils.hpp>
#include <boost/asio/post.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace eosio::chain::experimental {
namespace {
struct NativeRead {};
// The sole owner remains inside one synchronous call. Normal DB mutators and
// undo reject writes in this mode; no caller callback is invoked while active.
struct read_mode_guard {
   chainbase::database& db;
   explicit read_mode_guard(chainbase::database& source):db(source) {
      if(db.is_read_only())throw primary_state_invalid_attempt();
      (void)db.get_mutable_index<table_id_multi_index>(); // require prior write mode
      db.set_read_only_mode();
   }
   ~read_mode_guard()noexcept {db.unset_read_only_mode();} // file open mode is RW
};
void add_stats(primary_snapshot_capture_stats& out,const primary_snapshot_capture_stats& in) {
   out.tables_read+=in.tables_read;out.primary_queries_read+=in.primary_queries_read;
   out.primary_rows_copied+=in.primary_rows_copied;out.secondary_queries_read+=in.secondary_queries_read;
   out.accounts_read+=in.accounts_read;
}
}
struct bounded_native_capture_pool::impl {
   const std::thread::id owner=std::this_thread::get_id();
   const size_t participants;
   bool busy=false;
   std::atomic<bool> active{false};
   named_thread_pool<NativeRead> threads;
   explicit impl(size_t n):participants(n) {
      if(n!=1&&n!=2&&n!=4&&n!=8)throw primary_state_budget_exceeded();
      if(n>1)threads.start(n-1,threads.make_on_except_abort());
   }
};
bounded_native_capture_pool::bounded_native_capture_pool(size_t n):impl_(std::make_unique<impl>(n)){}
bounded_native_capture_pool::~bounded_native_capture_pool()=default;
bool bounded_native_capture_pool::active()const noexcept{return impl_->active.load(std::memory_order_acquire);}

std::shared_ptr<const frozen_primary_state> bounded_native_capture_pool::capture(
      chainbase::database& db,std::span<const table_key> keys,std::span<const primary_query> queries,
      primary_snapshot_limits limits,const isolated_cancel& cancel,std::span<const name> accounts,
      primary_snapshot_capture_stats* stats) {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   if(impl_->busy)throw primary_state_invalid_attempt();
   impl_->busy=true;
   struct reset_busy {bool& value;~reset_busy(){value=false;}} busy{impl_->busy};
   cancel.check();
   if(keys.size()>limits.max_tables||queries.size()>limits.max_queries||accounts.size()>limits.max_accounts)
      throw primary_state_budget_exceeded();
   struct part {
      std::vector<table_key> keys;
      std::vector<primary_query> queries;
      std::vector<name> accounts;
      primary_snapshot_limits limits;
      primary_snapshot_capture_stats stats;
      std::shared_ptr<const frozen_primary_state> snapshot;
      std::exception_ptr error;
   };
   struct frame_type {
      const chainbase::database* source=nullptr;
      isolated_cancel cancel;
      std::vector<part> parts;
      std::mutex mutex;
      std::condition_variable done;
      size_t pending=0;
   };
   auto frame=std::make_shared<frame_type>();
   frame->cancel=cancel;frame->parts.resize(impl_->participants);
   std::vector<table_key> all_keys(keys.begin(),keys.end());
   all_keys.reserve(keys.size()+queries.size());
   for(const auto& query:queries)all_keys.push_back(query.table);
   const table_key_less less;
   std::sort(all_keys.begin(),all_keys.end(),less);
   all_keys.erase(std::unique(all_keys.begin(),all_keys.end()),all_keys.end());
   if(all_keys.size()>limits.max_tables)throw primary_state_budget_exceeded();
   std::vector<name> unique_accounts(accounts.begin(),accounts.end());
   std::sort(unique_accounts.begin(),unique_accounts.end());
   unique_accounts.erase(std::unique(unique_accounts.begin(),unique_accounts.end()),unique_accounts.end());
   for(size_t i=0;i<frame->parts.size();++i) {
      auto& part=frame->parts[i];part.limits=limits;
      // Disjoint tables prevent duplicate row copies across partitions.
      // The quotas sum to exactly the original global row/value caps.
      part.limits.max_rows=limits.max_rows/frame->parts.size()+(i<limits.max_rows%frame->parts.size());
      part.limits.max_value_bytes=limits.max_value_bytes/frame->parts.size()+(i<limits.max_value_bytes%frame->parts.size());
   }
   for(size_t i=0;i<all_keys.size();++i)frame->parts[i%frame->parts.size()].keys.push_back(all_keys[i]);
   for(const auto& query:queries) {
      const auto position=std::lower_bound(all_keys.begin(),all_keys.end(),query.table,less);
      if(position==all_keys.end()||*position!=query.table)throw primary_state_invalid_attempt();
      const auto index=size_t(position-all_keys.begin())%frame->parts.size();
      frame->parts[index].queries.push_back(query);
   }
   for(size_t i=0;i<unique_accounts.size();++i)frame->parts[i%frame->parts.size()].accounts.push_back(unique_accounts[i]);
   cancel.check();
   read_mode_guard mode(db);
   frame->source=&db;
   impl_->active.store(true,std::memory_order_release);
   struct reset_active {std::atomic<bool>& value;~reset_active(){value.store(false,std::memory_order_release);}} active{impl_->active};
   const auto execute_part=[frame](size_t i)noexcept {
      auto& part=frame->parts[i];
      try {
         frame->cancel.check();
         if(!frame->source)throw primary_state_invalid_attempt();
         // Construct this owner-affine serial adapter on THIS native reader.
         // It never exposes native references; the existing builder copies all.
         chainbase_transaction_state_view view(*frame->source);
         part.snapshot=frozen_primary_state::capture_queries_impl(
            view,nullptr,part.keys,part.queries,{},part.limits,
            [frame]{frame->cancel.check();},part.accounts,nullptr,&part.stats);
      } catch(...) {part.error=std::current_exception();}
   };
   std::exception_ptr dispatch_error;
   for(size_t i=0;i+1<frame->parts.size();++i) {
      {std::lock_guard lock(frame->mutex);++frame->pending;}
      try {
         boost::asio::post(impl_->threads.get_executor(),[frame,execute_part,i] {
            execute_part(i);
            {std::lock_guard lock(frame->mutex);--frame->pending;}
            frame->done.notify_one();
         });
      } catch(...) {
         {std::lock_guard lock(frame->mutex);--frame->pending;}
         dispatch_error=std::current_exception();break;
      }
   }
   if(!dispatch_error)execute_part(frame->parts.size()-1); // owner contributes
   {std::unique_lock lock(frame->mutex);frame->done.wait(lock,[&]{return frame->pending==0;});}
   frame->source=nullptr; // no native capability survives the joined phase
   // Queued handler destruction may retain frame metadata briefly. Release
   // every owned partition payload before fallback/unwind to avoid a second cap.
   struct release_outputs {
      std::shared_ptr<frame_type> frame;
      ~release_outputs(){for(auto& part:frame->parts)part.snapshot.reset();frame->source=nullptr;}
   } release{frame};
   if(stats)for(const auto& part:frame->parts)add_stats(*stats,part.stats);
   cancel.check();
   if(dispatch_error)std::rethrow_exception(dispatch_error);
   bool partition_limit=false;
   for(const auto& part:frame->parts)if(part.error) {
      try {std::rethrow_exception(part.error);}
      catch(const primary_state_budget_exceeded&){partition_limit=true;}
   }
   if(partition_limit)throw native_capture_partition_limit();
   auto result=std::shared_ptr<frozen_primary_state>(new frozen_primary_state);
   result->tables_.reserve(all_keys.size());result->accounts_.reserve(unique_accounts.size());
   for(const auto& part:frame->parts) {
      if(!part.snapshot)throw primary_state_invalid_attempt();
      const auto& source=*part.snapshot;
      if(source.row_count_>limits.max_rows-result->row_count_||
         source.value_bytes_>limits.max_value_bytes-result->value_bytes_||
         source.query_count_>limits.max_queries-result->query_count_)
         throw primary_state_budget_exceeded();
      for(const auto& [key,table]:source.tables_)
         if(!result->tables_.emplace(key,table).second)throw primary_state_invalid_attempt();
      for(const auto& [account,read]:source.accounts_)
         if(!result->accounts_.emplace(account,read).second)throw primary_state_invalid_attempt();
      result->row_count_+=source.row_count_;result->value_bytes_+=source.value_bytes_;
      result->query_count_+=source.query_count_;
   }
   if(result->tables_.size()>limits.max_tables||result->accounts_.size()>limits.max_accounts)
      throw primary_state_budget_exceeded();
   return result;
}

std::shared_ptr<const frozen_primary_state> frozen_primary_capture_session::capture_parallel(
      chainbase::database& db,bounded_native_capture_pool& pool,
      std::span<const table_key> keys,std::span<const primary_query> queries,
      primary_snapshot_limits limits,const isolated_cancel& cancel,std::span<const name> accounts,
      const std::function<void()>& owner_checkpoint) {
   check_owner();source_.require_owner_thread();
   if(&db!=&source_.db_||captured_)throw primary_state_invalid_attempt();
   try {
      auto next=pool.capture(db,keys,queries,limits,cancel,accounts,&stats_);
      captured_=next;return next;
   } catch(const native_capture_partition_limit&) {
      // All native readers have joined and their partial values are gone.
      // Retry with the ORIGINAL global limits and original absolute checkpoint,
      // never another 4 MiB allowance or a renewed preparation deadline.
      return capture(keys,queries,{},limits,[&]{cancel.check();if(owner_checkpoint)owner_checkpoint();},accounts);
   }
}
}
