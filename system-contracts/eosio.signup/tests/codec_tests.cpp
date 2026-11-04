// Remote-only standalone checks. No chain, wallet or persisted private keys.
#include "key_codec.hpp"
#include "curve_check.hpp"
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace signup;
namespace {
size_t checks=0;
void expect(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
using bn_ptr=std::unique_ptr<BIGNUM,decltype(&BN_free)>;
bn_ptr number(const uint8_t* p,size_t n){return bn_ptr(BN_bin2bn(p,int(n),nullptr),BN_free);}
std::array<uint8_t,20> ripemd(const uint8_t*p,size_t n){
   std::array<uint8_t,20> out{};unsigned size=0;
   if(EVP_Digest(p,n,out.data(),&size,EVP_ripemd160(),nullptr)!=1||size!=20)throw std::runtime_error("RIPEMD backend");
   return out;
}
bool exponentiate(const field_element& a,const field_element& e,const field_element& p,field_element& out){
   auto aa=number(a.data(),32),ee=number(e.data(),32),pp=number(p.data(),32);bn_ptr r(BN_new(),BN_free);
   std::unique_ptr<BN_CTX,decltype(&BN_CTX_free)> ctx(BN_CTX_new(),BN_CTX_free);
   return BN_mod_exp(r.get(),aa.get(),ee.get(),pp.get(),ctx.get())==1&&BN_bn2binpad(r.get(),out.data(),32)==32;
}
std::string encode(const decoded_key& key,bool legacy=false){
   std::vector<uint8_t> body(key.bytes.begin(),key.bytes.end()),digest_input=body;
   if(!legacy){digest_input.push_back(key.kind==0?'K':'R');digest_input.push_back('1');}
   auto sum=ripemd(digest_input.data(),digest_input.size());body.insert(body.end(),sum.begin(),sum.begin()+4);
   auto value=number(body.data(),body.size());std::string output;
   const std::string alphabet="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
   while(!BN_is_zero(value.get())){auto r=BN_div_word(value.get(),58);if(r==static_cast<BN_ULONG>(-1))throw std::runtime_error("base58 backend");output.push_back(alphabet.at(size_t(r)));}
   std::reverse(output.begin(),output.end());
   return (legacy?std::string("EOS"):key.kind==0?std::string("PUB_K1_"):std::string("PUB_R1_"))+output;
}
bool openssl_point(const decoded_key& key){
   if(key.kind>1||(key.bytes[0]!=2&&key.bytes[0]!=3))return false;
   std::unique_ptr<EC_GROUP,decltype(&EC_GROUP_free)> group(EC_GROUP_new_by_curve_name(key.kind==0?NID_secp256k1:NID_X9_62_prime256v1),EC_GROUP_free);
   std::unique_ptr<EC_POINT,decltype(&EC_POINT_free)> point(EC_POINT_new(group.get()),EC_POINT_free);
   return EC_POINT_oct2point(group.get(),point.get(),key.bytes.data(),key.bytes.size(),nullptr)==1&&
          EC_POINT_is_on_curve(group.get(),point.get(),nullptr)==1&&!EC_POINT_is_at_infinity(group.get(),point.get());
}
decoded_key fresh_public(uint8_t kind){
   std::unique_ptr<EC_GROUP,decltype(&EC_GROUP_free)> group(EC_GROUP_new_by_curve_name(kind==0?NID_secp256k1:NID_X9_62_prime256v1),EC_GROUP_free);
   bn_ptr order(BN_new(),BN_free);std::unique_ptr<BIGNUM,decltype(&BN_clear_free)> scalar(BN_new(),BN_clear_free);
   expect(EC_GROUP_get_order(group.get(),order.get(),nullptr)==1,"curve order");
   do{expect(BN_rand_range(scalar.get(),order.get())==1,"ephemeral randomness");}while(BN_is_zero(scalar.get()));
   std::unique_ptr<EC_POINT,decltype(&EC_POINT_free)> point(EC_POINT_new(group.get()),EC_POINT_free);
   expect(EC_POINT_mul(group.get(),point.get(),scalar.get(),nullptr,nullptr,nullptr)==1,"public test point");
   decoded_key key;key.kind=kind;
   expect(EC_POINT_point2oct(group.get(),point.get(),POINT_CONVERSION_COMPRESSED,key.bytes.data(),33,nullptr)==33,"compressed public point");
   return key;
}
void field_cases(uint8_t kind){
   const auto p=field_hex(kind==0?"fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f":"ffffffff00000001000000000000000000000000ffffffffffffffffffffffff");
   auto pp=number(p.data(),32);std::unique_ptr<BN_CTX,decltype(&BN_CTX_free)> ctx(BN_CTX_new(),BN_CTX_free);
   for(unsigned i=0;i<128;++i){
      bn_ptr aa(BN_new(),BN_free),bb(BN_new(),BN_free),r(BN_new(),BN_free);
      expect(BN_rand_range(aa.get(),pp.get())==1&&BN_rand_range(bb.get(),pp.get())==1,"public arithmetic randomness");
      field_element a{},b{},expected{};BN_bn2binpad(aa.get(),a.data(),32);BN_bn2binpad(bb.get(),b.data(),32);
      auto sum=a;add_mod(sum,b,p);BN_mod_add(r.get(),aa.get(),bb.get(),pp.get(),ctx.get());BN_bn2binpad(r.get(),expected.data(),32);
      expect(sum==expected,"modular addition vs independent BIGNUM");
      auto difference=a;subtract_mod(difference,b,p);BN_mod_sub(r.get(),aa.get(),bb.get(),pp.get(),ctx.get());BN_bn2binpad(r.get(),expected.data(),32);
      expect(difference==expected,"modular subtraction vs independent BIGNUM");
   }
}
}
int main(){try{
   std::vector<std::string> examples;
   for(uint8_t kind:{0,1}){
      field_cases(kind);
      for(unsigned i=0;i<24;++i){
         auto key=fresh_public(kind);const char* error=nullptr;decoded_key decoded;
         auto modern=encode(key);expect(decode_public_key(modern,decoded,ripemd,error)&&decoded==key,"modern round trip");
         expect(valid_point(decoded,exponentiate),"valid point accepted by production point checker");
         auto bad=modern;bad.back()=bad.back()=='1'?'2':'1';expect(!decode_public_key(bad,decoded,ripemd,error),"checksum corruption rejected");
         auto suffix=modern.substr(7);expect(!decode_public_key((kind==0?"PUB_R1_":"PUB_K1_")+suffix,decoded,ripemd,error),"curve suffix checksum bound");
         expect(!decode_public_key(modern.substr(0,7)+"1"+suffix,decoded,ripemd,error),"noncanonical leading zero rejected");
         if(kind==0){auto legacy=encode(key,true);expect(decode_public_key(legacy,decoded,ripemd,error)&&decoded==key,"EOS legacy identity");}
         if(i==0)examples.push_back(modern);
      }
      for(unsigned i=0;i<128;++i){
         decoded_key key;key.kind=kind;expect(RAND_bytes(key.bytes.data(),33)==1,"public candidate randomness");key.bytes[0]=uint8_t(2+(i&1));
         expect(valid_point(key,exponentiate)==openssl_point(key),"point checker vs independent OpenSSL curve implementation");
      }
      decoded_key invalid;invalid.kind=kind;invalid.bytes.fill(255);invalid.bytes[0]=2;
      expect(!valid_point(invalid,exponentiate),"out-of-field coordinate rejected");invalid.bytes[0]=4;
      expect(!valid_point(invalid,exponentiate),"uncompressed prefix rejected");
   }
   for(const auto& text:std::vector<std::string>{"","EOS","PUB_K1_","PUB_R1_","PVT_K1_redacted","PVT_R1_redacted","PUB_WA_unsupported",std::string(65,'A')}){
      decoded_key key;const char* error=nullptr;expect(!decode_public_key(text,key,ripemd,error),"unsupported or private-key input rejected");
   }
   const char* error=nullptr;signup_request request;
   expect(parse_memo("abcdefghijkl-"+examples[0],request,error)&&request.owner==request.active,"single-key memo");
   expect(parse_memo("abcdefghijkl-"+examples[0]+"-"+examples[1],request,error)&&request.owner!=request.active,"separate K1/R1 owner/active memo");
   expect(parse_memo(" \tabcdefghijkl "+examples[0]+"\r\n",request,error),"legacy separator and outer whitespace");
   for(const auto& memo:std::vector<std::string>{"", "short-"+examples[0],"ABCDEFGHIJKL-"+examples[0],"abcdefghi6kl-"+examples[0],"eosio.signup-"+examples[0],"abcdefghijkl-", "abcdefghijkl-"+examples[0]+"-", "abcdefghijkl-"+examples[0]+"-"+examples[1]+"-extra",std::string(181,'a')})
      expect(!parse_memo(memo,request,error),"invalid memo rejected");
   std::cout<<"{\"status\":\"PASS\",\"checks\":"<<checks<<",\"scope\":\"standalone codec/field/curve checks only; no native chain execution\"}\n";
   return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
