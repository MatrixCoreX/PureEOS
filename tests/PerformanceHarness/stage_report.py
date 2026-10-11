#!/usr/bin/env python3
"""Compare bounded stage histograms from two Prometheus scrapes (not chain TPS)."""
import argparse
import json
import math
from pathlib import Path
import re

from finality_tps import require


def parse(text):
    metrics = {}
    pattern = re.compile(r'^nodeos_stage_duration_seconds_(bucket|sum|count)\{([^}]*)\} ([^ ]+)$')
    for line in text.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        kind, raw, value = match.groups()
        labels = dict(re.findall(r'(\w+)="([^"]*)"', raw))
        require('stage' in labels and set(labels) <= {'stage', 'le'}, 'Unexpected stage labels')
        stage = metrics.setdefault(labels['stage'], {'buckets': {}})
        value = float(value)
        require(math.isfinite(value) and value >= 0, 'Invalid histogram value')
        if kind == 'bucket':
            boundary = float(labels['le'])
            require(boundary not in stage['buckets'], 'Duplicate histogram bucket')
            stage['buckets'][boundary] = value
        else:
            require(kind not in stage, 'Duplicate histogram counter')
            stage[kind] = value
    require(metrics, 'No stage histograms found')
    return metrics


def compare(before, after):
    before, after = parse(before), parse(after)
    require(before.keys() == after.keys(), 'Stage set changed')
    result = {}
    for name, end in after.items():
        start = before[name]
        require(start['buckets'].keys() == end['buckets'].keys(), 'Bucket boundaries changed')
        count, total = end['count'] - start['count'], end['sum'] - start['sum']
        buckets = [(b, end['buckets'][b] - start['buckets'][b]) for b in sorted(end['buckets'])]
        require(count >= 0 and total >= 0 and buckets and math.isinf(buckets[-1][0]), 'Counter reset or missing +Inf')
        require(all(0 <= v <= count for _, v in buckets) and buckets[-1][1] == count,
                'Inconsistent histogram count')
        require(all(a[1] <= b[1] for a, b in zip(buckets, buckets[1:])), 'Nonmonotone bucket delta')
        item = {'attempt_count': count, 'inclusive_seconds': total,
                'mean_us': total * 1e6 / count if count else None}
        for q in (50, 95, 99):
            boundary = next((b for b, v in buckets if count and v >= count * q / 100), math.inf)
            item[f'p{q}_upper_bound_us'] = boundary * 1e6 if math.isfinite(boundary) else None
        result[name] = item
    return {'stages': result, 'warning': 'Inclusive, possibly nested/parallel attempts, including failures. Do not sum stages or equate them to billed CPU; quantiles are bucket upper bounds.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    print(json.dumps(compare(args.before.read_text(), args.after.read_text()), indent=2))
