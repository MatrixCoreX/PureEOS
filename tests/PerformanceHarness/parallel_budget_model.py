"""Offline design model: total work AND dependency-path work.

NOT a consensus rule, gas meter, access tracer or admission proof. Abstract
footprints and weights are supplied by tests/analysis; they are NOT trusted
transaction declarations. Current nodeos never imports this module. Results
cannot authorize increasing a chain limit or predict hardware TPS.
"""
from dataclasses import dataclass


MAX_UNITS = (1 << 64) - 1
MAX_TRANSACTIONS = 4096
MAX_KEYS = 65536
MAX_TRANSACTION_KEYS = 4096


def unsigned(value, label, positive=False):
    if type(value) is not int or not (int(positive) <= value <= MAX_UNITS):
        raise ValueError('Invalid ' + label)
    return value


def add(left, right):
    if right > MAX_UNITS - left:
        raise ValueError('Work counter overflow')
    return left + right


@dataclass(frozen=True)
class Budget:
    total: int
    critical_path: int
    serial: int
    transaction: int

    def __post_init__(self):
        for label in ('total', 'critical_path', 'serial', 'transaction'):
            unsigned(getattr(self, label), label, positive=True)
        if self.transaction > min(self.total, self.critical_path, self.serial):
            raise ValueError('Transaction limit exceeds an aggregate budget')


@dataclass(frozen=True)
class Work:
    units: int
    reads: frozenset[str] = frozenset()
    writes: frozenset[str] = frozenset()
    known: bool = True

    def __post_init__(self):
        unsigned(self.units, 'transaction work', positive=True)
        if type(self.known) is not bool:
            raise ValueError('Invalid coverage flag')
        for keys in (self.reads, self.writes):
            if not isinstance(keys, frozenset) or any(type(key) is not str or not 0 < len(key) <= 256 for key in keys):
                raise ValueError('Expected owned, bounded abstract keys')
        if len(self.reads | self.writes) > MAX_TRANSACTION_KEYS:
            raise ValueError('Transaction key limit exceeded')
        if not self.known and (self.reads or self.writes):
            raise ValueError('Unknown access must be an explicit whole-block barrier')


def assess_block(transactions, budget):
    """Ordered RAW/WAR/WAW dependencies; shared reads need no dependency.

    Weight units are abstract positive integers, not measured microseconds or
    instruction counts. Even a passing result supplies neither real footprint
    completeness nor a lower bound on actual computation; both are mandatory
    before any future protocol integration. Global/range/native/unknown effects
    must be explicit barriers until a canonical complete model covers them.
    """
    if not isinstance(budget, Budget) or not isinstance(transactions, (tuple, list)) or len(transactions) > MAX_TRANSACTIONS:
        raise ValueError('Invalid bounded block input')
    writers, readers = {}, {}
    total = serial = critical = barrier = 0
    completions, transaction_exceeded = [], False
    all_keys = set()
    for work in transactions:
        if not isinstance(work, Work):
            raise ValueError('Expected validated work')
        total = add(total, work.units)
        transaction_exceeded |= work.units > budget.transaction
        all_keys.update(work.reads | work.writes)
        if len(all_keys) > MAX_KEYS:
            raise ValueError('Block key limit exceeded')
        if not work.known:
            # Wait for ALL preceding chains. ALL later work waits for this
            # barrier, including work touching previously unseen keys.
            end = add(critical, work.units)
            serial = add(serial, work.units)
            barrier = end
        else:
            predecessor = barrier
            for key in work.reads:
                predecessor = max(predecessor, writers.get(key, 0))
            for key in work.writes:
                predecessor = max(predecessor, writers.get(key, 0), readers.get(key, 0))
            end = add(predecessor, work.units)
            for key in work.reads - work.writes:
                readers[key] = max(readers.get(key, 0), end)
            for key in work.writes:
                writers[key] = end
                readers.pop(key, None)
        critical = max(critical, end)
        completions.append(end)
    violations = []
    if transaction_exceeded:
        violations.append('transaction')
    for label, value in (('total', total), ('critical_path', critical), ('serial', serial)):
        if value > getattr(budget, label):
            violations.append(label)
    return {'scope': 'offline abstract model; NOT nodeos admission or a capacity/TPS guarantee',
            'within_model_budget': not violations, 'violations': violations,
            'transactions': len(transactions), 'total_work': total,
            'critical_path_work': critical, 'serial_barrier_work': serial,
            'completion_work': completions, 'abstract_keys': len(all_keys)}
