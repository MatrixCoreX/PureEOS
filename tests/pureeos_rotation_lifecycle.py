"""Pure completion gate for long-lived cloud tests; never creates or stops hosts."""
from pureeos_rotation_cohort_cloud_test import cohort_names


def completion_errors(report, expected_sha):
    errors=[]
    def require(ok,message):
        if not ok:errors.append(message)
    if not isinstance(report,dict):return ['Final test report missing']
    require(report.get('status')=='passed','Test is not fully passed')
    require(report.get('fourhost_script_sha256')==expected_sha,'Test source identity mismatch')
    require(report.get('vm_hosts')==4,'Four-host evidence missing')
    require(report.get('processes_stopped') is True and not report.get('shutdown_errors'),
            'Clean process closure missing')
    processes=report.get('processes',{})
    require(set(processes)==set(cohort_names(40)),'Forty independent nodes missing')
    require(len({str(v.get('pid')) for v in processes.values()})==40,'Unique process identities missing')
    phases=report.get('cohort_phases',[])
    require([p.get('candidate_count') for p in phases]==[30,40],'Both complete phases required')
    for phase in phases:
        count=phase.get('candidate_count')
        if count not in (30,40):errors.append('Unexpected candidate phase');continue
        wanted=set(cohort_names(count))
        require(phase.get('status')=='passed',f'{count}: phase not passed')
        require(wanted<=set(phase.get('queue_entries',[])),f'{count}: full-seat queue coverage incomplete')
        require(wanted<=set(phase.get('finalized_production',{}).get('counts',{})),
                f'{count}: independent irreversible production incomplete')
        agreement=phase.get('agreement',{})
        require(set(agreement.get('infos',{}))==wanted and bool(agreement.get('common_block_id')),
                f'{count}: independent ledger agreement missing')
    sizes={len(v.get('schedule',{}).get('producers',[])) for v in report.get('committee_events',[])}
    require(sizes==set(range(1,22)),'Every committee size 1..21 required')
    recovered=report.get('recovered_fault',{})
    require(bool(recovered.get('node')) and 0<int(recovered.get('missing',{}).get('zero_windows',0))<3
            and recovered.get('missing',{}).get('banned') is False
            and recovered.get('recovered',{}).get('zero_windows')==0
            and recovered.get('recovered',{}).get('banned') is False,'Recoverable fault evidence missing')
    fault=report.get('fault',{})
    banned=fault.get('candidate_after',{})
    require(bool(fault.get('node')) and banned.get('banned') is True
            and banned.get('zero_windows')==3 and len(set(banned.get('evidence_slots',[])))==3,
            'Three distinct zero-window permanent ban missing')
    require(len(fault.get('actual_post_ban_entries',[]))>=2
            and fault.get('node') not in fault.get('actual_post_ban_entries',[]),
            'Two actual post-ban replacements required')
    require('preserved_unpaid_blocks' in fault and bool(fault.get('finalized_blocks')),
            'Preserved rewards and independent ban-window scan missing')
    outage=report.get('host_group_outage',{})
    require(outage.get('role') in ('b','c','d') and bool(outage.get('stopped'))
            and outage.get('threshold')==15 and outage.get('surviving_finalizers',0)>=15,
            'Quorum-safe peer-host process outage missing')
    final=report.get('final_synchronization',{})
    require(set(final.get('infos',{}))==set(cohort_names(40)) and bool(final.get('common_block_id')),
            'Final forty-ledger agreement missing')
    samples=report.get('resource_samples',[])
    require(report.get('resource_evidence_status')=='complete' and bool(samples)
            and not report.get('resource_collection_errors'),'Resource evidence incomplete')
    for sample in samples:
        hosts=sample.get('hosts',{})
        require(set(hosts)==set('abcd'),'Four-host metric sample incomplete')
        for host in hosts.values():
            oom=host.get('probes',{}).get('oom',{})
            require(host.get('status')=='complete' and oom.get('oom_detected') is False,
                    'OOM or incomplete probe evidence')
    return errors


def monitor_action(unit_state, report, expected_sha):
    """Only a final, fully audited run can advance toward archive-gated cleanup."""
    if unit_state in ('active','activating','deactivating'):return 'observe'
    if report is None:return 'retry-observation'
    return 'collect-verified-archives' if not completion_errors(report,expected_sha) else 'needs-repair'
