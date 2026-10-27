#pragma once
#include <fc/crypto/sha256.hpp>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace eosio::chain::experimental {

// Owned immutable code bytes. Copies share only bytes/digest, never a VM or DB
// pointer. Replacement/resize publishes a new value, leaving worker inputs intact.
class immutable_code {
   std::shared_ptr<const std::vector<char>> bytes_;
   fc::sha256 digest_;
   void replace(std::vector<char> value) {
      auto frozen=std::make_shared<const std::vector<char>>(std::move(value));
      auto digest=frozen->empty() ? fc::sha256{} : fc::sha256::hash(frozen->data(),frozen->size());
      bytes_=std::move(frozen);digest_=digest;
   }
public:
   class byte_reference {
      immutable_code* owner_;
      size_t index_;
   public:
      byte_reference(immutable_code& owner,size_t index):owner_(&owner),index_(index) {}
      operator char() const {return owner_->view().at(index_);}
      byte_reference& operator=(char value) {
         auto next=owner_->view();next.at(index_)=value;owner_->replace(std::move(next));return *this;
      }
      byte_reference& operator=(const byte_reference& other) {return *this=static_cast<char>(other);}
      byte_reference& operator^=(char value) {return *this=static_cast<char>(static_cast<char>(*this)^value);}
   };
   immutable_code()=default;
   immutable_code(std::vector<char> value) {replace(std::move(value));}
   immutable_code(std::initializer_list<char> value) {replace(std::vector<char>(value));}
   template<typename Iterator> immutable_code(Iterator first,Iterator last) {assign(first,last);}
   immutable_code& operator=(std::vector<char> value) {replace(std::move(value));return *this;}
   immutable_code& operator=(std::initializer_list<char> value) {replace(std::vector<char>(value));return *this;}
   const std::vector<char>& view() const noexcept {
      static const std::vector<char> empty;
      return bytes_ ? *bytes_ : empty;
   }
   operator const std::vector<char>&() const noexcept {return view();}
   const char* data() const noexcept {return view().data();}
   size_t size() const noexcept {return bytes_ ? bytes_->size() : 0;}
   bool empty() const noexcept {return !size();}
   auto begin() const noexcept {return view().begin();}
   auto end() const noexcept {return view().end();}
   char operator[](size_t index) const {return view()[index];}
   byte_reference operator[](size_t index) {return byte_reference(*this,index);}
   const fc::sha256& content_hash() const noexcept {return digest_;}
   // Exact SHA256(bytes), including SHA256(empty). content_hash() keeps its
   // established all-zero no-code sentinel. Never substitute one for the other.
   const fc::sha256& byte_hash() const {
      if (!empty()) return digest_;
      static const auto empty_digest=fc::sha256::hash("",0);
      return empty_digest;
   }
   template<typename Iterator> void assign(Iterator first,Iterator last) {replace(std::vector<char>(first,last));}
   void clear() noexcept {bytes_.reset();digest_=fc::sha256{};}
   void resize(size_t count,char value=0) {
      auto replacement=view();replacement.resize(count,value);replace(std::move(replacement));
   }
   friend bool operator==(const immutable_code& a,const immutable_code& b) {
      return a.bytes_==b.bytes_ || a.view()==b.view();
   }
   friend bool operator==(const immutable_code& a,const std::vector<char>& b) {return a.view()==b;}
   friend bool operator==(const std::vector<char>& a,const immutable_code& b) {return a==b.view();}
};
} // namespace eosio::chain::experimental
