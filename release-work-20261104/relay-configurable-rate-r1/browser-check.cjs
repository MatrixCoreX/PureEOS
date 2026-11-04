const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict'),{createRequire}=require('node:module');
const K='/opt/pureeos-explorer-key-rate-20261104-r3',R='/opt/pureeos-keynet-20261103-r2',S=K+'/source/dist';
const {chromium}=createRequire(R+'/history-source/package.json')('playwright'),phase=process.argv[2]||'preview';
const origin='https://aic-test.matrixai.one',mime={'.html':'text/html','.js':'text/javascript','.css':'text/css','.svg':'image/svg+xml'};
const money=n=>{n=String(n).padStart(5,'0');return n.slice(0,-4)+'.'+n.slice(-4)+' AIC';};
const atoms=s=>{assert(/^\d+\.\d{4} AIC$/.test(s),s);return BigInt(s.replace(' AIC','').replace('.',''));};
(async()=>{
 const browser=await chromium.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage']});
 const variants=[],errors=[],out=K+'/browser-'+phase;fs.mkdirSync(out,{recursive:true});let currentPage;
 try {
  for(const v of [{language:'zh',theme:'dark',width:1440},{language:'zh',theme:'light',width:390},{language:'en',theme:'dark',width:390},{language:'en',theme:'light',width:1440}]){
   const context=await browser.newContext({viewport:{width:v.width,height:1100}});context.setDefaultTimeout(60000);
   await context.addInitScript(v=>{localStorage.setItem('pureeos-explorer.language.v1',v.language);localStorage.setItem('pureeos-explorer.theme.v1',v.theme);},v);
   const page=currentPage=await context.newPage();page.on('pageerror',e=>errors.push(String(e)));
   if(phase==='preview')await page.route(origin+'/**',async route=>{
    const u=new URL(route.request().url());if(/^\/(api|rpc|history|relay)(\/|$)/.test(u.pathname))return route.continue();
    const p=path.resolve(S,'.'+(u.pathname==='/'?'/index.html':decodeURIComponent(u.pathname)));assert(p.startsWith(S+'/'));
    if(fs.existsSync(p)&&fs.statSync(p).isFile())return route.fulfill({status:200,contentType:mime[path.extname(p)]||'application/octet-stream',body:fs.readFileSync(p)});
    return route.continue();
   });
   await page.goto(origin+'/#/',{waitUntil:'domcontentloaded',timeout:60000});
   const panel=page.locator('.key-fee-estimate');await panel.getByTestId('fee-base').filter({hasText:/AIC/}).waitFor();
   const intro=await panel.locator('.fee-estimate-intro').innerText();
   assert(intro.startsWith(v.language==='zh'?'直接用公钥作为收付款账号':'Use your public key as your payment account'));
   const formula=await panel.locator('.fee-formulas').innerText();assert(formula.includes('10%')&&formula.includes('30%')&&formula.includes('0.0001 AIC'));
   if(v.language==='en')assert(!/[\u4e00-\u9fff]/.test(await panel.innerText()),'Untranslated panel text');
   const selector=panel.getByLabel(v.language==='zh'?'收费中继':'Relay operator');
   assert.equal(await selector.inputValue(),'gptgenesisbp','Default must be the active founder');
   const options=await selector.locator('option').evaluateAll(xs=>xs.map(x=>({bp:x.value,text:x.textContent})));
   assert.equal(options.length,phase==='public'?5:1);
   if(phase==='public')assert.equal(options.filter(x=>/未启用|Inactive/.test(x.text)).length,4);
   let checked=0;
   for(const option of options){
    const m=option.text.match(/ · (\d+)\.(\d{2})%/);assert(m,option.text);
    const rate=BigInt(m[1])*100n+BigInt(m[2]);await selector.selectOption(option.bp);
    const inactive=/未启用|Inactive/.test(option.text);
    if(inactive)await panel.getByText(v.language==='zh'?'该中继尚未启用，当前报价仅供费用比较。':'This relay is inactive. This quote is for fee comparison only.',{exact:true}).waitFor();
    for(const rows of ['0','1','2']){
     await panel.getByLabel(v.language==='zh'?'收款存储场景':'Recipient storage scenario').selectOption(rows);
     await panel.getByLabel(v.language==='zh'?'备注长度 / UTF-8 字节':'Memo length / UTF-8 bytes').fill('65');
     const base=atoms(await panel.getByTestId('fee-base').innerText()),service=(base*rate+9999n)/10000n;
     assert.equal(await panel.getByTestId('fee-relay').innerText(),money(service));
     assert.equal(await panel.getByTestId('fee-total').innerText(),money(base+service));checked++;
    }
   }
   await selector.selectOption('gptgenesisbp');
   await panel.getByLabel(v.language==='zh'?'收款存储场景':'Recipient storage scenario').selectOption('0');
   await panel.getByLabel(v.language==='zh'?'备注长度 / UTF-8 字节':'Memo length / UTF-8 bytes').fill('0');
   await panel.scrollIntoViewIfNeeded();
   const layout=await page.evaluate(()=>({width:document.documentElement.scrollWidth,viewport:innerWidth}));assert(layout.width<=v.width+1,JSON.stringify(layout));
   await page.screenshot({path:out+'/'+v.language+'-'+v.theme+'-'+v.width+'.png',fullPage:true});
   if(v.language==='en'&&v.width===1440){
    await page.route('**/v1/chain/get_table_rows',async route=>{
     if(route.request().postDataJSON()?.table==='powup.state')return route.fulfill({status:503,contentType:'application/json',body:JSON.stringify({message:'FEE_MARKET_UNAVAILABLE'})});
     return route.continue();
    });
    await panel.getByRole('button',{name:'Refresh fee estimate',exact:true}).click();await panel.getByText('FEE_MARKET_UNAVAILABLE',{exact:true}).waitFor();
    assert.equal(await panel.getByTestId('fee-total').innerText(),'—');assert.equal(await panel.getByTestId('fee-base').innerText(),'—');
    await page.unroute('**/v1/chain/get_table_rows');await panel.getByRole('button',{name:'Refresh fee estimate',exact:true}).click();
    await panel.getByTestId('fee-base').filter({hasText:/AIC/}).waitFor();
   }
   variants.push({...v,layout,operators:options,formula_cases:checked});await context.close();
  }
  assert.deepEqual(errors,[]);
  fs.writeFileSync(out+'/RESULT.json',JSON.stringify({status:'PASS',phase,UTC:new Date().toISOString(),variants,errors,failure_clears_quote:true,recovery:true,build_files:JSON.parse(fs.readFileSync(K+'/BUILD_RESULT.json')).files},null,2)+'\n');
  console.log('BROWSER PASS '+phase+' 4 variants');
 } catch(e) {
  fs.writeFileSync(out+'/FAILURE-'+Date.now()+'.json',JSON.stringify({error:String(e),errors,variants,body:await currentPage?.locator('body').innerText().catch(()=>'' )},null,2));throw e;
 } finally {await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
