#pragma once
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace eosio::chain::experimental {

// Owned immutable BYTES/catalog, never a VM, overlay or database capability.
// Construction owns a copy/move; there is no shared_ptr<T> adoption API which
// could leave a mutable alias. Copying this handle never copies its payload.
template<class Input>
class immutable_worker_input {
   std::shared_ptr<const Input> value_;
public:
   immutable_worker_input() = default;
   immutable_worker_input(Input input) : value_(std::make_shared<const Input>(std::move(input))) {}
   explicit operator bool() const noexcept { return bool(value_); }
   const Input& operator*() const { return *value_; }
   const Input* operator->() const noexcept { return value_.get(); }
   void reset() noexcept { value_.reset(); }
};

// One owner, one transaction/catalog version, one joined preparation call.
// invalidate BEFORE changing roots/programs/context; changing only captured
// state/tickets needs no input copy. Never retain this cache across adoption,
// prefix refresh, undo or another batch. Published old versions stay immutable.
template<class Input>
class joined_input_cache {
   const std::thread::id owner_ = std::this_thread::get_id();
   immutable_worker_input<Input> published_;
   void check_owner() const {
      if (std::this_thread::get_id() != owner_) throw std::logic_error("worker input cache used by non-owner");
   }
public:
   joined_input_cache() = default;
   joined_input_cache(const joined_input_cache&) = delete;
   joined_input_cache& operator=(const joined_input_cache&) = delete;
   joined_input_cache(joined_input_cache&&) noexcept = default;
   immutable_worker_input<Input> get(const Input& current) {
      check_owner();
      if (!published_) published_ = immutable_worker_input<Input>{current};
      return published_;
   }
   void invalidate() { check_owner(); published_.reset(); }
};
} // namespace eosio::chain::experimental
