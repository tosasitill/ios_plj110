#!/usr/bin/env python3
"""Freeze the opt-in GPU I/O-coherency cache-skip candidate on pan207 build inputs."""
from pathlib import Path
import argparse
import difflib
import json
import shlex
import shutil
import subprocess

w = Path(__file__).resolve().parent
root = w.parents[1]
p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, default=root / 'x9/out/pan209-gpu-coherent-skip')
out = p.parse_args().out.resolve()
base = root / 'x9/out/pan207-import-cache-ranges'
native = root / 'MaliMetal/X9/General'
previous = json.loads((base / 'reference-inputs.json').read_text())

# Kernel-only candidate. Both .inc files are compiled by exactly one unit.
kernel_changed = ['SweetX9CSFResident.inc', 'SweetX9RenderEngine.inc']
changed = ['kernel/' + name for name in kernel_changed]
for name in changed:
    if (base / 'metal-source' / name).read_text() == (native / name).read_text():
        raise RuntimeError('No source delta for ' + name)

# The 607-byte boot line is full (600 bytes). startup_debug is stock osfmk
# startup-phase diagnostics (a TUNABLE bool present in the linked kernel), not
# an X9 functional gate: no X9-owned source reads it. Dropping it is the only
# inert way to make room for the explicit coherency opt-in; -v, debug=0xc,
# serial=3 and every X9 gate stay.
boot = (base / 'boot-args.txt').read_text().strip()
if boot.split().count('startup_debug=1') != 1:
    raise RuntimeError('Expected the diagnostic startup_debug argument exactly once')
for tree in ['kernel-source/iokit/Kernel/arm', 'kernel-source/bsd/dev', 'shim',
             'packaging-source/x9/tools']:
    hit = subprocess.run(['/usr/bin/grep', '-rlI', 'startup_debug', str(base / tree)],
                         capture_output=True, text=True)
    if hit.stdout.strip():
        raise RuntimeError('X9-owned source reads startup_debug; not droppable: ' + tree)
new_args = ' '.join(a for a in boot.split() if a != 'startup_debug=1') + ' x9_gpu_coherent=1'
if new_args.split().count('x9_gpu_coherent=1') != 1 or len(new_args.encode()) > 607:
    raise RuntimeError('Boot argument activation or 607-byte budget violated')
if 'x9_metal_render=1' not in new_args.split() or 'x9_statusbar_provider=1' not in new_args.split():
    raise RuntimeError('Render session and statusbar seeding must stay enabled')

out.mkdir(exist_ok=False)
for name in ['kernel-source', 'packaging-source', 'build-tools', 'generated', 'firmware',
             'compute-source', 'kernel-objects', 'plugin-package']:
    subprocess.run(['/bin/cp', '-cR', str(base / name), str(out / name)], check=True)
shutil.copytree(native, out / 'metal-source')
for name in kernel_changed:
    shutil.copyfile(native / 'kernel' / name, out / 'kernel-source/iokit/Kernel/arm' / name)
with (out / 'pan209-gpu-coherent-skip.patch').open('w') as patch:
    for name in changed:
        old = (base / 'metal-source' / name).read_text()
        new = (native / name).read_text()
        patch.writelines(difflib.unified_diff(old.splitlines(True), new.splitlines(True),
            fromfile='a/MaliMetal/X9/General/' + name, tofile='b/MaliMetal/X9/General/' + name))

build_sh = (base / 'build.sh').read_text()
if build_sh.count(boot) != 1 or shlex.split(build_sh)[shlex.split(build_sh).index('--boot-args') + 1] != boot:
    raise RuntimeError('Base build script boot arguments disagree with boot-args.txt')
(out / 'build.sh').write_text(build_sh.replace(boot, new_args))
(out / 'boot-args.txt').write_text(new_args + '\n')
for name in ['root-manifest.json', 'build-kernel.py', 'build-driver.py',
             'external-metal-verification.patch', 'x9-metal-plugins-ios.tar.gz',
             'unixnu-reference-inputs.json']:
    shutil.copyfile(base / name, out / name)
for name in previous['runtime_patch_order'] + ['pan207-import-cache-ranges.patch']:
    shutil.copyfile(base / name, out / name)
shutil.copyfile(base / 'reference-inputs.json', out / 'pan207-reference-inputs.json')

records = json.loads((base / 'commands.json').read_text().replace(str(base), str(out)))
(out / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
(out / 'kernel-link.filelist').write_text((base / 'kernel-link.filelist').read_text().replace(str(base), str(out)))
resume = [r for r in records if any(arg.endswith('/SweetX9CSF.cpp') for arg in r['args'])]
if len(resume) != 1:
    raise RuntimeError('Expected exactly one SweetX9CSF compilation unit')
(out / 'resume-commands.json').write_text(json.dumps(resume + records[-2:], indent=2) + '\n')
(out / 'resume-build.py').write_text((out / 'build-kernel.py').read_text().replace("out/'commands.json'", "out/'resume-commands.json'"))
shutil.copyfile(Path(__file__), out / 'prepare-pan209.py')

meta = dict(previous)
meta.update({
    'candidate': out.name, 'base': str(base),
    'build_status': 'PREPARED', 'runtime_status': 'NOT RUN',
    'installation_status': 'NOT STAGED; owner flashes manually',
    'changed_runtime_files': [], 'changed_native_files': changed,
    'native_kernel_changes': kernel_changed, 'native_patch': 'pan209-gpu-coherent-skip.patch',
    'inherited_native_kernel_changes': previous['native_kernel_changes'] + previous['inherited_native_kernel_changes'],
    'base_choice': 'pan207, not pan208: pan208 only replaced the seeded UIKit statusbar provider with _UIStatusBarVisualProvider_Phone and the owner reported no visible status bar. Rebuilding on207 re-seeds the previous LegacyPhone value over the persisted plist, so this candidate carries no unverified statusbar experiment',
    'plugin_revision': 207,
    'plugin_reuse': 'Exact207 plugin package, allowlist-derived manifest and generated external-file header reused; no plugin source, binary or identity change',
    'boot_arguments': {'bytes': len(new_args.encode()), 'limit': 607,
                       'added': ['x9_gpu_coherent=1'], 'removed': ['startup_debug=1'],
                       'removal_reason': 'stock osfmk startup-phase diagnostic bool (TUNABLE symbol present in the linked kernel); no X9-owned source in iokit/Kernel/arm, bsd/dev, shim or the packaging tools reads it. Removing it is what keeps the explicit opt-in inside the 607-byte boot line; verbose console, debug=0xc and serial=3 are unchanged',
                       'unchanged': 'every other argument, including x9_metal_render=1 and x9_statusbar_provider=1'},
    'inherited_implementation': previous['implementation'],
    'implementation': {
        'scope': 'Opt-in GPU I/O-coherency skip of per-submit client cache maintenance. No AS residency, no async submission, no change to command/fence storage syncs',
        'gate': 'resident_begin activates ResidentSession::coherent only when boot argument x9_gpu_coherent=1 is present AND COHERENCY_NUM_GROUPS(0x300) is 1..64 AND COHERENCY_GROUP_0(0x308) type is ACE-Lite(1) or ACE(2). Anything else leaves the existing full fallback in force',
        'pre_submit': 'render_submit skips the CIVAC bounding-interval clean for cpu_exposed resources only. Never-exposed allocations keep their one-time whole-BO initial clean, so initial_cache_clean is still set only by a real full Clean in x9_render_memory_cache',
        'post_submit': 'Same skip for GPUWrite cpu_exposed resources after group halt, flush_gpu and AS detach. The disabled/uncertain poisoning path is untouched because it is only reachable from the maintenance it replaces',
        'retained': 'Explicit client X9RenderCache selector operations, GPU flush, fence pages, private storage syncs, group start/halt, health checks and every uncertainty branch are unchanged',
        'native_basis': 'Matches mesa panvk/panthor semantics where pan_kmod_queue_bo_map_sync early-returns for IO_COHERENT BOs and the kernel performs no per-submit re-clean; flag documentation in lib/kmod/pan_kmod.h',
        'observability': 'X9 RENDER coherency at session start logs groups/type/requested/active. X9 RENDER cache-coherent every2s logs active plus pre/post skipped byte averages, so a disabled gate is visible as active=0 with unchanged cache bytes',
    },
    'remaining_constraints': [
        'NOT RUN: compiled and packaged only. No FPS, tearing, corruption or latency claim',
        'A non-zero COHERENCY_GROUP_0 type is a GPU register capability, not proof that this MT6993 interconnect snoops CPU caches for client addresses. The opt-in is the operator claim; if it is wrong, GPU reads can see stale lines and CPU reads can see stale GPU output',
        'The gate defaults to off. Without x9_gpu_coherent=1 behaviour is byte-identical to207 apart from two added log counters',
        '208 UIKit _UIStatusBarVisualProvider_Phone experiment is not carried; the persisted plist is overwritten with the207 LegacyPhone value on an enabled boot',
        'startup_debug=1 is no longer passed, so stock osfmk startup-phase prints are absent from this candidate. Verbose console, debug=0xc, serial=3, X9 milestone logs, disk sidecar and pstore are unchanged',
        'No writable-host-import fix, no CSG/AS residency, no async submission, no Apple byte change, no root-disk or device write',
        'Owner flashes and tests manually; no tests, no post-build artifact inspection, no flash or reboot performed here',
    ],
    'inherited_constraints': previous['remaining_constraints'] + previous['inherited_constraints'],
    'panvk_status': 'Unchanged207 bundle reused; not rebuilt',
    'plugins': str(out / 'x9-metal-plugins-ios.tar.gz'),
    'exact_file_manifest': str(out / 'plugin-package/manifest.json'),
    'replay_build': {'cwd': str(root),
        'prepare_new_candidate': ['python3', str(w / 'prepare-pan209.py'), '--out', 'NEW_OUTPUT_DIRECTORY'],
        'kernel_and_boot': ['python3', str(out / 'build-driver.py')]},
    'build_evidence': {'kernel_log': str(out / 'kernel-build.log'), 'package_log': str(out / 'package-build.log'),
        'normal_build_completed': False, 'extra_tests_run': False, 'post_build_binary_checks_run': False,
        'mailbox_payload_bytes': previous['build_evidence']['mailbox_payload_bytes']},
    'hardware_evidence': {'candidate': out.name, 'loaded': False, 'status': 'NOT OBSERVED',
                         'coherency_registers': None, 'cache_coherent_active': None,
                         'pre_cache_us': None, 'post_cache_us': None, 'image_correctness': None},
    'deployment': {'boot': str(out / 'boot.img'), 'plugins': str(out / 'x9-metal-plugins-ios.tar.gz'),
        'mailbox_committed': False, 'boot_flash': 'Not performed; owner only'},
    'installation_evidence': {'updater_executed': False, 'mailbox_written': False,
        'boot_partition_written': False, 'rebooted': False, 'reason': 'Owner flashes candidates manually'},
})
(out / 'reference-inputs.json').write_text(json.dumps(meta, indent=2, ensure_ascii=False) + '\n')
print('Prepared ' + str(out) + ';207 plugins reused, boot args ' + str(len(new_args.encode())) + ' bytes', flush=True)
