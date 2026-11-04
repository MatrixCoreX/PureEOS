const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{createRequire}=require('node:module');
const K='/opt/pureeos-dev-proposals-20261104-r1',R='/opt/pureeos-keynet-20261103-r2',S=K+'/frontend/dist';
const {chromium}=createRequire(R+'/history-source/package.json')('playwright'),phase=process.argv[2]||'preview';
const origin='https://aic-test.matrixai.one',mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'};
(async()=>{
 const browser=await chromium.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage']}),variants=[],errors=[];
 const out=K+'/browser-'+phase;fs.mkdirSync(out,{recursive:true});let current;
 try{
  for(const v of [{language:'zh',theme:'dark',width:1440},{language:'zh',theme:'light',width:390},{language:'en',theme:'dark',width:390},{language:'en',theme:'light',width:1440}]){
   const context=await browser.newContext({viewport:{width:v.width,height:1100}});context.setDefaultTimeout(60000);
   await context.addInitScript(v=>{localStorage.setItem('pureeos-explorer.language.v1',v.language);localStorage.setItem('pureeos-explorer.theme.v1',v.theme);},v);
   const page=current=await context.newPage();page.on('pageerror',e=>errors.push(String(e)));
   if(phase==='preview')await page.route(origin+'/**',async route=>{
    const u=new URL(route.request().url());if(/^\/(api|rpc|history|relay)(\/|$)/.test(u.pathname))return route.continue();
    const p=path.resolve(S,'.'+(u.pathname==='/'?'/index.html':decodeURIComponent(u.pathname)));assert(p.startsWith(S+'/'));
    if(fs.existsSync(p)&&fs.statSync(p).isFile())return route.fulfill({status:200,contentType:mime[path.extname(p)]||'application/octet-stream',body:fs.readFileSync(p)});
    return route.continue();
   });
   await page.goto(origin+'/#/upgrades',{waitUntil:'domcontentloaded',timeout:60000});
   const panel=page.locator('.developer-proposals');await panel.locator('.upgrade-heading strong').waitFor();
   const title=await panel.locator('.upgrade-heading strong').innerText(),badge=await panel.locator('.badge').first().innerText();
   assert(/#\d+/.test(title),title);
   const details=panel.locator('.upgrade-details');assert.equal(await details.getAttribute('open'),null);
   await details.locator('summary').click();await details.locator('code').first().waitFor({state:'visible'});
   const hashes=await details.locator('code').allInnerTexts();assert(hashes.length>=5&&hashes.every(h=>/^[a-f\d]{64}$/.test(h)));
   if(v.language==='en')assert(!/[\u4e00-\u9fff]/.test(await panel.innerText()),'Untranslated proposal text');
   await details.locator('summary').click();assert.equal(await details.getAttribute('open'),null);
   const layout=await page.evaluate(()=>({width:document.documentElement.scrollWidth,viewport:innerWidth}));assert(layout.width<=v.width+1,JSON.stringify(layout));
   await panel.screenshot({path:out+'/'+v.language+'-'+v.theme+'-'+v.width+'.png'});
   if(v.language==='en'&&v.width===1440){
    await page.route('**/v1/chain/get_upgrade_state',route=>route.fulfill({status:503,contentType:'application/json',body:'{"message":"UPGRADE_STATE_UNAVAILABLE"}'}));
    await panel.getByRole('button',{name:'Refresh developer proposal'}).click();await panel.getByText('UPGRADE_STATE_UNAVAILABLE',{exact:true}).waitFor();
    assert.equal(await panel.locator('.upgrade-heading').count(),0);await page.unroute('**/v1/chain/get_upgrade_state');
    await panel.getByRole('button',{name:'Refresh developer proposal'}).click();await panel.locator('.upgrade-heading').waitFor();
   }
   let feeCases=0;
   if(phase==='public-after'){
    assert.equal(hashes[1],'bc3358d79a64a42fcef367079ce77bcc261b51a39ff73481880d7f5bbf35b6b1');assert.equal(hashes.at(-1),hashes[1]);assert(/Closed|已结束/.test(badge));
    await page.goto(origin+'/#/',{waitUntil:'domcontentloaded'});const fees=page.locator('.key-fee-estimate');await fees.getByTestId('fee-base').filter({hasText:/AIC/}).waitFor();
    const formula=fees.locator('details');assert.equal(await formula.getAttribute('open'),null);await formula.locator('summary').click();const text=await formula.innerText();assert(text.includes('1536')&&text.includes('512')&&text.includes('0.5%'));await formula.locator('summary').click();
    const atoms=s=>BigInt(s.replace(' AIC','').replace('.','')),relay=fees.getByLabel(v.language==='zh'?'收费中继':'Relay operator'),scenario=fees.getByLabel(v.language==='zh'?'收款存储场景':'Recipient storage scenario');
    const options=await relay.locator('option').evaluateAll(xs=>xs.map(x=>({bp:x.value,text:x.textContent})));assert.equal(options.length,5);assert.equal(options.filter(x=>/Inactive|未启用/.test(x.text)).length,4);
    for(const option of options){await relay.selectOption(option.bp);const m=option.text.match(/ · (\d+)\.(\d{2})%/),rate=BigInt(m[1])*100n+BigInt(m[2]);for(const rows of ['0','1','2']){
     await scenario.selectOption(rows);const base=atoms(await fees.getByTestId('fee-base').innerText()),storage=atoms(await fees.getByTestId('fee-storage').innerText()),service=atoms(await fees.getByTestId('fee-relay').innerText());
     assert.equal(service,(base*rate+9999n)/10000n);assert.equal(atoms(await fees.getByTestId('fee-total').innerText()),base+service);assert.equal(atoms(await fees.getByTestId('fee-execution').innerText())+storage+atoms(await fees.getByTestId('fee-supplement').innerText()),base);
     assert((await fees.locator('.fee-ram').innerText()).includes(String([0,512,1536][Number(rows)])));if(rows==='0')assert.equal(storage,0n);feeCases++;
    }}
    await relay.selectOption('gptgenesisbp');await scenario.selectOption('2');if(v.language==='en')assert(!/[\u4e00-\u9fff]/.test(await fees.innerText()));assert.equal(await formula.getAttribute('open'),null);await fees.screenshot({path:out+'/fees-'+v.language+'-'+v.theme+'-'+v.width+'.png'});
   }
   variants.push({...v,title,badge,hashes,layout,default_hashes_collapsed:true,actual_market_fee_cases:feeCases});await context.close();
  }
  assert.deepEqual(errors,[]);fs.writeFileSync(out+'/RESULT.json',JSON.stringify({status:'PASS',phase,variants,errors,actual_native_RPC:true,read_only:true,UTC:new Date().toISOString(),build_files:JSON.parse(fs.readFileSync(K+'/FRONTEND_RESULT.json')).files},null,2)+'\n');console.log('DEV PROPOSAL BROWSER PASS '+phase);
 }catch(e){fs.writeFileSync(out+'/FAILURE-'+Date.now()+'.json',JSON.stringify({error:String(e),errors,variants,body:await current?.locator('body').innerText().catch(()=>'' )},null,2));throw e;}
 finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
