const {createRequire}=require('node:module'),fs=require('node:fs'),assert=require('node:assert/strict');
const K='/opt/pureeos-key-explorer-20261103-r1',E='/opt/pureeos-keynet-public-qa-20261103-r1';
const {chromium}=createRequire(K+'/browser-tools/package.json')('playwright');
const {PublicKey,PrivateKey}=createRequire(K+'/source/package.json')('@wharfkit/antelope');
const data=JSON.parse(fs.readFileSync(E+'/PUBLIC_DATA.json')),origin='https://aic-test.matrixai.one';
(async()=>{
 const browser=await chromium.launch({headless:true,args:['--no-sandbox','--disable-dev-shm-usage']});
 const errors=[],variants=[];let lastPage;fs.mkdirSync(E+'/screenshots',{recursive:true});
 try{
  for(const v of [{language:'zh',theme:'dark',width:1440},{language:'zh',theme:'light',width:390},{language:'en',theme:'dark',width:390},{language:'en',theme:'light',width:1440}]){
   const context=await browser.newContext({viewport:{width:v.width,height:1000},userAgent:'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36'});
   await context.addInitScript(v=>{localStorage.clear();localStorage.setItem('pureeos-explorer.language.v1',v.language);localStorage.setItem('pureeos-explorer.theme.v1',v.theme);},v);
   const page=lastPage=await context.newPage();page.on('pageerror',error=>errors.push(String(error)));
   for(const holding of data.holdings){
    const modern=PublicKey.from(holding.key).toString(),legacy=PublicKey.from(holding.key).toLegacyString();
    await page.goto(origin+'/#/key/'+legacy);
    for(const balance of holding.balances)await page.getByText(balance.display,{exact:true}).waitFor({timeout:60000});
    await page.locator('.top-search input').fill(legacy);await page.locator('.top-search input').press('Enter');
    await page.waitForURL(url=>url.hash.includes(modern));
    for(const balance of holding.balances)await page.getByText(balance.display,{exact:true}).waitFor({timeout:60000});
   }
   const layout=await page.evaluate(()=>({width:document.documentElement.scrollWidth,viewport:innerWidth,theme:document.documentElement.dataset.theme}));
   assert(layout.width<=v.width+1,JSON.stringify(layout));assert.equal(layout.theme,v.theme);
   await page.screenshot({path:E+'/screenshots/'+v.language+'-'+v.theme+'-'+v.width+'.png',fullPage:true});
   if(v.language==='en'&&v.width===1440){
    await page.goto(origin+'/#/key/'+PrivateKey.generate('K1').toPublic().toString());
    await page.getByText('This public key has no wallet record yet.',{exact:true}).waitFor({timeout:60000});
    assert.equal(await page.getByText('0.0000 AIC',{exact:true}).count(),0);
   }
   variants.push({...v,layout,actual_balances:data.holdings});await context.close();
  }
  assert.equal(errors.length,0,JSON.stringify(errors));
  fs.writeFileSync(E+'/BROWSER_RESULT.json',JSON.stringify({status:'PASS',chain_id:data.chain_id,origin,variants,errors,unknown_key_not_fake_zero:true,source:'Actual public AIC/USD ledger after real HTTPS cleos relay payment'},null,2));
  console.log('PUBLIC LIVE AIC/USD BROWSER PASS: '+variants.length+' variants');
 }catch(error){
  fs.writeFileSync(E+'/BROWSER_FAILURE.json',JSON.stringify({error:String(error),errors,variants,body:await lastPage?.locator('body').innerText().catch(()=> '')},null,2));
  if(lastPage)await lastPage.screenshot({path:E+'/screenshots/failure.png',fullPage:true}).catch(()=>{});
  throw error;
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
