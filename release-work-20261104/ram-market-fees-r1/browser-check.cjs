// Actual remote Chrome, compiled preview, public read-only RPC. market-fixture
// overrides ONLY the wallet code-hash response to exercise the unpublished
// candidate rule; its RAM reserves and all other chain data are read live.
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{createRequire}=require('node:module');
const K='/opt/pureeos-key-ram-market-20261104-r1',R='/opt/pureeos-keynet-20261103-r2',S=K+'/frontend/dist';
const {chromium}=createRequire(R+'/history-source/package.json')('playwright');
const candidate='bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1';
const origin='https://aic-test.matrixai.one',mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'};
const atoms=s=>{assert(/^\d+\.\d{4} AIC$/.test(s),s);return BigInt(s.replace(' AIC','').replace('.',''));};
const money=n=>`${n/10000n}.${String(n%10000n).padStart(4,'0')} AIC`;
(async()=>{
 const browser=await chromium.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage']}),variants=[],errors=[];
 const out=K+'/browser';fs.mkdirSync(out,{recursive:true});let current;
 try{
  for(const profile of ['fixed','market-fixture'])for(const v of [{language:'zh',theme:'dark',width:1440},{language:'zh',theme:'light',width:390},{language:'en',theme:'dark',width:390},{language:'en',theme:'light',width:1440}]){
   const context=await browser.newContext({viewport:{width:v.width,height:1100}});context.setDefaultTimeout(60000);
   await context.addInitScript(v=>{localStorage.setItem('pureeos-explorer.language.v1',v.language);localStorage.setItem('pureeos-explorer.theme.v1',v.theme);},v);
   const page=current=await context.newPage();page.on('pageerror',e=>errors.push(String(e)));
   await page.route(origin+'/**',async route=>{
    const u=new URL(route.request().url());if(/^\/(api|rpc|history|relay)(\/|$)/.test(u.pathname))return route.continue();
    const p=path.resolve(S,'.'+(u.pathname==='/'?'/index.html':decodeURIComponent(u.pathname)));assert(p.startsWith(S+'/'));
    if(fs.existsSync(p)&&fs.statSync(p).isFile())return route.fulfill({status:200,contentType:mime[path.extname(p)]||'application/octet-stream',body:fs.readFileSync(p)});
    return route.continue();
   });
   if(profile==='market-fixture')await page.route('**/v1/chain/get_code_hash',async route=>{
    if(route.request().postDataJSON()?.account_name==='eosio.key')return route.fulfill({status:200,contentType:'application/json',body:JSON.stringify({account_name:'eosio.key',code_hash:candidate})});
    return route.continue();
   });
   await page.goto(origin+'/#/',{waitUntil:'domcontentloaded',timeout:60000});
   const panel=page.locator('.key-fee-estimate');await panel.getByTestId('fee-base').filter({hasText:/AIC/}).waitFor();
   const details=panel.locator('details');assert.equal(await details.getAttribute('open'),null);
   const collapsed=(await panel.boundingBox()).height;await details.locator('summary').click();
   const explanation=await details.innerText();assert(explanation.includes('10%')&&explanation.includes('30%')&&explanation.includes('0.0001 AIC'));
   if(profile==='market-fixture')assert(explanation.includes('1536')&&explanation.includes('512')&&explanation.includes('0.5%'));
   if(v.language==='en')assert(!/[\u4e00-\u9fff]/.test(await panel.innerText()),'Untranslated text');
   assert((await panel.boundingBox()).height>collapsed);await details.locator('summary').click();
   const relay=panel.getByLabel(v.language==='zh'?'收费中继':'Relay operator'),scenario=panel.getByLabel(v.language==='zh'?'收款存储场景':'Recipient storage scenario');
   assert.equal(await relay.inputValue(),'gptgenesisbp');
   const options=await relay.locator('option').evaluateAll(xs=>xs.map(x=>({bp:x.value,text:x.textContent})));
   assert.equal(options.length,5);assert.equal(options.filter(o=>/未启用|Inactive/.test(o.text)).length,4);
   let cases=0;
   for(const option of options){
    await relay.selectOption(option.bp);const m=option.text.match(/ · (\d+)\.(\d{2})%/),rate=BigInt(m[1])*100n+BigInt(m[2]);
    for(const rows of ['0','1','2']){
     await scenario.selectOption(rows);
     const base=atoms(await panel.getByTestId('fee-base').innerText()),storage=atoms(await panel.getByTestId('fee-storage').innerText());
     assert.equal(await panel.getByTestId('fee-relay').innerText(),money((base*rate+9999n)/10000n));
     assert.equal(await panel.getByTestId('fee-total').innerText(),money(base+(base*rate+9999n)/10000n));
     assert.equal(atoms(await panel.getByTestId('fee-execution').innerText())+storage+atoms(await panel.getByTestId('fee-supplement').innerText()),base);
     if(rows==='0')assert.equal(storage,0n);
     if(profile==='market-fixture')assert((await panel.locator('.fee-ram').innerText()).includes(String([0,512,1536][Number(rows)])));
     cases++;
    }
   }
   await relay.selectOption('gptgenesisbp');await scenario.selectOption('2');await panel.scrollIntoViewIfNeeded();
   const layout=await page.evaluate(()=>({width:document.documentElement.scrollWidth,viewport:innerWidth}));assert(layout.width<=v.width+1,JSON.stringify(layout));
   await panel.screenshot({path:out+'/'+profile+'-'+v.language+'-'+v.theme+'-'+v.width+'.png'});
   if(profile==='market-fixture'&&v.language==='en'&&v.width===1440){
    await page.route('**/v1/chain/get_table_rows',async route=>{
     if(route.request().postDataJSON()?.table==='rammarket')return route.fulfill({status:503,contentType:'application/json',body:'{"message":"RAM_MARKET_UNAVAILABLE"}'});
     return route.continue();
    });
    await panel.getByRole('button',{name:'Refresh fee estimate',exact:true}).click();await panel.getByText('RAM_MARKET_UNAVAILABLE',{exact:true}).waitFor();
    assert.equal(await panel.getByTestId('fee-total').innerText(),'—');await page.unroute('**/v1/chain/get_table_rows');
    await panel.getByRole('button',{name:'Refresh fee estimate',exact:true}).click();await panel.getByTestId('fee-base').filter({hasText:/AIC/}).waitFor();
   }
   variants.push({profile,...v,cases,layout,default_collapsed:true});await context.close();
  }
  assert.deepEqual(errors,[]);fs.writeFileSync(out+'/RESULT.json',JSON.stringify({status:'PASS',variants,errors,scope:'Compiled local preview in remote real Chrome; live read-only RPC. market-fixture substitutes the unpublished candidate code hash only; not public activation proof.',UTC:new Date().toISOString()},null,2)+'\n');
  console.log('BROWSER PASS 8 variants');
 }catch(error){fs.writeFileSync(out+'/FAILURE-'+Date.now()+'.json',JSON.stringify({error:String(error),errors,variants,body:await current?.locator('body').innerText().catch(()=>'' )},null,2));throw error;}
 finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
