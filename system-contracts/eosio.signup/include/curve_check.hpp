// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
#pragma once
#include "key_codec.hpp"
#include <algorithm>

namespace signup {
using field_element=std::array<uint8_t,32>;
inline field_element field_hex(std::string_view text) {
   field_element out{};
   auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
   for(size_t i=0;i<32;++i)out[i]=uint8_t(nibble(text[2*i])*16+nibble(text[2*i+1]));
   return out;
}
inline bool less(const field_element&a,const field_element&b) {
   return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end());
}
inline void subtract_raw(field_element&a,const field_element&b) {
   int borrow=0;
   for(size_t i=32;i-->0;){const int v=int(a[i])-int(b[i])-borrow;a[i]=uint8_t(v);borrow=v<0;}
}
inline void add_mod(field_element&a,const field_element&b,const field_element&p) {
   unsigned carry=0;
   for(size_t i=32;i-->0;){carry+=unsigned(a[i])+b[i];a[i]=uint8_t(carry);carry>>=8;}
   if(carry||!less(a,p))subtract_raw(a,p);
}
inline void subtract_mod(field_element&a,const field_element&b,const field_element&p) {
   const bool borrow=less(a,b);subtract_raw(a,b);
   if(borrow){unsigned carry=0;for(size_t i=32;i-->0;){carry+=unsigned(a[i])+p[i];a[i]=uint8_t(carry);carry>>=8;}}
}

// Validate compressed K1/P-256 points using the platform modular-exponentiation
// primitive. Both selected curves have cofactor 1 and prime p == 3 (mod 4).
template<class ModExp>
bool valid_point(const decoded_key& key,const ModExp& modexp) {
   if(key.kind>1||(key.bytes[0]!=2&&key.bytes[0]!=3))return false;
   const auto p=field_hex(key.kind==0?
      "fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f":
      "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
   field_element x{};std::copy(key.bytes.begin()+1,key.bytes.end(),x.begin());
   if(!less(x,p))return false;
   field_element exponent3{},exponent2{},rhs{},root{},squared{};exponent3[31]=3;exponent2[31]=2;
   if(!modexp(x,exponent3,p,rhs))return false;
   if(key.kind==0){field_element b{};b[31]=7;add_mod(rhs,b,p);}
   else {
      subtract_mod(rhs,x,p);subtract_mod(rhs,x,p);subtract_mod(rhs,x,p);
      const auto b=field_hex("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b");
      add_mod(rhs,b,p);
   }
   auto exponent=p;unsigned carry=1;
   for(size_t i=32;i-->0;){carry+=exponent[i];exponent[i]=uint8_t(carry);carry>>=8;}
   uint8_t previous=0;
   for(auto& byte:exponent){const uint8_t next=byte&3;byte=uint8_t((previous<<6)|(byte>>2));previous=next;}
   if(!modexp(rhs,exponent,p,root)||!modexp(root,exponent2,p,squared)||squared!=rhs)return false;
   return key.bytes[0]==2||std::any_of(root.begin(),root.end(),[](uint8_t b){return b!=0;});
}
}
