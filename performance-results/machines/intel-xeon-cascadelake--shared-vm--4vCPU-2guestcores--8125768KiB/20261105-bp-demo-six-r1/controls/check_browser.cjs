const fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('/opt/pureeos-keynet-20261103-r2/history-source/node_modules/playwright');
const R='/opt/pureeos-bp-demo-six-20261105-r1',pub=process.env.PUREEOS_PUBLIC==='1',demo=process.env.PUREEOS_FINAL_DEMO==='1';
const origin=pub?'https://aic-test.matrixai.one':'http://127.0.0.1:9078',phase=demo?'DEMO_PUBLIC':pub?'CAPACITY_PUBLIC':'CAPACITY_PREVIEW';
const plan=JSON.parse(fs.readFileSync(R+'/PLAN.json'));
const utc=value=>new Date(value).toISOString().replace('T',' ').replace('Z',' UTC');
(async()=>{const browser=await chromium.launch({executablePath:'/opt/pureeos-key-history-20261103-r1/tools/browsers/chromium_headless_shell-1223/chrome-headless-shell-linux64/chrome-headless-shell',headless:true,args:['--no-sandbox','--disable-dev-shm-usage']});const variants=[];
try{for(const[language,theme,width]of[['zh','dark',1440],['en','light',1440],['zh','light',390],['en','dark',390]]){
 const context=await browser.newContext({viewport:{width,height:1100}});
 await context.addInitScript(({language,theme,pub})=>{localStorage.setItem('pureeos-explorer.language.v1',language);localStorage.setItem('pureeos-explorer.theme.v1',theme);if(!pub)localStorage.setItem('pureeos-explorer.node.v1',JSON.stringify({rpc:location.origin+'/api',history:location.origin+'/history',historyExplicit:true,chainId:'09f10e504b67f8c6fd519c8d310c8285172c1c603461d97e612ba0f6380de532'}))},{language,theme,pub});
 const page=await context.newPage(),errors=[];page.on('pageerror',e=>errors.push(String(e)));
 await page.goto(origin+'/#/producers',{waitUntil:'domcontentloaded',timeout:60000});
 const card=page.locator('.producer-stats-grid .stat').filter({has:page.locator('.stat-label').filter({hasText:language==='zh'?'允许轮岗节点上限':'Allowed rotation capacity'})});
 await card.locator('.stat-detail').filter({hasText:language==='zh'?'下次增加：':'Next increase:'}).waitFor({timeout:120000});
 const summary=await page.evaluate(async()=>{let r=await fetch('/history/v2/state/get_producer_summary',{cache:'no-store',credentials:'omit'});if(!r.ok)throw Error('Summary HTTP'+r.status);return r.json()});
 assert.equal(summary.chain_id,plan.chain_id);assert.equal(summary.available,true);assert(summary.next_capacity_estimated_at);
 const expected=(language==='zh'?'下次增加：':'Next increase: ')+utc(summary.next_capacity_estimated_at);
 assert.equal((await card.locator('.stat-detail').innerText()).trim(),expected);assert.equal((await card.locator('.stat-value').innerText()).replace(/,/g,''),String(summary.rotation_capacity));
 assert(!(await page.locator('body').innerText()).includes(language==='zh'?'链上候选容量，不是已上线数量':'On-chain candidate capacity, not online node count'));
 const geometry=await card.evaluate(el=>{const d=el.querySelector('.stat-detail');return {height:el.getBoundingClientRect().height,detailHeight:d.clientHeight,detailScrollHeight:d.scrollHeight,width:el.getBoundingClientRect().width,detailWidth:d.clientWidth,detailScrollWidth:d.scrollWidth}});
 assert.equal(Math.round(geometry.height),120);assert(geometry.detailScrollHeight<=geometry.detailHeight+1,JSON.stringify(geometry));assert(geometry.detailScrollWidth<=geometry.detailWidth+1,JSON.stringify(geometry));
 await page.locator('.producer-list-table tbody tr').first().waitFor({timeout:90000});
 const rows=[];
 if(demo){
  await page.waitForFunction(()=>{
   const rows=[...document.querySelectorAll('.producer-list-table tbody tr')];
   return document.querySelectorAll('.notice').length===0&&rows.length===30&&rows.every(row=>{
    const cells=row.querySelectorAll('td');
    return cells.length===9&&[2,3,4,7].every(i=>!/(未取得|Not obtained|Unavailable)/i.test(cells[i].textContent));
   });
  },null,{timeout:120000});
  assert.equal(summary.registered_bp_count,30);assert.equal(summary.eligible_bp_count,24);assert.equal(summary.permanent_blacklist_count,5);assert.equal(summary.maintenance_active_count,1);assert.equal(summary.maintenance_pending_count,0);
  for(const name of plan.fault_targets){const row=page.locator('.producer-list-table tbody tr').filter({has:page.locator(`a[href="#/account/${name}"]`)});await row.getByText(language==='zh'?'永久黑名单':'Permanent blacklist',{exact:true}).waitFor({timeout:120000});const text=await row.innerText();assert(text.includes('10,000.0000 AIC')||text.includes('10000.0000 AIC'));rows.push({name,text});}
  const row=page.locator('.producer-list-table tbody tr').filter({has:page.locator(`a[href="#/account/${plan.maintenance_target}"]`)});await row.getByText(language==='zh'?'维护中':'In maintenance',{exact:true}).waitFor({timeout:120000});rows.push({name:plan.maintenance_target,text:await row.innerText()});
 }
 assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));assert.equal(errors.length,0,JSON.stringify(errors));
 await page.screenshot({path:`${R}/${phase.toLowerCase()}-${language}-${theme}-${width}.png`,fullPage:true});
 variants.push({language,theme,width,expectedCaption:expected,summary,geometry,rows,errors,visibleNotices:await page.locator('.notice').allTextContents()});await context.close();
}fs.writeFileSync(R+'/'+phase+'.json',JSON.stringify({status:'PASS',UTC:new Date().toISOString(),origin,scope:'Real remote Chrome with actual current-chain read APIs; four language/theme/width variants, no fabricated data.',variants},null,2)+'\n');}
catch(e){fs.writeFileSync(R+'/'+phase+'_FAILURE.json',JSON.stringify({status:'FAIL',error:String(e),variants},null,2)+'\n');throw e}finally{await browser.close()}})().catch(e=>{console.error(e);process.exitCode=1});
