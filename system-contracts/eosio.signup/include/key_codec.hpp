// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace signup {
struct decoded_key {
   uint8_t kind = 0; // Canonical Antelope tags: K1=0, R1=1.
   std::array<uint8_t,33> bytes{};
};
inline bool operator==(const decoded_key& a,const decoded_key& b) {
   return a.kind==b.kind && a.bytes==b.bytes;
}

// Digest is supplied by the platform. No signing or private-key operation.
template<class Ripemd>
bool decode_public_key(std::string_view text,decoded_key& out,const Ripemd& digest,const char*& error) {
   auto reject=[&](const char* message){error=message;return false;};
   if(text.empty() || text.size()>64) return reject("invalid public-key length");
   bool legacy=false;size_t offset=0;
   if(text.substr(0,3)=="EOS") {legacy=true;offset=3;out.kind=0;}
   else if(text.substr(0,7)=="PUB_K1_") {offset=7;out.kind=0;}
   else if(text.substr(0,7)=="PUB_R1_") {offset=7;out.kind=1;}
   else return reject("use an EOS, PUB_K1 or PUB_R1 public key; never a private key");
   const auto encoded=text.substr(offset);
   if(encoded.empty() || encoded.size()>52 || encoded.front()=='1') return reject("noncanonical public-key encoding");
   constexpr std::string_view alphabet="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
   std::array<uint8_t,37> data{};
   for(char ch:encoded) {
      const auto digit=alphabet.find(ch);
      if(digit==std::string_view::npos) return reject("invalid base58 character");
      unsigned carry=unsigned(digit);
      for(size_t i=data.size();i-->0;) {carry+=unsigned(data[i])*58;data[i]=uint8_t(carry);carry>>=8;}
      if(carry) return reject("public-key encoding overflow");
   }
   if(data[0]!=2 && data[0]!=3) return reject("compressed elliptic-curve public key required");
   std::array<uint8_t,35> input{};
   for(size_t i=0;i<33;++i) {input[i]=data[i];out.bytes[i]=data[i];}
   if(!legacy) {input[33]=out.kind==0?'K':'R';input[34]='1';}
   const auto sum=digest(input.data(),legacy?33:35);
   unsigned mismatch=0;for(size_t i=0;i<4;++i)mismatch|=unsigned(sum[i]^data[33+i]);
   if(mismatch) return reject("public-key checksum mismatch");
   error=nullptr;return true;
}

struct signup_request {std::string account,owner,active;};
inline bool parse_memo(std::string_view input,signup_request& out,const char*& error) {
   auto reject=[&](const char* message){error=message;return false;};
   if(input.empty() || input.size()>180) return reject("invalid signup memo length");
   auto space=[](char c){return c==' '||c=='\t'||c=='\r'||c=='\n';};
   while(!input.empty()&&space(input.front()))input.remove_prefix(1);
   while(!input.empty()&&space(input.back()))input.remove_suffix(1);
   auto first=input.find('-');if(first==std::string_view::npos)first=input.find(' ');
   if(first!=12) return reject("memo must start with a 12-character account name and separator");
   for(char c:input.substr(0,12))
      if(!((c>='a'&&c<='z')||(c>='1'&&c<='5'))) return reject("account name accepts lowercase a-z and digits 1-5 only");
   const auto keys=input.substr(first+1);auto second=keys.find('-');
   const auto owner=keys.substr(0,second);
   const auto active=second==std::string_view::npos?owner:keys.substr(second+1);
   if(owner.empty()||active.empty()||owner.size()>64||active.size()>64||active.find('-')!=std::string_view::npos)
      return reject("provide one public key or separate owner and active public keys");
   out={std::string(input.substr(0,12)),std::string(owner),std::string(active)};
   error=nullptr;return true;
}
}
