"""Wait on native irreversible deadlines, then each authorized phase ONCE.

Read retries are confined to await_review(). Financial phase calls are outside
that loop and never retried. The immutable per-phase sender enforces this too.
"""
import json
import time
import bank_upgrade as b

def await_review(old, new, identifier):
    failed_reads = 0
    while True:
        try:
            s = b.rpc('get_upgrade_state')
            b.verify_proposal(s, old, new, identifier)
            observation = b.observe()
            failed_reads = 0
        except RuntimeError as error:
            failed_reads += 1
            b.save('WAIT_READ_WARNING.json', {'utc': b.now(), 'count': failed_reads, 'message': str(error),
                                              'no_signature_or_submission': True})
            if failed_reads >= 3:
                raise
            time.sleep(20)
            continue
        if observation['remaining_irreversible_seconds'] == 0:
            b.verify_proposal(s, old, new, identifier, mature=True)
            return
        time.sleep(20)

def main():
    b.artifacts()
    assert b.load('AUTHORIZATION.json')['status'] == 'AUTHORIZED'
    p = b.load('PROPOSAL.json')
    assert p['status'] == 'PROPOSED_IRREVERSIBLE'
    assert not (b.ROOT / 'WORKFLOW_STARTED.json').exists(), 'Observe/reconcile existing workflow; never replay it'
    b.save('WORKFLOW_STARTED.json', {'utc': b.now(), 'candidate_proposal_id': p['governance']['proposal_id'],
                                    'only_new_case_controller': True, 'no_financial_retries': True})
    await_review(b.OLD, b.NEW, p['governance']['proposal_id'])
    minted = b.execute_mint_restore_proposal()  # exactly one authorized call
    b.save('WORKFLOW_MINT_DONE.json', minted)
    restoration = b.load('MINT_AND_RESTORE_PROPOSAL.json')
    await_review(b.NEW, b.OLD, restoration['restore_governance']['proposal_id'])
    restored = b.restore()  # exactly one authorized call
    b.save('WORKFLOW_COMPLETE.json', {'utc': b.now(), **restored})
    print(json.dumps(restored), flush=True)

if __name__ == '__main__':
    try:
        main()
    except BaseException as error:
        b.save('WORKFLOW_HOLD.json', {'utc': b.now(), 'type': type(error).__name__, 'message': str(error),
                                    'no_automatic_financial_retry': True})
        print(json.dumps({'status': 'HOLD_READ_ONLY_RECONCILIATION_REQUIRED', 'type': type(error).__name__,
                          'message': str(error)}), flush=True)
        raise SystemExit(1)
