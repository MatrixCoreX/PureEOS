"""Bounded synchronized loopback submissions for correctness, never TPS evidence."""
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import threading
from urllib.parse import urlsplit


def push_packed_wave(url, transactions):
    endpoint=urlsplit(url)
    if (len(transactions)!=4 or endpoint.scheme!='http' or endpoint.hostname!='127.0.0.1'
        or endpoint.port is None or endpoint.username is not None or endpoint.password is not None
        or endpoint.path not in ('','/') or endpoint.query or endpoint.fragment):
        raise ValueError('Exactly four transactions and a private loopback endpoint are required')
    bodies=[json.dumps(t).encode() for t in transactions]
    clients=[];ready=threading.Barrier(4,timeout=5)
    try:
        for _ in bodies:
            client=http.client.HTTPConnection(endpoint.hostname,endpoint.port,timeout=10)
            clients.append(client);client.connect()
        def send(item):
            client,body=item;ready.wait()
            client.request('POST','/v1/chain/push_transaction',body,{'Content-Type':'application/json'})
            response=client.getresponse();payload=response.read()
            if response.status not in (200,202):
                raise RuntimeError('Correctness wave rejected: '+payload.decode(errors='replace')[:1500])
            return json.loads(payload)
        with ThreadPoolExecutor(max_workers=4) as workers:
            return list(workers.map(send,zip(clients,bodies)))
    finally:
        for client in clients:client.close()
