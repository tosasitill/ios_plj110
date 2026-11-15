#!/usr/bin/env python3
"""Build one complete native-Bluetooth candidate; never install, boot or probe.

Native userland/jobs -> offline root payload -> (optional) kernel-embedded
root payload -> full overlay kernel -> ONE boot.img. With boot-arg
x9_bt_root=1 the kernel's PID1 installs the seven root files itself before
launchd starts (x9_bluetooth_root.inc); a second boot-rollback/boot.img with
x9_bt_root=2 restores the Apple originals. Completion is published only after every normal build command exits
zero. There is no new-artifact hash, unpack, disassembly or test-suite pass.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
X9 = ROOT / 'x9'
sys.path.insert(0, str(Path(__file__).resolve().parent))
from launch_cache import CACHE_NAME, CACHE_LIMIT, derive, validate_pair
from package import JOBS
XNU_TAG = 'xnu-8796.141.3'
XNU_COMMIT = '1b191cb58250d0705d8a51287127505aa4bc0789'
JOB_NAMES = ('com.apple.bluetoothd.plist', 'com.apple.BlueTool.plist')
LIMITATIONS = [
    'built_complete/candidate_ready describe compilation and packaging only, NOT controller readiness',
    'root payload is NOT installed; obtain explicit user confirmation before root modification',
    'root staging requires live root preflight, exact original jobs/cache comparison and preserved backups',
    'sleep/resume is closed-transport-only; no active-link idle sleep, wake-enhance or system-suspend claim',
    'SSPM/profile/ownership, UART PIO/escape throughput and Android vendor completions remain hardware-unverified on XNU',
    '20H392 native injection, initialization, scan, pairing, BLE, A2DP, HFP and SCO require actual iOS validation',
    'MTK SCO framing is implemented; native voice-route/WBS vendor equivalence still requires real MT6653 evidence',
    'boot image alone does not install the H4 bridge or independent/cached Program-only launch job overrides',
    'keep bootloaders/recovery/modem/persist/partition backups intact; device boot is performed by the user',
]


def repository_path(path):
    """Repo-local inputs may use the documented shared .local worktree link."""
    path = path if path.is_absolute() else ROOT / path
    resolved = path.resolve()
    if not (resolved.is_relative_to(ROOT) or resolved.is_relative_to((ROOT / '.local').resolve())):
        raise ValueError('input must be inside this repository or its fixed shared .local layout')
    return path


def original_bytes(path, maximum=65536):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= maximum:
        raise ValueError(f'expected bounded regular build input: {path.name}')
    return path.read_bytes()


def write_json(path, value):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def readiness(path, ready):
    if not path.is_file():
        if ready:
            raise ValueError('required component build metadata is missing')
        return
    value = json.loads(path.read_text())
    value.update(candidate_ready=ready, runtime_verified=False, audio_verified=False, stageable=False)
    write_json(path, value)


def run(command, log):
    shown = '+ ' + shlex.join(map(str, command))
    print(shown, flush=True)
    log.write(shown + '\n'); log.flush()
    process = subprocess.Popen(list(map(str, command)), cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, errors='replace',
                               start_new_session=True)
    try:
        for line in process.stdout:
            log.write(line); log.flush()
            try:
                sys.stdout.write(line); sys.stdout.flush()
            except BrokenPipeError:
                pass  # The authoritative build log remains intact.
        status = process.wait()
    except BaseException:
        # Stop only this build's child process group; never any device process.
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=10)
        except ProcessLookupError:
            process.wait()
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        raise
    finally:
        process.stdout.close()
    log.write(f'command exit={status}\n'); log.flush()
    if status:
        raise subprocess.CalledProcessError(status, command)


def verify_isolated_source(dxb, abi, out, log):
    # Source-only comparison. Never inspect the newly built kernel or objects.
    source = dxb / 'xnu'
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', XNU_TAG + '^{commit}'],
                                     text=True).strip()
    if actual != XNU_COMMIT:
        raise ValueError('isolated XNU does not contain the pinned original source commit')
    patches = [X9 / 'xnu/0001-x9-sweet-xnu-8796.141.3.patch']
    if abi == 'ios':
        patches += [X9 / 'xnu' / name for name in (
            '0002-x9-ios-kext-cxx-abi.patch', '0003-x9-sparse-prelink-kext-memory.patch',
            '0004-x9-diag-unaligned-static-mfree.patch', '0005-x9-diag-prelinked-kext-load.patch')]
    patches.append(X9 / 'xnu/0006-x9-bt-uart-irq.patch')
    with tempfile.TemporaryDirectory(prefix='source-preflight-', dir=out) as name:
        directory = Path(name)
        archive = directory / 'original.tar'
        with archive.open('wb') as stream:
            subprocess.run(['git', '-C', str(source), 'archive', XNU_TAG], stdout=stream, check=True)
        expected = directory / 'expected'
        expected.mkdir()
        run(['tar', '-xf', archive, '-C', expected], log)
        run(['git', '-C', expected, 'init', '-q'], log)
        run(['git', '-C', expected, 'apply', '--whitespace=nowarn', *patches], log)
        run(['diff', '-rq', '-x', '.git', expected, source], log)
    return [path.name for path in patches]


def build(a):
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,95}', a.name) or a.jobs < 1:
        raise ValueError('--name needs a simple unique output name, and --jobs must be positive')
    out = X9 / 'out' / a.name
    if out.exists() or out.is_symlink():
        raise ValueError('candidate output already exists; choose a NEW name')
    config = repository_path(a.config)
    boot_config = repository_path(a.boot_config)
    launchd = repository_path(a.launchd_source)
    launchd_cache = repository_path(a.launchd_cache_source)
    spec = importlib.util.spec_from_file_location('x9_candidate_builder', X9 / 'build/build.py')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    config_bytes = original_bytes(config)
    original_bytes(boot_config)
    cfg = builder.load_config(config)
    if cfg.get('allow_large_boot'):
        raise ValueError('complete Bluetooth candidates must retain the normal 20 MiB boot-image guard')
    opts = builder.boot_options(boot_config, a.boot_arg)
    if (not isinstance(opts.get('boot_args'), list) or not isinstance(opts.get('options'), list)
            or not all(isinstance(token, str) and token and not any(c.isspace() or c == '\0' for c in token)
                       for token in opts['boot_args'])
            or not all(isinstance(token, str) and '\0' not in token for token in opts['options'])):
        raise ValueError('invalid complete boot configuration')
    root_args = [t for t in opts['boot_args'] if t.partition('=')[0] == 'x9_bt_root']
    if root_args not in ([], ['x9_bt_root=1']):
        raise ValueError('only x9_bt_root=1 selects the kernel root installer (=2 is the generated rollback image)')
    embed_root = bool(root_args)
    if embed_root != bool(a.apple_originals):
        raise ValueError('x9_bt_root=1 and --apple-originals must be given together')
    apple = repository_path(a.apple_originals) if embed_root else None
    if a.previous_payload and not embed_root:
        raise ValueError('--previous-payload only applies with x9_bt_root=1')
    previous = [repository_path(path) for path in a.previous_payload]
    if not builder.bluetooth_init_enabled(opts['boot_args']):
        raise ValueError('complete candidate requires explicit x9_bt=1 x9_bt_init=1 x9_bt_fc=1')
    builder.require_bluetooth_inputs(cfg, opts['boot_args'])
    selectors = {}
    for token in opts['boot_args']:
        key, sep, value = token.partition('=')
        if key in ('x9_bt_probe', 'x9_bt_raw', 'x9_bt_discovery'):
            raise ValueError('retired probe/raw/discovery arguments are not complete native candidates')
        if key.startswith('x9_wifi_xo'):
            if key in selectors or not sep:
                raise ValueError('missing/duplicate captured XO word')
            number = int(value, 0)
            if not 0 <= number <= 0xffffffff:
                raise ValueError('captured XO word outside uint32')
            selectors[key] = number
    record = b''.join(selectors[f'x9_wifi_xo{i}'].to_bytes(4, 'little') for i in range(7))
    if record[:2] != b'\x19\0':
        raise ValueError('complete captured CONNXO record is required, without guessed defaults')
    dxb = Path(cfg['dxb']).resolve()
    if not dxb.is_relative_to((X9 / 'out').resolve()):
        raise ValueError('candidate config.dxb must select a verified isolated tree inside this worktree x9/out/')
    for name in ('xnu', 'xnu/.git', 'build', 'build/xnu.obj', 'fakeroot'):
        child = dxb / name
        if child.is_symlink() or not child.is_dir() or not child.resolve().is_relative_to(dxb):
            raise ValueError('isolated compiler tree must be self-contained')
    jobs = {name: original_bytes(launchd / name) for name in JOB_NAMES}
    cache_bytes = original_bytes(launchd_cache, CACHE_LIMIT)
    cache_candidate, _, _ = derive(cache_bytes, JOBS)
    cache_contract = validate_pair(cache_bytes, cache_candidate, JOBS)
    commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    if subprocess.run(['git', '-C', str(ROOT), 'diff', '--quiet', 'HEAD', '--', 'x9']).returncode:
        raise ValueError('commit completed x9 source changes before building the final candidate')
    out.mkdir(parents=True, exist_ok=False)
    complete = False
    stage = 'input-preflight'
    runtime = out / 'runtime'
    payload = out / 'root-payload'
    boot = out / 'boot'
    try:
        inputs = out / 'inputs'
        inputs.mkdir()
        config_snapshot = inputs / 'local.json'
        config_snapshot.write_bytes(config_bytes)
        boot_snapshot = inputs / 'boot.json'
        write_json(boot_snapshot, opts)
        job_snapshot = inputs / 'launchd'
        job_snapshot.mkdir()
        for name, data in jobs.items():
            (job_snapshot / name).write_bytes(data)
        (job_snapshot / CACHE_NAME).write_bytes(cache_bytes)
        with (out / 'candidate-build.log').open('w') as log:
            run([sys.executable, '-I', X9 / 'build/prepare-local.py', '--check'], log)
            patches = verify_isolated_source(dxb, a.abi, out, log)
            common = ['--config', config_snapshot, '--boot-config', boot_snapshot]
            stage = 'native-userland-and-launch-jobs'
            run([sys.executable, '-I', X9 / 'bluetooth/build.py', '--developer-dir', cfg['developer_dir'],
                 '--launchd-source', job_snapshot, '--launchd-cache-source', job_snapshot / CACHE_NAME,
                 '--out', runtime], log)
            stage = 'offline-root-payload'
            run([sys.executable, '-I', X9 / 'bluetooth/package.py', '--runtime', runtime, '--out', payload], log)
            stage = 'kernel-root-payload'
            if embed_root:
                run([sys.executable, '-I', X9 / 'bluetooth/root_header.py', '--payload', payload,
                     '--apple-originals', apple,
                     *[arg for path in previous for arg in ('--previous-payload', path)]], log)
            else:
                run([sys.executable, '-I', X9 / 'bluetooth/root_header.py', '--stub'], log)
            stage = 'full-overlay-kernel'
            run([sys.executable, '-I', X9 / 'build/build.py', 'kernel', *common,
                 '--name', a.name + '/kernel', '--jobs', str(a.jobs)], log)
            stage = 'single-boot-image'
            run([sys.executable, '-I', X9 / 'build/build.py', 'boot', *common,
                 '--kernel', out / 'kernel/kernel.development.sweet', '--name', a.name + '/boot'], log)
            if embed_root:
                stage = 'rollback-boot-image'
                rollback = dict(opts, boot_args=['x9_bt_root=2' if t == 'x9_bt_root=1' else t
                                                 for t in opts['boot_args']])
                rollback_snapshot = inputs / 'boot-rollback.json'
                write_json(rollback_snapshot, rollback)
                run([sys.executable, '-I', X9 / 'build/build.py', 'boot', '--config', config_snapshot,
                     '--boot-config', rollback_snapshot, '--kernel', out / 'kernel/kernel.development.sweet',
                     '--name', a.name + '/boot-rollback'], log)
            if subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip() != commit:
                raise ValueError('source commit changed during the build; candidate is not published')
            if subprocess.run(['git', '-C', str(ROOT), 'diff', '--quiet', 'HEAD', '--', 'x9']).returncode:
                raise ValueError('tracked x9 source changed during the build; candidate is not published')
            stage = 'completion-publication'
            # Delay ALL ready markers until boot exits zero. Even termination
            # during an earlier component build cannot leave a misleading true.
            readiness(runtime / 'build.json', True)
            readiness(payload / 'root-payload.json', True)
            write_json(out / 'manifest.json', {
                'schema': 1, 'kind': 'x9-bluetooth-native-complete-candidate',
                'ios_build': '20H392',
                'commit': commit, 'xnu_commit': XNU_COMMIT, 'xnu_abi': a.abi, 'xnu_patches': patches,
                'built_complete': True, 'candidate_ready': True,
                'runtime_verified': False, 'audio_verified': False,
                'stageable': False, 'root_installed': False,
                'root_install': 'kernel-pid1' if embed_root else 'none',
                'root_install_boot_arg': 'x9_bt_root=1' if embed_root else None,
                'root_requires_user_confirmation': True, 'device_operation_performed': False,
                'power_interface': 'kern.x9_bt_power', 'power_lifecycle': 'closed-transport-only',
                'launchd_cache': cache_contract,
                'paths': {'kernel': 'kernel/kernel.development.sweet', 'runtime': 'runtime',
                          'root_payload': 'root-payload/root-payload.json', 'boot_image': 'boot/boot.img',
                          'rollback_boot_image': 'boot-rollback/boot.img' if embed_root else None,
                          'config': 'inputs/local.json', 'boot_config': 'inputs/boot.json',
                          'launchd_cache_original': 'inputs/launchd/' + CACHE_NAME,
                          'build_log': 'candidate-build.log'},
                'component_exit_status': {'full_overlay_kernel': 0, 'native_userland_jobs': 0,
                                          'root_payload': 0, 'boot_image': 0},
                'limitations': LIMITATIONS + ([
                    'x9_bt_root=1: PID1 installs the root files on first boot; the log line X9 BT ROOT is the only receipt',
                    'boot-rollback/boot.img (x9_bt_root=2) restores the .x9-orig Apple files and removes the four own files',
                ] if embed_root else []),
            })
            complete = True
    finally:
        if not complete:
            for metadata in (runtime / 'build.json', payload / 'root-payload.json'):
                try:
                    readiness(metadata, False)
                except (OSError, ValueError):
                    # An interrupted metadata write is not a complete product.
                    # Preserve it separately, while explicitly marking false.
                    if metadata.is_file():
                        metadata.rename(metadata.with_name(metadata.name + '.failed'))
                        write_json(metadata, {'schema': 1, 'candidate_ready': False,
                                   'runtime_verified': False, 'stageable': False,
                                   'incomplete_build_metadata': True})
            (out / 'manifest.json').unlink(missing_ok=True)
            # Preserve a failed packer's output without presenting it as the
            # final boot candidate, including a rejected oversized image.
            for image in (boot / 'boot.img', out / 'boot-rollback/boot.img'):
                if image.is_file():
                    image.rename(image.with_name('boot.img.failed'))
            write_json(out / 'failure.json', {'schema': 1, 'built_complete': False,
                       'candidate_ready': False, 'runtime_verified': False, 'stageable': False,
                       'failed_stage': stage, 'commit': commit, 'log': 'candidate-build.log'})
    print(f'Complete Bluetooth candidate built: {out / "boot/boot.img"}; '
          + ('root installs on first boot via x9_bt_root=1; rollback image boot-rollback/boot.img'
             if embed_root else 'root NOT installed') + '; iOS NOT validated', flush=True)


def interrupted(number, frame):
    del number, frame
    raise InterruptedError('build terminated')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name', required=True, help='NEW simple directory name inside x9/out/')
    ap.add_argument('--config', type=Path, required=True, help='repo-local config selecting isolated DXB')
    ap.add_argument('--boot-config', type=Path, required=True, help='complete production device boot arguments')
    ap.add_argument('--boot-arg', action='append', default=[], help='explicit additional token; repeatable')
    ap.add_argument('--launchd-source', type=Path, required=True, help='original 20H392 launch job directory')
    ap.add_argument('--launchd-cache-source', type=Path, required=True,
                    help='explicit repo-local original 20H392 version-7 launchd service cache')
    ap.add_argument('--apple-originals', type=Path,
                    help='private 20H392 bluetoothd/BlueTool originals; required with --boot-arg x9_bt_root=1')
    ap.add_argument('--previous-payload', type=Path, action='append', default=[],
                    help='earlier candidate root-payload already installed on the device; repeatable')
    ap.add_argument('--abi', choices=('macos', 'ios'), default='macos')
    ap.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 4))
    a = ap.parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    try:
        build(a)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, KeyboardInterrupt) as error:
        print(f'Bluetooth candidate build failed: {error}; no complete candidate published', file=sys.stderr)
        raise SystemExit(1)
