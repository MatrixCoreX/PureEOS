#pragma once
#include <eosio/chain/action.hpp>
#include <optional>
#include <new>
#include <type_traits>
namespace eosio::chain {
// Transaction-local memoization of a pure action-return digest only.
// Every action field and output byte is compared;no code/state/authority result,
// receipt sequence,transaction ID,DB pointer or borrowed input is retained.
// Payload ownership is bounded;copy failure keeps original SHA behavior.
class bounded_action_digest_cache {
 struct entry {action input;std::vector<char> output;digest_type digest;};
 std::optional<entry> cached_;
 static bool same_action(const action& a,const action& b)noexcept {
  if(a.account!=b.account || a.name!=b.name || a.data!=b.data ||
     a.authorization.size()!=b.authorization.size())return false;
  for(size_t i=0;i<a.authorization.size();++i)
   if(a.authorization[i].actor!=b.authorization[i].actor ||
      a.authorization[i].permission!=b.authorization[i].permission)return false;
  return true;
 }
 static bool eligible(const action& a,const std::vector<char>& output)noexcept {
  if(a.authorization.size()>16 || a.data.size()>max_payload_bytes ||
     output.size()>max_payload_bytes-a.data.size())return false;
  return a.authorization.size()*sizeof(permission_level)<=max_payload_bytes-a.data.size()-output.size();
 }
public:
 static constexpr size_t max_payload_bytes=1024;
 bounded_action_digest_cache()=default;
 bounded_action_digest_cache(const bounded_action_digest_cache&)=delete;
 bounded_action_digest_cache& operator=(const bounded_action_digest_cache&)=delete;
 digest_type digest(const action& actual,const std::vector<char>& output,bool includes_return) {
  // Original SHA implementations and full input comparisons remain unchanged.
  if(!includes_return)return digest_type::hash(actual);
  const bool keep=eligible(actual,output);
  if(keep && cached_ && same_action(cached_->input,actual) && cached_->output==output)
   return cached_->digest;
  const auto result=generate_action_digest(actual,output);
  if(keep)try {
   entry next{actual,output,result};
   static_assert(std::is_nothrow_move_constructible_v<entry>);
   cached_.emplace(std::move(next));
  } catch(const std::bad_alloc&) {
   // Optional ownership publication fails closed to the original computed hash.
   // The prior owned entry is unchanged when its replacement copy cannot form.
  }
  return result;
 }
 size_t retained_payload_bytes()const noexcept {
  return cached_?cached_->input.data.size()+cached_->output.size()+
   cached_->input.authorization.size()*sizeof(permission_level):0;
 }
};
}
