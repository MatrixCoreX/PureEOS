const fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('/opt/pureeos-keynet-20261103-r2/history-source/node_modules/playwright');
const R='/opt/pureeos-bp-demo-six-20261105-r1',origin='https://aic-test.matrixai.one';
const plan=JSON.parse(fs.readFileSync(R+'/PLAN.json'));
(async()=>{
 const browser=await chromium.launch({executablePath:'/opt/pureeos-key-history-20261103-r1/tools/browsers/chromium_headless_shell-1223/chrome-headless-shell-linux64/chrome-headless-shell',headless:true,args:['--no-sandbox','--disable-dev-shm-usage']});const variants=[];
 try{
  for(const [language,theme,width]of [['zh','dark',1440],['en','light',390]]){
   const context=await browser.newContext({viewport:{width,height:1100}});
   await context.addInitScript(({language,theme})=>{localStorage.setItem('pureeos-explorer.language.v1',language);localStorage.setItem('pureeos-explorer.theme.v1',theme)},{language,theme});
   const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(String(e)));
   await page.goto(origin+'/#/producers',{waitUntil:'domcontentloaded',timeout:60000});
   const row=page.locator('.producer-list-table tbody tr').filter({has:page.locator(`a[href="#/account/${plan.maintenance_target}"]`)});
   await row.getByText(language==='zh'?'维护中':'In maintenance',{exact:true}).waitFor({timeout:120000});
   await page.waitForFunction(()=>!document.querySelector('.notice'),null,{timeout:120000});
   const summary=await page.evaluate(async()=>{const r=await fetch('/history/v2/state/get_producer_summary',{cache:'no-store',credentials:'omit'});if(!r.ok)throw Error('Public summary HTTP'+r.status);return r.json()});
   assert.equal(summary.chain_id,plan.chain_id);assert.equal(summary.available,true);assert.equal(summary.registered_bp_count,30);assert.equal(summary.eligible_bp_count,29);assert.equal(summary.permanent_blacklist_count,0);assert.equal(summary.maintenance_active_count,1);assert.equal(summary.maintenance_pending_count,0);
   const text=await row.innerText();assert(/10,?000\.0000 AIC/.test(text),text);assert.equal(errors.length,0,JSON.stringify(errors));
   assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));
   const screenshot=`immediate-maintenance-public-${language}-${theme}-${width}.png`;await page.screenshot({path:R+'/'+screenshot,fullPage:true});
   variants.push({language,theme,width,producer:plan.maintenance_target,row:text,summary,errors,visibleNotices:await page.locator('.notice').allTextContents(),screenshot});await context.close();
  }
  fs.writeFileSync(R+'/MAINTENANCE_PUBLIC_BROWSER.json',JSON.stringify({status:'PASS',UTC:new Date().toISOString(),origin,variants,scope:'Actual normal public HTTPS Chrome, desktop Chinese dark/mobile English light, immediate native maintenance state and unchanged bond shown. Five fault targets still await native duties.'},null,2)+'\n');
 }catch(e){fs.writeFileSync(R+'/MAINTENANCE_PUBLIC_BROWSER_FAILURE.json',JSON.stringify({status:'FAIL',error:String(e),variants},null,2)+'\n');throw e}finally{await browser.close()}
})().catch(e=>{console.error(e);process.exitCode=1});
