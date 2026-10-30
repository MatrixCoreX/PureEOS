#include <eosio/chain/parallel_input_fingerprint.hpp>
#include <eosio/chain/thread_utils.hpp>
#include <fc/io/raw.hpp>
#include <boost/asio/post.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
namespace eosio::chain::experimental {
namespace {
struct FingerprintThread {};
fc::sha256 shape_fingerprint(const root_fingerprint_binding& binding,const isolated_action_input& input) {
   fc::sha256::encoder shape;constexpr char domain[]="pureeos/access-path-shape/v1";
   shape.write(domain,sizeof(domain));fc::raw::pack(shape,binding.code_hash);
   fc::raw::pack(shape,binding.limits.max_pages);fc::raw::pack(shape,binding.limits.max_call_depth);
   fc::raw::pack(shape,input.receiver);fc::raw::pack(shape,input.account);fc::raw::pack(shape,input.action);
   fc::raw::pack(shape,input.authorizations);fc::raw::pack(shape,input.data.size());
   fc::raw::pack(shape,input.max_return_bytes);return shape.result();
}
}
struct owned_fingerprint_pool::impl {
   const std::thread::id owner=std::this_thread::get_id();
   const size_t participants;bool busy=false;
   named_thread_pool<FingerprintThread> threads;
   explicit impl(size_t n):participants(n) {
      if(n!=1&&n!=2&&n!=4&&n!=8)throw primary_state_budget_exceeded();
      if(n>1)threads.start(n-1,threads.make_on_except_abort());
   }
};
owned_fingerprint_pool::owned_fingerprint_pool(size_t n):impl_(std::make_unique<impl>(n)){}
owned_fingerprint_pool::~owned_fingerprint_pool()=default;
std::vector<owned_fingerprint_input> owned_fingerprint_pool::prepare(
      std::vector<owned_fingerprint_input> inputs,const isolated_cancel& cancel) {
   if(std::this_thread::get_id()!=impl_->owner)throw state_view_wrong_thread();
   if(impl_->busy)throw primary_state_invalid_attempt();
   impl_->busy=true;struct reset_busy{bool& value;~reset_busy(){value=false;}}busy{impl_->busy};
   cancel.check();if(inputs.size()>256)throw primary_state_budget_exceeded();
   size_t bytes=0;constexpr size_t cap=4*1024*1024;
   for(const auto& input:inputs) {
      if(input.roots.empty()||input.roots.size()>16||input.bindings.size()!=input.roots.size())
         throw primary_state_invalid_attempt();
      for(const auto& root:input.roots) {
         if(root.data.size()>64*1024||root.authorizations.size()>64)throw primary_state_budget_exceeded();
         const size_t amount=root.data.size()+root.authorizations.size()*sizeof(root.authorizations[0])+sizeof(root);
         if(amount>cap-bytes)throw primary_state_budget_exceeded();bytes+=amount;
      }
   }
   if(inputs.empty())return {};
   struct frame_type {
      std::vector<owned_fingerprint_input> inputs;std::vector<std::exception_ptr> errors;
      isolated_cancel cancel;std::atomic<size_t> next{0};
      std::mutex mutex;std::condition_variable done;size_t pending=0;
      void drain()noexcept {
         for(;;) {
            const auto i=next.fetch_add(1,std::memory_order_relaxed);if(i>=inputs.size())break;
            try {
               cancel.check();auto& input=inputs[i];std::vector<fc::sha256> execution,hints;
               execution.reserve(input.roots.size());hints.reserve(input.roots.size());
               for(size_t r=0;r<input.roots.size();++r) {
                  cancel.check();const auto& binding=input.bindings[r];const auto& root=input.roots[r];
                  execution.push_back(isolated_execution_fingerprint(binding.code_hash,binding.limits,root));
                  hints.push_back(shape_fingerprint(binding,root));
               }
               input.execution_fingerprint=isolated_action_tree_fingerprint(execution,input.context);
               input.hint_fingerprint=isolated_action_tree_fingerprint(hints,input.context);
            }catch(...){errors[i]=std::current_exception();}
         }
      }
   };
   auto frame=std::make_shared<frame_type>();frame->inputs=std::move(inputs);
   frame->errors.resize(frame->inputs.size());frame->cancel=cancel;
   std::exception_ptr dispatch_error;
   for(size_t i=0;i+1<impl_->participants;++i) {
      {std::lock_guard lock(frame->mutex);++frame->pending;}
      try {
         boost::asio::post(impl_->threads.get_executor(),[frame] {
            frame->drain();{std::lock_guard lock(frame->mutex);--frame->pending;}frame->done.notify_one();
         });
      } catch(...) {
         {std::lock_guard lock(frame->mutex);--frame->pending;}
         dispatch_error=std::current_exception();break;
      }
   }
   frame->drain();
   {std::unique_lock lock(frame->mutex);frame->done.wait(lock,[&]{return frame->pending==0;});}
   // An asio handler may retain the frame briefly after notifying. Always empty
   // its owned payload before unwinding or returning to another preparation.
   struct clear_inputs{std::shared_ptr<frame_type> frame;~clear_inputs(){frame->inputs.clear();}}clear{frame};
   cancel.check();if(dispatch_error)std::rethrow_exception(dispatch_error);
   for(const auto& error:frame->errors)if(error)std::rethrow_exception(error);
   return std::move(frame->inputs);
}
}
