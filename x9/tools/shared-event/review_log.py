#!/usr/bin/env python3
"""Read one local X9 boot capture; summarize only bounded SharedEvent diagnostics."""
import argparse
import json
import re

STAMP = r'^\[\s*(\d+(?:\.\d+)?)\]\s+'
EVENT = re.compile(STAMP + r'SWEET SURF EVENT( RESULT)?: (\S+) pid=(\d+) seq=(\d+) sel=(\d+) (.*)$')
ABORT = re.compile(STAMP + r'CRASH AegirPoster\[(\d+)\] sig 6(?:\s|$)')
DARK = re.compile(STAMP + r'SWEET FB: display off(?:\s|$)')
BOOT = re.compile(STAMP + r'.*\bboot-args = (.*)$')
STATUS = re.compile(r'kr=(0x[0-9a-fA-F]+)\s*$')


def review(lines):
    boot_flags, calls, aborts = [], {}, []
    first_dark = None
    errors = 0
    for line_no, line in enumerate(lines, 1):
        match = BOOT.match(line)
        if match:
            args = match[2].split()
            boot_flags.append({
                'experimental': [t for t in args if t.startswith('x9_surf_events=')],
                'diagnostic': [t for t in args if t.startswith(('x9ed=', 'x9_surf_event_diag='))],
            })
        match = DARK.match(line)
        if match and first_dark is None:
            first_dark = float(match[1])
        match = ABORT.match(line)
        if match:
            aborts.append({'seconds': float(match[1]), 'pid': int(match[2]), 'line': line_no})
        match = EVENT.match(line)
        if not match:
            continue
        seconds, result, comm, pid, seq, sel, details = match.groups()
        if comm != 'AegirPoster':
            continue
        start = {'pid': int(pid), 'seq': int(seq), 'selector': int(sel)}
        key = (start['pid'], start['seq'], start['selector'])
        if not result:
            if key in calls:
                errors += 1
            else:
                calls[key] = dict(start, seconds=float(seconds), line=line_no)
        else:
            status = STATUS.fullmatch(details)
            start_call = calls.get(key)
            if not status or not start_call or 'return_code' in start_call or float(seconds) < start_call['seconds']:
                errors += 1
            else:
                start_call.update(return_code=status[1].lower(), result_line=line_no)
    unambiguous = len(boot_flags) == 1 and not errors
    first_abort = aborts[0] if aborts else None
    before_abort = []
    if unambiguous and first_abort:
        for call in calls.values():
            if (call['pid'] != first_abort['pid'] or call['line'] >= first_abort['line']
                    or call['seconds'] > first_abort['seconds']):
                continue
            prior = call.copy()
            if prior.get('result_line', 0) >= first_abort['line']:
                prior.pop('result_line')
                prior.pop('return_code')
            before_abort.append(prior)
    return {
        'boot_args_observations': boot_flags,
        'diagnostic_conflicts_or_orphans': errors,
        'calls_observed': len(calls),
        'results_matched': sum('return_code' in c for c in calls.values()),
        'first_display_off_seconds': first_dark,
        'abort_count_observed': len(aborts),
        'aborts_at_or_after_first_display_off': None if first_dark is None else
            sum(a['seconds'] >= first_dark for a in aborts),
        'first_abort': first_abort,
        'same_pid_calls_before_first_abort': before_abort,
        'acceptance': 'not evaluated: call results do not prove notification delivery or feature behavior',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture')
    args = parser.parse_args()
    with open(args.capture, encoding='utf-8', errors='replace') as capture:
        print(json.dumps(review(capture), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
