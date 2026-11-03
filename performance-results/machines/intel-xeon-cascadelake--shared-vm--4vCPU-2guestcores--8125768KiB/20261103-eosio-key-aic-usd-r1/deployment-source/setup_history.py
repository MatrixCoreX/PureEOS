"""Install a complete, separately credentialed PureHistory stack on the TEST VM."""
from pathlib import Path
import base64
import hashlib
import json
import os
import secrets
import subprocess
import tarfile
import time
import urllib.request

R = Path('/opt/pureeos-keynet-20261103-r1')
os.umask(0o077)
templates = json.loads((R / 'HISTORY_TEMPLATES.json').read_text())
source = R / 'history-source'
assert source.exists() and (source/'build/api/server.js').exists(), 'Stage existing verified history build first'
archive = Path('/opt/pureeos-test45-20261102-r1/history-source-private.tar.gz')
passwords = {name:secrets.token_urlsafe(30) for name in ['elasticsearch','rabbitmq','redis','mongo']}
(R / 'private/history-passwords.json').write_text(json.dumps(passwords,indent=2)+'\n')
images = {name:values[0] for name,values in templates['backend_images'].items()}
compose = {'name':'pureeos-keynet-history','services':{},'volumes':{}}
specs = {
    'elasticsearch': {'mem_limit':'2g','cpus':2,'environment':{'discovery.type':'single-node','cluster.name':'pureeos-keynet','xpack.security.enabled':'true','xpack.security.http.ssl.enabled':'false','xpack.security.transport.ssl.enabled':'false','ELASTIC_PASSWORD':passwords['elasticsearch'],'ES_JAVA_OPTS':'-Xms768m -Xmx768m -XX:ActiveProcessorCount=2'},'ports':['127.0.0.1:9200:9200'],'volumes':['es-data:/usr/share/elasticsearch/data']},
    'mongo': {'mem_limit':'512m','cpus':1,'environment':{'MONGO_INITDB_ROOT_USERNAME':'hyperion','MONGO_INITDB_ROOT_PASSWORD':passwords['mongo']},'command':['mongod','--wiredTigerCacheSizeGB','0.25'],'ports':['127.0.0.1:27017:27017'],'volumes':['mongo-data:/data/db']},
    'rabbitmq': {'mem_limit':'512m','cpus':1,'environment':{'RABBITMQ_DEFAULT_USER':'hyperion','RABBITMQ_DEFAULT_PASS':passwords['rabbitmq'],'RABBITMQ_DEFAULT_VHOST':'pureeos_hyperion','RABBITMQ_SERVER_ADDITIONAL_ERL_ARGS':'+S 2:2 +A 2'},'ports':['127.0.0.1:5672:5672','127.0.0.1:15672:15672'],'volumes':['rabbit-data:/var/lib/rabbitmq']},
    'redis': {'mem_limit':'256m','cpus':.5,'command':['redis-server','--requirepass',passwords['redis'],'--maxmemory','128mb','--maxmemory-policy','noeviction','--appendonly','yes'],'ports':['127.0.0.1:6379:6379'],'volumes':['redis-data:/data']},
}
for name,spec in specs.items():
    compose['services'][name] = {'image':images[name],'container_name':'pureeos-keynet-history-'+name,'restart':'unless-stopped','logging':{'driver':'json-file','options':{'max-size':'10m','max-file':'3'}},**spec}
for name in ['es-data','mongo-data','rabbit-data','redis-data']:
    compose['volumes'][name] = {}
backend = R / 'history-backends'
backend.mkdir()
(backend / 'compose.json').write_text(json.dumps(compose,indent=2)+'\n')
subprocess.run(['docker','compose','-f',str(backend/'compose.json'),'up','-d'],check=True)
connections = templates['connections']
connections['elasticsearch'].update(host='127.0.0.1:9200',ingest_nodes=['127.0.0.1:9200'],user='elastic',**{'pass':passwords['elasticsearch']})
connections['amqp'].update(host='127.0.0.1:5672',api='127.0.0.1:15672',user='hyperion',vhost='pureeos_hyperion',**{'pass':passwords['rabbitmq']})
connections['redis'].update(host='127.0.0.1',port=6379,password=passwords['redis'])
connections['mongodb'].update(host='127.0.0.1',port=27017,database_prefix='pureeos_keynet_test',user='hyperion',**{'pass':passwords['mongo']})
ports = json.loads((R/'FOUNDER_PORTS.json').read_text())
bootstrap = json.loads((R/'BOOTSTRAP_RESULT.json').read_text())
chain = connections['chains']['aic']
chain.update(name='PureEOS AIC low-cost TEST',chain_id=bootstrap['chain_id'],http='http://127.0.0.1:8890',ship=[{'label':'independent-nonproducing-TEST-history','url':'ws://127.0.0.1:8080'}])
connections['chains'] = {'aic':chain}
connections['alerts'] = {'triggers':{},'providers':{}}
config = templates['chain']
config['api'].update(chain_name='PureEOS AIC TEST',server_name='aic-test.matrixai.one/history',provider_url='https://aic-test.matrixai.one',node_max_old_space_size=384)
config['indexer'].update(start_on=0,stop_on=0,live_reader=True,live_only_mode=False,rewrite=False,purge_queues=False,node_max_old_space_size=512)
config['settings'].update(chain='aic',index_version='v1',es_replicas=0,max_ws_payload_mb=64)
config['scaling'].update(readers=1,ds_queues=1,ds_threads=1,ds_pool_size=1,indexing_queues=1,ad_idx_queues=1,dyn_idx_queues=1,max_autoscale=1,batch_size=100,resume_trigger=500,auto_scale_trigger=5000,block_queue_limit=64,max_queue_limit=1000)
(source/'config/chains').mkdir(parents=True,exist_ok=True)
(source/'config/connections.json').write_text(json.dumps(connections,indent=2)+'\n')
(source/'config/chains/aic.config.json').write_text(json.dumps(config,indent=2)+'\n')
authorization = 'Basic '+base64.b64encode(('elastic:'+passwords['elasticsearch']).encode()).decode()
deadline = time.monotonic()+180
while True:
    try:
        req=urllib.request.Request('http://127.0.0.1:9200/_cluster/health',headers={'Authorization':authorization})
        with urllib.request.urlopen(req,timeout=5) as response:
            health=json.load(response)
        if health['status'] in ['green','yellow']:
            break
    except Exception:
        pass
    assert time.monotonic()<deadline,'Elasticsearch readiness timeout'
    time.sleep(1)
for name,entry,heap in [('indexer','build/indexer/launcher.js',512),('api','build/api/server.js',384)]:
    unit='pureeos-keynet-purehistory-'+name+'-20261103.service'
    text='[Unit]\nDescription=PureHistory TEST '+name+'\nAfter=docker.service network-online.target\n[Service]\nType=simple\nWorkingDirectory='+str(source)+'\nEnvironment=CONFIG_JSON=config/chains/aic.config.json\nEnvironment=PATH='+str(R/'tools/node-current/bin')+':/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\nExecStart='+str(R/'tools/node-current/bin/node')+' --max-old-space-size='+str(heap)+' '+str(source/entry)+'\nRestart=on-failure\nRestartSec=5\nNice=10\nCPUWeight=20\n'+('CPUQuota=100%\n' if name=='indexer' else '')+'LimitNOFILE=65536\nTimeoutStopSec=120\nStandardOutput=append:'+str(R/('history-'+name+'.log'))+'\nStandardError=append:'+str(R/('history-'+name+'.log'))+'\n[Install]\nWantedBy=multi-user.target\n'
    Path('/etc/systemd/system',unit).write_text(text)
    subprocess.run(['systemctl','daemon-reload'],check=True)
    subprocess.run(['systemctl','enable','--now',unit],check=True,capture_output=True)
report={'status':'STARTED','source':'2837894d40fb8e58866fb7c7def62717ad714785','source_SHA256':hashlib.sha256(archive.read_bytes()).hexdigest(),'build_reused_from_verified_old_history':True,'old_indices_reused':False,'chain_id':bootstrap['chain_id'],'backends':images,'settings':{'ES_heap_MiB':768,'api_heap_MiB':384,'indexer_heap_MiB':512,'indexing_workers':1,'API_port':7000,'from_genesis':True,'new_independent_passwords':True},'scope':'Full PureHistory with ES/Mongo/Rabbit/Redis plus API/indexer on same cheapVM; no TPS workload.'}
(R/'PUREHISTORY_DEPLOYMENT.json').write_text(json.dumps(report,indent=2)+'\n')
print('PUREHISTORY_TEST_STACK_STARTED',flush=True)
