#!/usr/bin/env python3
"""Focused host checks for the opt-in Preferences Bluetooth diagnostic."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import shutil
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('bt_package', HERE / 'package.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
spec = importlib.util.spec_from_file_location('launch_cache', HERE / 'launch_cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)

# Preferred Mach-O VAs from the read-only 20H392 Settings audit. These are
# already nonzero shared-cache VAs; dli_fbase must never be added to them.
PREFERRED_TEXT = {
    'MobileBluetooth': 0x1b08fc000,
    'BluetoothManager': 0x1d4d0b000,
}
EXPECTED = {
    'BTAccessoryManagerAddCallbacks': (0x1b0900a78, [0xd10283ff, 0xa90567fa, 0xa9065ff8, 0xa90757f6]),
    'BTAccessoryManagerGetDefault': (0x1b08fdee8, [0xd10343ff, 0xa9095ff8, 0xa90a57f6, 0xa90b4ff4]),
    'BTLocalDeviceAddCallbacks': (0x1b0918c30, [0xd10283ff, 0xa90567fa, 0xa9065ff8, 0xa90757f6]),
    'BTLocalDeviceGetDefault': (0x1b08fdad4, [0xd10383ff, 0xa90967fa, 0xa90a5ff8, 0xa90b57f6]),
    'BTServiceAddCallbacks': (0x1b08fde4c, [0xa9bd57f6, 0xa9014ff4, 0xa9027bfd, 0x910083fd]),
    'BTSessionAttachWithQueue': (0x1b08fd88c, [0xd10183ff, 0xa9025ff8, 0xa90357f6, 0xa9044ff4]),
    '__btSessionEventCallback': (0x1d4d0c380, [0xd10283ff, 0xa90567fa, 0xa9065ff8, 0xa90757f6]),
}


class DiagnosticChecks(unittest.TestCase):
    def test_pinned_abi_and_forwarding(self):
        source = (HERE / 'X9SettingsBTDiag.c').read_text()
        found = re.findall(r'\{"([A-Za-z_]+)", (?:UINT64_C\((0x[0-9a-f]+)\)|(CALLBACK_VA)),\s*'
                           r'\{([^}]+)\}, ([A-Za-z_]+)\}', source)
        self.assertEqual(len(found), 7)
        self.assertEqual({name for name, *_ in found}, set(EXPECTED))
        for name, address, callback_va, words, wrapper in found:
            self.assertEqual(0x1d4d0c380 if callback_va else int(address, 16), EXPECTED[name][0])
            self.assertEqual([int(word, 16) for word in words.split(',')], EXPECTED[name][1])
            body = re.search(r'static (?:int|void) ' + wrapper + r'\([^)]*\)\s*\{(.*?)\n\}', source, re.S)
            self.assertIsNotNone(body)
            self.assertIn('originals[', body.group(1))
            if name == '__btSessionEventCallback':
                self.assertIn('((event_fn)originals[6])(service, first, second, manager);', body.group(1))
            else:
                self.assertRegex(body.group(1),
                                 r'int result = .*originals\[\d\].*;\n.*result=%d.*\n\s*return result;')
        self.assertNotRegex(source, r'\+0x18\]|\+ 0x18\]|#0x18')
        self.assertIn('(first | second) ? "" : " setup-eligible"', source)
        self.assertEqual(source.count('sys_icache_invalidate'), 2)
        self.assertEqual(source.count('VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY'), 2)
        self.assertNotIn('VM_PROT_ALL', source)

    def test_shared_cache_slide_and_exact_symbol_provenance(self):
        source = (HERE / 'X9SettingsBTDiag.c').read_text()
        self.assertIn('struct native_image { uintptr_t base; intptr_t slide; };', source)
        self.assertIn('out->base = (uintptr_t)info.dli_fbase;', source)
        self.assertIn('out->slide = _dyld_get_image_vmaddr_slide(index);', source)
        self.assertIn('(uintptr_t)symbol != expected', source)
        self.assertIn('(uintptr_t)info.dli_fbase != image->base', source)
        self.assertNotIn('mobile_base', source)
        self.assertNotIn('manager_base', source)
        # Model the audited nonzero preferred bases and both zero/nonzero ASLR
        # slides. Correct addresses are slide+VA, independent of dli_fbase+VA.
        for preferred in PREFERRED_TEXT.values():
            self.assertGreater(preferred, 0)
            for slide in (0, 0x40000000):
                actual_base = preferred + slide
                for name, (va, _) in EXPECTED.items():
                    if (name == '__btSessionEventCallback') != (preferred == PREFERRED_TEXT['BluetoothManager']):
                        continue
                    self.assertEqual(actual_base - preferred + va, slide + va)
                    self.assertNotEqual(actual_base + va, slide + va)

    def test_preferences_only_default_off_hash_gate(self):
        injector = (ROOT / 'x9/kernel/src/bsd/kern/x9_bt_settings_exec_env.inc').read_text()
        self.assertIn('sweet_x9_arg("x9_bt_settings_diag",0)!=1', injector)
        self.assertIn('"/Applications/Preferences.app/Preferences"', injector)
        self.assertIn('x9_btr_check(X9_BT_SETTINGS_DIAG_PATH,x9_btr_files[i].bytes,', injector)
        self.assertIn('2*(size_t)ptr_size', injector)
        self.assertIn('x9_btr_files[i].sha,x9_btr_files[i].mode', injector)
        self.assertIn('"DYLD_INSERT_LIBRARIES=",22', injector)
        self.assertIn('"X9_BT_SETTINGS_DIAG=",20', injector)
        self.assertIn('existing environment kept; diagnostic skipped', injector)
        source = (HERE / 'X9SettingsBTDiag.c').read_text()
        self.assertIn('strcmp(getprogname(), "Preferences")', source)
        self.assertIn('!enabled || strcmp(enabled, "1")', source)
        self.assertIn('sysctlbyname("kern.x9_bt_note"', source)
        self.assertIn('int saved_errno = errno;', source)
        self.assertIn('errno = saved_errno;', source)
        kernel = (ROOT / 'x9/kernel/src/bsd/kern/kern_exec.c').read_text()
        self.assertLess(kernel.index('#include "x9_bluetooth_root.inc"'),
                        kernel.index('#include "x9_bt_settings_exec_env.inc"'))
        self.assertIn('error = x9_bt_settings_exec_env(imgp, new_ptr_size);', kernel)
        candidate = (HERE / 'candidate.py').read_text()
        self.assertIn("'x9_bt_settings_diag=1'", candidate)
        self.assertIn("'x9_bt_settings_diag defaults off", candidate)

    def test_payload_hash_order_and_modes(self):
        out = ROOT / 'x9/out'
        out.mkdir(exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix='settings-diag-check-', dir=out))
        try:
            runtime, out = scratch / 'runtime', scratch / 'payload'
            runtime.mkdir()
            for directory in ('launchd-originals', 'launchd-candidates'):
                (runtime / directory).mkdir()
            for name in package.BINARIES:
                (runtime / name).write_bytes(('diagnostic-test:' + name).encode())
            originals, candidates = [], []
            for label, program, args, service, replacement in package.JOBS:
                stock = {'Label': label, 'UserName': 'mobile', 'Program': program,
                         'ProgramArguments': [program, *args], 'MachServices': {service: True}}
                raw = plistlib.dumps(stock, fmt=plistlib.FMT_BINARY, sort_keys=False)
                (runtime / 'launchd-originals' / (label + '.plist')).write_bytes(raw)
                (runtime / 'launchd-candidates' / (label + '.plist')).write_bytes(
                    plistlib.dumps(dict(stock, Program=replacement), fmt=plistlib.FMT_BINARY, sort_keys=False))
                originals.append((label + '.plist', raw, stock))
                candidates.append(dict(stock, Program=replacement))
            original_cache = b'bplist00-diagnostic-test-original-cache'
            candidate_cache = b'bplist00-diagnostic-test-candidate-cache'
            contract = {'path': cache.CACHE_PATH, 'method': 'unit-fixture',
                        'original_geometry': {'fixture': True}, 'candidate_geometry': {'fixture': True}}
            (runtime / 'launchd-originals' / cache.CACHE_NAME).write_bytes(original_cache)
            (runtime / 'launchd-candidates' / cache.CACHE_NAME).write_bytes(candidate_cache)
            build = {'schema': 1, 'ios_build': '20H392', 'bluetoothd_uuid': package.UUID,
                     'bluetoothd_sha256': package.STOCK_SHA, 'client': 'btctl',
                     'bridge': 'X9H4Transport.dylib', 'launch_gate': 'X9BluetoothLaunch',
                     'bluetool_adapter': 'X9BlueTool', 'settings_diagnostic': 'X9SettingsBTDiag.dylib',
                     'settings_diagnostic_default': False,
                     'settings_diagnostic_contract': {
                         'preferences_uuid': 'B2ED1830-FB2B-3702-B254-17872B2E0A97',
                         'manager_uuid': '54FB08F7-FE66-344D-B7CE-31838CDEE572',
                         'mobile_bluetooth_uuid': 'FDE5B98E-2257-35C8-B323-92B824492207',
                         'hook_count': 7, 'boot_arg': 'x9_bt_settings_diag=1',
                         'log_interface': 'kern.x9_bt_note', 'manager_state_modified': False},
                     'power_interface': 'kern.x9_bt_power',
                     'power_lifecycle': 'closed-transport-only',
                     'launchd': {'installed': False, 'originals': 'launchd-originals',
                                 'candidates': 'launchd-candidates',
                                 'cache': dict(contract,
                                               original='launchd-originals/' + cache.CACHE_NAME,
                                               candidate='launchd-candidates/' + cache.CACHE_NAME)},
                     'candidate_ready': False}
            (runtime / 'build.json').write_text(json.dumps(build))
            with patch.object(package, 'validate_pair', return_value=contract):
                package.package(runtime, out)
            manifest = json.loads((out / 'root-payload.json').read_text())
            entries = manifest['files']
            self.assertEqual(len(entries), 8)
            self.assertEqual(manifest['install_order'], [entry['path'] for entry in entries])
            self.assertEqual(entries[-1]['path'], cache.CACHE_PATH)
            self.assertFalse(manifest['settings_diagnostic_default'])
            self.assertEqual(manifest['settings_diagnostic'], package.SETTINGS_DIAG_PATH)
            self.assertEqual(manifest['settings_diagnostic_contract'], build['settings_diagnostic_contract'])
            diagnostic = next(entry for entry in entries if entry['path'] == package.SETTINGS_DIAG_PATH)
            data = (out / diagnostic['source']).read_bytes()
            self.assertEqual(diagnostic['precondition'], {'absent': True})
            self.assertEqual(diagnostic['mode'], '0755')
            self.assertEqual(diagnostic['sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual(diagnostic['size'], len(data))
            self.assertEqual([entry['mode'] for entry in entries], ['0755'] * 5 + ['0644'] * 3)
        finally:
            shutil.rmtree(scratch)


if __name__ == '__main__':
    unittest.main()
