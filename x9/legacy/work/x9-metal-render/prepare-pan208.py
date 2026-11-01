#!/usr/bin/env python3
"""Freeze the owner's UIKit preference patch on current pan207 build inputs."""
from pathlib import Path
import argparse
import difflib
import json
import shlex
import shutil
import subprocess

w = Path(__file__).resolve().parents[3] / '.local/x9/work/x9-metal-render'
root = w.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, default=root / 'x9/out/pan208-uikit-statusbar')
out = parser.parse_args().out.resolve()
base = root / 'x9/out/pan207-import-cache-ranges'
patch = root / 'xnu/patches/0217-x9-native-uikit-statusbar-preference.patch'
target = Path('kernel-source/bsd/kern/kern_exec.c')
original = (base / target).read_text()
patch_text = patch.read_text()

# pan207's current SOURCE inputs already contain an older LegacyPhone helper
# and the callsite. Preserve that source separately; do not duplicate either
# or pretend the insertion-only patch directly applies to this modified base.
hunks = patch_text.split('\n@@ ')
if len(hunks) != 3 or '+++ b/bsd/kern/kern_exec.c' not in hunks[0].splitlines():
    raise RuntimeError('Unexpected supplied patch targets or hunk count')
added = ['\n'.join(line[1:] for line in hunk.splitlines()[1:] if line.startswith('+')) + '\n'
         for hunk in hunks[1:]]
helper, hook = added
name = 'sweet_x9_seed_uikit_statusbar(proc_t p, user_addr_t scratch, user_addr_t end)'
if original.count(name) != 1 or helper.count(name) != 1 or original.count(hook) != 1:
    raise RuntimeError('Expected exactly one existing helper and the supplied callsite')
function = original.index(name)
begin = original.rfind('/* Original 20H392 UIKit __UIStatusBarGetVisualProviderClassForScreen reads', 0, function)
end = original.index('\n}\n', function) + len('\n}\n')
if begin < 0 or 'static int\n' not in original[begin:function]:
    raise RuntimeError('Unexpected helper boundary')
updated = original[:begin] + helper.rstrip('\n') + '\n' + original[end:]
if updated == original:
    raise RuntimeError('Supplied helper is already present; no new source delta')
args = shlex.split((base / 'build.sh').read_text())
boot_args = args[args.index('--boot-args') + 1]
if boot_args != (base / 'boot-args.txt').read_text().strip():
    raise RuntimeError('Existing boot argument inputs disagree')
if boot_args.split().count('x9_statusbar_provider=1') != 1 or len(boot_args.encode()) > 607:
    raise RuntimeError('Statusbar opt-in missing/duplicated or boot argument limit exceeded')

out.mkdir(exist_ok=False)
for name in ['kernel-source', 'packaging-source', 'build-tools', 'generated',
             'firmware', 'compute-source', 'kernel-objects', 'metal-source', 'plugin-package']:
    subprocess.run(['/bin/cp', '-cR', str(base / name), str(out / name)], check=True)
for name in ['root-manifest.json', 'boot-args.txt', 'build.sh', 'build-kernel.py',
             'build-driver.py', 'external-metal-verification.patch',
             'x9-metal-plugins-ios.tar.gz', 'unixnu-reference-inputs.json']:
    shutil.copyfile(base / name, out / name)
shutil.copyfile(patch, out / patch.name)
shutil.copyfile(base / target, out / 'kern_exec.pan207-input.c')
(out / target).write_text(updated)
delta = ''.join(difflib.unified_diff(original.splitlines(True), updated.splitlines(True),
    fromfile='a/bsd/kern/kern_exec.c', tofile='b/bsd/kern/kern_exec.c'))
(out / 'pan208-applied-statusbar.patch').write_text(delta)

previous = json.loads((base / 'reference-inputs.json').read_text())
for name in previous['runtime_patch_order'] + ['pan207-import-cache-ranges.patch']:
    shutil.copyfile(base / name, out / name)
shutil.copyfile(base / 'reference-inputs.json', out / 'pan207-reference-inputs.json')
records = json.loads((base / 'commands.json').read_text().replace(str(base), str(out)))
(out / 'commands.json').write_text(json.dumps(records, indent=2) + '\n')
(out / 'kernel-link.filelist').write_text((base / 'kernel-link.filelist').read_text().replace(str(base), str(out)))
compile_exec = [r for r in records if any(arg.endswith('/kern_exec.c') for arg in r['args'])]
if len(compile_exec) != 1:
    raise RuntimeError('Expected one kern_exec compilation unit')
(out / 'resume-commands.json').write_text(json.dumps(compile_exec + records[-2:], indent=2) + '\n')
(out / 'resume-build.py').write_text((out / 'build-kernel.py').read_text().replace("out/'commands.json'", "out/'resume-commands.json'"))
shutil.copyfile(Path(__file__), out / 'prepare-pan208.py')

meta = dict(previous)
meta.update({
    'candidate': out.name, 'base': str(base),
    'build_status': 'PREPARED', 'runtime_status': 'NOT RUN', 'installation_status': 'NOT STAGED; build-only request',
    'changed_native_files': [], 'changed_runtime_files': [],
    'native_kernel_changes': ['bsd/kern/kern_exec.c'],
    'inherited_native_kernel_changes': previous['native_kernel_changes'] + previous['inherited_native_kernel_changes'],
    'native_patch': 'pan208-applied-statusbar.patch', 'owner_patch': patch.name,
    'patch_application': 'Current207 source already contained LegacyPhone helper and identical hook. Replaced entire helper with supplied first-hunk additions; retained existing single hook after SpringBoard seed and external plugin setup. Owner patch unmodified; exact derived delta and original source retained',
    'plugin_revision': 207,
    'plugin_reuse': 'Exact207 plugin package and generated allowlist reused; no plugin source/binary change or new plugin identity',
    'boot_arguments': {'bytes': len(boot_args.encode()), 'limit': 607, 'changed_from_current_base_inputs': False,
                       'statusbar_provider': 1, 'cpus': 8},
    'inherited_implementation': previous['implementation'],
    'implementation': {
        'helper': 'Owner-supplied UIKit preference helper; _UIStatusBarVisualProvider_Phone instead of current-source LegacyPhone',
        'activation': 'x9_statusbar_provider=1 already in current207 source build arguments; preserved in208',
        'filesystem_effect': 'At owner boot on writable native root, backs up an existing regular plist with a hard link if backup absent, writes temporary0600 uid/gid501 plist then renames. It replaces the suite, not a merge. Failure logs and continues boot',
        'target': '/private/var/mobile/Library/Preferences/com.apple.UIKit.plist',
        'backup': '/private/var/mobile/Library/Preferences/com.apple.UIKit.x9-before-seed.plist',
        'device_geometry': 'Unchanged; no Apple executable byte patch',
        'gpu': '207 mapping-bound cache optimization and GPU synchronization unchanged',
    },
    'remaining_constraints': [
        'NOT RUN: compilation/packaging does not prove UIKit selects the requested provider or corrects statusbar appearance',
        'Kernel helper replaces the whole UIKit preference suite on enabled boot; existing backup is not overwritten. No preference was written by this host build',
        '208 boot continues to require existing207 Metal and PanVK bundles selected by unchanged verified allowlist',
        'No60FPS/120Hz, full GPU/CI/capture parity, rendering corruption or app-transition reboot fix claimed',
        'No tests, extra post-build artifact inspection, device installation, flash or reboot requested/performed',
    ],
    'inherited_constraints': previous['remaining_constraints'] + previous['inherited_constraints'],
    'panvk_status': 'Unchanged207 bundle reused; not rebuilt',
    'plugins': str(out / 'x9-metal-plugins-ios.tar.gz'),
    'exact_file_manifest': str(out / 'plugin-package/manifest.json'),
    'replay_build': {'cwd': str(root),
        'prepare_new_candidate': ['python3', str(w / 'prepare-pan208.py'), '--out', 'NEW_OUTPUT_DIRECTORY'],
        'kernel_and_boot': ['python3', str(out / 'build-driver.py')]},
    'build_evidence': {'kernel_log': str(out / 'kernel-build.log'), 'package_log': str(out / 'package-build.log'),
        'normal_build_completed': False, 'extra_tests_run': False, 'post_build_binary_checks_run': False,
        'mailbox_payload_bytes': previous['build_evidence']['mailbox_payload_bytes']},
    'hardware_evidence': {'candidate': out.name, 'loaded': False, 'status': 'NOT OBSERVED',
                         'statusbar_preference': None, 'statusbar_appearance': None},
    'deployment': {'boot': str(out / 'boot.img'), 'plugins': str(out / 'x9-metal-plugins-ios.tar.gz'),
        'mailbox_committed': False, 'boot_flash': 'Not performed; owner only'},
    'installation_evidence': {'updater_executed': False, 'mailbox_written': False,
        'boot_partition_written': False, 'rebooted': False, 'reason': 'User requested patch/build only'},
})
(out / 'reference-inputs.json').write_text(json.dumps(meta, ensure_ascii=False, indent=2) + '\n')
print('Prepared ' + str(out) + '; exact owner helper applied,207 plugins reused', flush=True)
