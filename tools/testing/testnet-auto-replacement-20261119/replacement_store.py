"""Crash-persistent local journal for TEST replacement jobs."""
from pathlib import Path
import json
import os


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    with temporary.open('w') as stream:
        os.chmod(temporary, 0o600)
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def load(path, default=None):
    path = Path(path)
    return json.loads(path.read_text()) if path.exists() else default


def advance_transaction(path, intent, backend):
    """Broadcast once; any subsequent call only reconciles the immutable ID."""
    path = Path(path)
    state = load(path)
    if state is not None:
        if state['intent'] != intent:
            raise ValueError('Existing transaction intent changed')
        if state['status'] == 'IRREVERSIBLE':
            backend.verify_receipt(state['receipt'])
            return state['receipt']
        if not state.get('transaction_id'):
            raise RuntimeError('HOLD_INTERRUPTED_SIGNING: no automatic re-sign')
        receipt, cursor = backend.reconcile(state)
        state['cursor'] = cursor
        if receipt:
            state.update(status='IRREVERSIBLE', receipt=receipt)
        save(path, state)
        return receipt
    state = {'status': 'SIGNING_NO_BROADCAST', 'intent': intent}
    save(path, state)
    packet, transaction_id, start_LIB, packet_hash = backend.sign(intent, path.parent)
    state.update(status='SIGNED_IMMUTABLE', transaction_id=transaction_id,
                 start_LIB=start_LIB, cursor=start_LIB + 1, packet_SHA256=packet_hash)
    save(path, state)
    state['status'] = 'BROADCASTING_ONCE'
    save(path, state)
    try:
        backend.broadcast(packet, transaction_id)
        state['status'] = 'ACCEPTED_AWAITING_IRREVERSIBILITY'
    except Exception as error:
        state.update(status='AMBIGUOUS_OR_REJECTED_RECONCILE_ONLY', error=type(error).__name__)
    save(path, state)
    receipt, cursor = backend.reconcile(state)
    state['cursor'] = cursor
    if receipt:
        state.update(status='IRREVERSIBLE', receipt=receipt)
    save(path, state)
    return receipt
