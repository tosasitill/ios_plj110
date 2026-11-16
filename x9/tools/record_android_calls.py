#!/usr/bin/env python3
"""Record existing Android radio/audio logs; never dial or change device state.

The private capture directory must contain session.json with the explicitly
selected serial (as recorded by the baseline capture). No arbitrary device
selection, log clearing, setprop, RIL restart, instrumentation or device writes.
The bounded worker persists between CLI turns; stop requests use a local file.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def now():
    return datetime.datetime.now().astimezone().isoformat()


def private_write(path, data, exclusive=False):
    with path.open('xb' if exclusive else 'wb') as f:
        os.chmod(path, 0o600)
        f.write(data)


def write_json(path, data):
    # Status readers must never see partially written worker metadata.
    tmp = path.with_suffix(path.suffix + '.tmp')
    private_write(tmp, (json.dumps(data, indent=2) + '\n').encode())
    tmp.replace(path)


def read_json(path):
    return json.loads(path.read_text())


def device(out):
    adb = shutil.which('adb')
    if not adb:
        raise ValueError('adb not installed')
    serial = read_json(out / 'session.json')['serial']
    if not isinstance(serial, str) or not serial or serial.startswith('-'):
        raise ValueError('invalid selected device in private session.json')
    return [adb, '-s', serial]


def mark(out, label):
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,63}', label):
        raise ValueError('use a short event label, not a phone number')
    r = subprocess.run(device(out) + ['exec-out',
        'sh -c "date +%s; cat /proc/uptime"'], capture_output=True, timeout=10)
    record = {'host_time': now(), 'label': label, 'device_clock_exit': r.returncode,
              'device_clock': r.stdout.decode(errors='replace').strip()}
    p = out / 'events.jsonl'
    with p.open('a') as f:
        p.chmod(0o600)
        f.write(json.dumps(record) + '\n')


def snapshot(out):
    """Save a bounded, read-only snapshot without changing log settings.

    Keep stdout/stderr even on failure: some su wrappers report a shell error
    in stdout and exit zero. Metadata distinguishes transport exit from a
    recognizable source shape; neither is a call/SMS success receipt.
    """
    commands = [
        ('radio-logcat.txt', ['exec-out', 'logcat', '-b', 'radio', '-d', '-v', 'threadtime']),
        ('main-logcat.txt', ['exec-out', 'logcat', '-b', 'main', '-d', '-v', 'threadtime']),
        ('telephony-registry.txt', ['exec-out', 'dumpsys', 'telephony.registry']),
        ('telecom.txt', ['exec-out', 'dumpsys', 'telecom']),
        ('dmesg.txt', ['exec-out', "su -c 'dmesg'"]),
    ]
    # Preflight all local names before collecting anything. Never overwrite
    # evidence or leave a worker running after the CLI turn finishes.
    names = ['snapshot.json']
    for name, _ in commands:
        names.extend([name, name + '.stderr'])
    if any((out / name).exists() for name in names):
        raise ValueError('snapshot already exists; preserve it and use a new case')
    adb = device(out)
    selected = read_json(out / 'session.json')['serial']
    r = subprocess.run([adb[0], 'devices'], capture_output=True, timeout=10)
    connected = [parts[0] for line in r.stdout.decode(errors='replace').splitlines()[1:]
                 if len(parts := line.split()) == 2 and parts[1] == 'device']
    if r.returncode or connected != [selected]:
        raise ValueError('expected the sole selected connected device; serial withheld')
    state = {'started': now(), 'read_only': True, 'logcat_cleared': False,
             'state': 'collecting', 'records': []}
    # Exclusive creation also rejects a concurrent snapshot invocation.
    private_write(out / 'snapshot.json', (json.dumps(state, indent=2) + '\n').encode(),
                  exclusive=True)
    failed = False
    try:
        for name, args in commands:
            command = adb + args
            timed_out = False
            try:
                r = subprocess.run(command, capture_output=True, timeout=45)
                data, errors, code = r.stdout, r.stderr, r.returncode
            except subprocess.TimeoutExpired as e:
                data, errors, code = e.stdout or b'', e.stderr or b'', None
                timed_out = True
            private_write(out / name, data, exclusive=True)
            private_write(out / (name + '.stderr'), errors, exclusive=True)
            if name.endswith('logcat.txt'):
                recognized = b'--------- beginning of ' in data
            elif name == 'telephony-registry.txt':
                recognized = b'TelephonyRegistry' in data or b'mCallState=' in data
            elif name == 'telecom.txt':
                recognized = b'CallsManager:' in data
            else:
                clean = re.sub(rb'\x1b\[[0-9;]*m', b'', data)
                recognized = re.search(rb'^\[\s*[0-9]+\.[0-9]+\]', clean, re.M) is not None
            failed |= code != 0 or not recognized
            state['records'].append({'file': name, 'command': shlex.join(command),
                'exit': code, 'timed_out': timed_out, 'bytes': len(data),
                'sha256': hashlib.sha256(data).hexdigest(),
                'recognized_source_shape': recognized})
            write_json(out / 'snapshot.json', state)
    except BaseException:
        state.update(state='interrupted', ended=now())
        write_json(out / 'snapshot.json', state)
        raise
    state.update(state='incomplete' if failed else 'captured', ended=now())
    write_json(out / 'snapshot.json', state)
    print(json.dumps({'state': state['state'], 'read_only': True,
        'files': [{'file': x['file'], 'bytes': x['bytes'], 'exit': x['exit'],
                   'recognized_source_shape': x['recognized_source_shape']}
                  for x in state['records']]}, indent=2))
    return 2 if failed else 0


def watch(out, seconds):
    state = {'worker_pid': os.getpid(), 'started': now(),
             'deadline_seconds': seconds, 'state': 'starting',
             'read_only': True, 'logcat_cleared': False,
             'command': "su -c 'logcat -b all -v threadtime -T 1'"}
    proc = None
    try:
        with (out / 'live-logcat.txt').open('wb') as data, \
                (out / 'live-logcat.stderr').open('wb') as errors:
            for p in [out / 'live-logcat.txt', out / 'live-logcat.stderr']:
                p.chmod(0o600)
            proc = subprocess.Popen(device(out) + ['exec-out', state['command']],
                                    stdout=data, stderr=errors)
            state.update(state='recording', logcat_pid=proc.pid)
            write_json(out / 'live.json', state)
            deadline = time.monotonic() + seconds
            while proc.poll() is None and time.monotonic() < deadline:
                if (out / 'stop-requested').exists():
                    break
                time.sleep(0.5)
            reason = ('logcat_exited' if proc.poll() is not None else
                      'requested' if (out / 'stop-requested').exists() else 'deadline')
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            state.update(state='stopped', ended=now(), reason=reason,
                         logcat_exit=proc.returncode)
    except BaseException as e:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        state.update(state='failed', ended=now(), error=type(e).__name__)
        raise
    finally:
        write_json(out / 'live.json', state)


def status(out):
    state = read_json(out / 'live.json')
    p = out / 'live-logcat.txt'
    active = state['state'] == 'recording'
    if active:
        try:
            os.kill(state['worker_pid'], 0)
        except ProcessLookupError:
            active = False
    print(json.dumps({'state': state['state'], 'worker_alive': active,
        'bytes': p.stat().st_size if p.exists() else 0,
        'started': state['started'], 'ended': state.get('ended'),
        'reason': state.get('reason')}, indent=2))
    return active


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['snapshot', 'start', 'status', 'mark', 'stop', '_watch'])
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--seconds', type=int, default=1200)
    p.add_argument('--label', default='manual')
    args = p.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to((ROOT / 'x9/out').resolve()) or not out.is_dir():
        p.error('use an existing private directory under this checkout x9/out/')
    if not 10 <= args.seconds <= 1800:
        p.error('seconds must be 10..1800')
    try:
        if args.action == 'snapshot':
            return snapshot(out)
        elif args.action == '_watch':
            watch(out, args.seconds)
        elif args.action == 'start':
            if (out / 'live.json').exists() or (out / 'live-logcat.txt').exists():
                raise ValueError('live capture already exists; preserve it and use a new case')
            r = subprocess.run(device(out) + ['exec-out', "su -c 'id'"],
                               capture_output=True, timeout=10)
            if r.returncode or not r.stdout.startswith(b'uid=0('):
                raise ValueError('verified root su -c unavailable; no logging settings changed')
            mark(out, 'capture_start')
            with (out / 'worker.stderr').open('wb') as errors:
                (out / 'worker.stderr').chmod(0o600)
                worker = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                    '_watch', '--out', str(out), '--seconds', str(args.seconds)],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                    stderr=errors, start_new_session=True)
            for _ in range(30):
                if (out / 'live.json').exists():
                    break
                if worker.poll() is not None:
                    raise ValueError('worker failed; inspect private worker.stderr')
                time.sleep(0.1)
            time.sleep(0.5)
            if not status(out):
                raise ValueError('logcat exited; inspect private stderr')
        elif args.action == 'status':
            return 0 if status(out) else 2
        elif args.action == 'mark':
            if not status(out):
                raise ValueError('capture is not active')
            mark(out, args.label)
        elif args.action == 'stop':
            if read_json(out / 'live.json')['state'] == 'recording':
                mark(out, 'capture_stop_requested')
                private_write(out / 'stop-requested', now().encode())
                for _ in range(100):
                    if read_json(out / 'live.json')['state'] != 'recording':
                        break
                    time.sleep(0.1)
                else:
                    raise ValueError('worker did not stop; inspect live.json before any signal')
            status(out)
    except subprocess.TimeoutExpired:
        p.error('selected device command timed out; serial withheld')
    except (ValueError, KeyError, OSError) as e:
        p.error(str(e))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
