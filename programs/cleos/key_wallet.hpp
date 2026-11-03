// SPDX-License-Identifier: LicenseRef-AIC-Noncommercial-3.0
// Included after cleos's ABI/RPC/transaction helpers. No user private key flag:
// owner intent signatures are made by the user's existing local keosd wallet.
namespace key_client {
constexpr auto account = "eosio.key"_n;
inline bool is_public_key(const string& s) { return s.starts_with("PUB_") || s.starts_with("EOS"); }
inline public_key_type k1(const string& text) {
   public_key_type key(text);
   FC_ASSERT(key.which() == 0 && key.valid(), "A valid K1 public key is required");
   return key;
}
inline fc::variant configuration() {
   auto value = call(get_table_func, fc::mutable_variant_object("json",true)("code",account)("scope",account)("table","config")("limit",1));
   FC_ASSERT(value["rows"].get_array().size()==1, "eosio.key is not initialized");
   auto cfg=value["rows"].get_array().front();auto info=get_info();
   FC_ASSERT(cfg["version"].as_uint64()==1 && cfg["chain_id"].as<chain_id_type>()==info.chain_id && cfg["fee_symbol"].as_string()=="4,AIC", "Unsupported wallet domain or fee asset");
   return cfg;
}
inline asset fee_cap(const string& text) {
   FC_ASSERT(!text.empty(), "An explicit AIC maximum fee is required");
   auto fee=asset::from_string(text);
   FC_ASSERT(fee.get_symbol()==symbol::from_string("4,AIC") && fee.get_amount()>=0, "Fee cap must be nonnegative 4-decimal AIC");
   return fee;
}
inline std::vector<chain::action> deposit(name sender, const string& recipient, name token, const asset& quantity,
                                           const string& maximum, const string& funding) {
   auto cfg=configuration();auto key=k1(recipient);auto cap=fee_cap(maximum);
   FC_ASSERT(quantity.get_amount()>0, "Principal must be positive");
   std::vector<chain::action> actions;
   if(!funding.empty()) {
      auto amount=fee_cap(funding);FC_ASSERT(amount.get_amount()>0,"Fee funding must be positive");
      actions.push_back(create_transfer(cfg["fee_contract"].as<name>(),sender,account,amount,"fee"));
   }
   actions.push_back(create_transfer(token,sender,account,quantity,
      "PK1|"+key.to_string({})+"|"+std::to_string(cap.get_amount())));
   return actions;
}
inline fc::variant wallet(const public_key_type& key) {
   auto packed=fc::raw::pack(key);auto hash=digest_type::hash(packed.data(),packed.size());
   auto response=call(get_table_func,fc::mutable_variant_object("json",true)("code",account)("scope",account)("table","wallets")
      ("index_position","2")("key_type","sha256")("lower_bound",hash.str())("upper_bound",hash.str())("limit",2));
   const auto& rows=response["rows"].get_array();
   FC_ASSERT(rows.size()==1 && rows[0]["key"].as<public_key_type>()==key,"Sender public-key wallet not found");
   return rows[0];
}
inline digest_type digest(const string& operation,const fc::variant& request,const fc::variant& cfg) {
   FC_ASSERT(operation=="ksend"||operation=="withdraw"||operation=="selfout"||operation=="cancel"||operation=="selfcancel", "Unsupported key operation");
   const bool cancel=operation=="cancel"||operation=="selfcancel";
   const std::vector<string> fields=cancel?std::vector<string>{"from","nonce","next_nonce","expires","relayer","reward_to","max_fee"}:
      std::vector<string>{"from","to","token_contract","quantity","memo","nonce","expires","relayer","reward_to","max_fee"};
   FC_ASSERT(request.get_object().size()==fields.size(),"Signed request must contain exactly the protocol fields");
   for(const auto& field:fields)FC_ASSERT(request.get_object().contains(field.c_str()),"Missing signed request field ${field}",("field",field));
   k1(request["from"].as_string());fee_cap(request["max_fee"].as_string());
   if(operation=="ksend")k1(request["to"].as_string());
   const bool self=operation=="selfout"||operation=="selfcancel";
   FC_ASSERT(self?(request["relayer"].as_string().empty()&&request["reward_to"].as_string().empty()):
      (!request["relayer"].as_string().empty()&&request["relayer"]==request["reward_to"]),"Selected BP/reward fields do not match operation");
   if(!cancel)FC_ASSERT(asset::from_string(request["quantity"].as_string()).get_amount()>0&&request["memo"].as_string().size()<=128,"Invalid amount or memo");
   auto abi=abi_serializer_resolver(account);FC_ASSERT(abi,"Missing eosio.key ABI");
   bytes result;
   auto append=[&](const auto& object){auto b=fc::raw::pack(object);result.insert(result.end(),b.begin(),b.end());};
   append(string("PureEOS:eosio.key"));append(uint32_t(1));append(cfg["chain_id"].as<chain_id_type>());
   append(account);append(cfg["deployment"].as<digest_type>());append(name(operation));
   auto body=abi->variant_to_binary(cancel?"cancellation_request":operation=="ksend"?"transfer_request":"withdrawal_request",
                                   request,abi_serializer::create_yield_function(abi_serializer_max_time));
   result.insert(result.end(),body.begin(),body.end());return digest_type::hash(result.data(),result.size());
}
inline signature_type owner_signature(const digest_type& hash,const public_key_type& key) {
   auto sig=call(wallet_url,"/v1/wallet/sign_digest",fc::variants{fc::variant(hash),fc::variant(key)}).as<signature_type>();
   FC_ASSERT(public_key_type(sig,hash)==key,"Wallet returned an incorrect owner signature");return sig;
}
struct commands {
   string from,to,amount,token="eosio.token",maximum,memo,bp,input,payer,relay_url;
   uint32_t valid_seconds=600;bool self=false,allow_http=false;
   commands(CLI::App& app) {
      auto root=app.add_subcommand("key","Public-key transfers via eosio.key; local wallet signing, extra AIC fees")->require_subcommand();
      auto prepare=root->add_subcommand("prepare","Prepare a public-key send or withdrawal to an existing text account");
      prepare->add_option("from",from,"Sender K1 public key")->required();
      prepare->add_option("to",to,"Recipient K1 public key or existing native account")->required();
      prepare->add_option("quantity",amount,"Full principal, e.g. 1.0000 AIC")->required();
      prepare->add_option("--token-contract,-c",token,"Issuing token contract");
      prepare->add_option("--max-fee",maximum,"Maximum TOTAL extra AIC fee, including BP markup")->required();
      prepare->add_option("--bp",bp,"Selected registered BP relay; required unless --self");
      prepare->add_option("--memo",memo,"Recipient memo, at most 128 UTF-8 bytes");
      prepare->add_option("--valid-seconds",valid_seconds,"Owner intent validity, 30..3500 seconds");
      prepare->add_flag("--self",self,"Prepare owner withdrawal with an independent native fee payer; no BP markup");
      prepare->callback([this]{
         FC_ASSERT(valid_seconds>=30&&valid_seconds<=3500,"Invalid validity interval");
         FC_ASSERT((self&&bp.empty()&&!is_public_key(to))||(!self&&!bp.empty()),"Choose a BP or --self native withdrawal");
         auto cfg=configuration();auto key=k1(from);auto source=wallet(key);
         auto operation=self?string("selfout"):is_public_key(to)?string("ksend"):string("withdraw");
         if(operation!="ksend")call(get_account_func,fc::mutable_variant_object("account_name",name(to)));
         auto quantity=to_asset(name(token),amount);auto cap=fee_cap(maximum);
         fc::variant request=fc::mutable_variant_object("from",key)("to",operation=="ksend"?fc::variant(k1(to)):fc::variant(name(to)))
            ("token_contract",name(token))("quantity",quantity)("memo",memo)("nonce",std::to_string(source["next_nonce"].as_uint64()))
            ("expires",fc::time_point_sec(get_info().head_block_time).sec_since_epoch()+valid_seconds)
            ("relayer",bp)("reward_to",bp)("max_fee",cap);
         auto hash=digest(operation,request,cfg);
         std::cout<<fc::json::to_pretty_string(fc::mutable_variant_object("operation",operation)("request",request)
            ("chain_id",cfg["chain_id"])("deployment",cfg["deployment"])("digest",hash))<<std::endl;
      });
      auto sign=root->add_subcommand("sign","Sign prepared intent with the sender key in local keosd; outputs relay JSON");
      sign->add_option("prepared",input,"Prepared JSON file or JSON text")->required();
      sign->callback([this]{
         auto body=json_from_file_or_string(input);auto cfg=configuration();
         FC_ASSERT(body.get_object().size()==5&&body["chain_id"]==cfg["chain_id"]&&body["deployment"]==cfg["deployment"],"Prepared intent domain does not match chain");
         auto op=body["operation"].as_string();auto hash=digest(op,body["request"],cfg);
         FC_ASSERT(hash==body["digest"].as<digest_type>(),"Prepared digest changed; review and prepare again");
         auto sig=owner_signature(hash,k1(body["request"]["from"].as_string()));
         std::cout<<fc::json::to_pretty_string(fc::mutable_variant_object("operation",op)("request",body["request"])("signature",sig))<<std::endl;
      });
      auto push=root->add_subcommand("push","Push signed intent using BP child permission, or --payer for self withdrawal");
      push->add_option("signed-intent",input,"Owner-signed JSON file or JSON text")->required();
      push->add_option("--payer",payer,"Native transaction signer for selfout/selfcancel");
      add_standard_transaction_options_plus_signing(push,"selected-bp@keyrelay or payer@active");
      push->callback([this]{
         auto body=json_from_file_or_string(input);auto cfg=configuration();auto op=body["operation"].as_string();
         FC_ASSERT(body.get_object().size()==3,"Expected operation/request/signature only");
         auto hash=digest(op,body["request"],cfg);auto key=k1(body["request"]["from"].as_string());
         FC_ASSERT(public_key_type(body["signature"].as<signature_type>(),hash)==key,"Invalid owner signature");
         fc::mutable_variant_object data("request",body["request"]);data("sig",body["signature"]);
         vector<permission_level> auth;
         if(op=="selfout"||op=="selfcancel"){
            FC_ASSERT(!payer.empty(),"Self operation requires --payer");data("payer",name(payer));
            auth=get_account_permissions(tx_permission,{name(payer),"active"_n});
         }else{
            FC_ASSERT(payer.empty()&&!tx_permission.empty(),"Commercial push requires explicit -p selected-bp@registered-child-permission");
            auth=get_account_permissions(tx_permission);
         }
         send_actions({create_action(auth,account,name(op),data)},signing_keys_opt.get_keys());
      });
      auto submit=root->add_subcommand("submit","Send signed intent to an HTTPS BP relay; never uploads a private key");
      submit->add_option("signed-intent",input,"Owner-signed JSON file or JSON text")->required();
      submit->add_option("--relay-url",relay_url,"Registered BP relay HTTPS URL")->required();
      submit->add_flag("--allow-http-for-test",allow_http,"Allow loopback HTTP in an isolated test only");
      submit->callback([this]{
         auto body=json_from_file_or_string(input);auto cfg=configuration();auto op=body["operation"].as_string();
         FC_ASSERT(body.get_object().size()==3&&(op=="ksend"||op=="withdraw"||op=="cancel"),"Commercial relay JSON required");
         auto hash=digest(op,body["request"],cfg);
         FC_ASSERT(public_key_type(body["signature"].as<signature_type>(),hash)==k1(body["request"]["from"].as_string()),"Invalid owner signature");
         FC_ASSERT(relay_url.starts_with("https://")||(allow_http&&(relay_url.starts_with("http://127.0.0.1:")||relay_url.starts_with("http://localhost:"))),"Use HTTPS, or explicit loopback HTTP test mode");
         while(!relay_url.empty()&&relay_url.back()=='/')relay_url.pop_back();
         std::cout<<fc::json::to_pretty_string(call(relay_url,"/submit",body))<<std::endl;
      });
   }
};
}
