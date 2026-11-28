#!/usr/bin/env python3
"""Focused host checks for the opt-in BTAudio XPC read-only recorder."""
import re
import unittest

from pathlib import Path

HERE = Path(__file__).resolve().parent

DRIVER_UUID = '9899D1C1-2F49-3AAB-9223-DFAD564CFE2E'
DAEMON_UUID = 'AA9424E8-0648-34E2-B3CF-E6C793B9D471'
TARGETS = {
    '_xpc_dictionary_get_int64': 'rep_get_int64',
    '_xpc_dictionary_get_string': 'rep_get_string',
    '_xpc_dictionary_get_value': 'rep_get_value',
    '_xpc_dictionary_apply': 'rep_apply',
}


def uuid_bytes(text):
    flat = text.replace('-', '')
    return [int(flat[i:i + 2], 16) for i in range(0, len(flat), 2)]


class BTAudioDiagChecks(unittest.TestCase):
    def setUp(self):
        self.source = (HERE / 'X9BTAudioDiag.c').read_text()

    def test_pinned_images_and_process_gate(self):
        self.assertIn(f'#define DRIVER_UUID "{DRIVER_UUID}"', self.source)
        self.assertIn(f'#define BTDAEMON_UUID "{DAEMON_UUID}"', self.source)
        for name, text in (('driver_uuid', DRIVER_UUID), ('daemon_uuid', DAEMON_UUID)):
            body = re.search(name + r'\[16\] = \{([^}]+)\}', self.source)
            self.assertIsNotNone(body)
            self.assertEqual([int(x, 16) for x in body.group(1).split(',')],
                             uuid_bytes(text))
        self.assertIn('strcmp(prog, "coreaudiod") && strcmp(prog, "bluetoothd")',
                      self.source)
        self.assertIn('getenv("X9_BT_AUDIO_DIAG")', self.source)
        self.assertIn('!enabled || strcmp(enabled, "1")', self.source)
        self.assertIn('_dyld_register_func_for_add_image(on_image);', self.source)
        self.assertIn('image_uuid_is(header, driver_uuid)', self.source)
        self.assertIn('image_uuid_is(header, daemon_uuid)', self.source)

    def test_targets_forward_first_and_return_native_result(self):
        found = re.findall(r'\{"(_xpc_dictionary_[a-z0-9_]+)", \(void \*\)(rep_[a-z0-9_]+),',
                           self.source)
        self.assertEqual(dict(found), TARGETS)
        for wrapper, original in (('rep_get_int64', 'orig_get_int64'),
                                  ('rep_get_string', 'orig_get_string'),
                                  ('rep_get_value', 'orig_get_value'),
                                  ('rep_apply', 'orig_apply')):
            body = re.search(re.escape(wrapper) + r'\([^)]*\)\s*\{(.*?)\n\}',
                             self.source, re.S)
            self.assertIsNotNone(body, wrapper)
            text = body.group(1)
            self.assertIn(original + ' ? ' + original, text)
            self.assertIn('maybe_dump(dict);', text)
            self.assertLess(text.index(original + '(dict'), text.index('maybe_dump'))
            self.assertIn('int saved_errno = errno;', text)
            self.assertIn('errno = saved_errno;', text)
            self.assertIn('return result;', text)
        self.assertEqual(self.source.count('return result;'), 4)

    def test_read_only_no_dictionary_mutation(self):
        self.assertNotRegex(self.source, r'xpc_dictionary_set_|xpc_dictionary_remove_value'
                                         r'|xpc_dictionary_set_value')
        self.assertNotIn('sys_icache_invalidate', self.source)
        self.assertNotIn('VM_PROT_ALL', self.source)
        self.assertNotRegex(self.source, r'\*\s*dict\s*=')
        dump = re.search(r'static void maybe_dump.*?\n\}\n', self.source, re.S)
        self.assertIsNotNone(dump)
        self.assertIn('xpc_dictionary_get_value(dict, "kBTAudioMsgId")', dump.group(0))

    def test_envelope_gating_caps_and_shmem(self):
        self.assertIn('#define MAX_DUMPS 16', self.source)
        self.assertIn('#define DUMP_LINE_CAP 220', self.source)
        self.assertIn('already_dumped(dict, xpc_int64_get_value(id))', self.source)
        self.assertIn('SEEN_WINDOW_SEC 2', self.source)
        self.assertIn('xpc_shmem_map(value, &region)', self.source)
        self.assertIn('shmem len=%zu head=%s', self.source)
        # The dump is generic over whatever keys exist; no property key is
        # baked into the recorder, so future/unknown keys are captured too.
        self.assertNotIn('"kBTAudioMsgProperty', self.source)
        self.assertIn('"kBTAudioMsgDeviceUid"', self.source)
        self.assertIn('"kBTAudioMsgMethod"', self.source)
        self.assertIn('"kBTAudioMsgProcess"', self.source)
        self.assertIn('msg dict=%p id=%lld uid=%s method=%lld process=%s', self.source)

    def test_rebind_scope_and_slot_write_safety(self):
        self.assertIn('"__la_symbol_ptr"', self.source)
        self.assertIn('"__got"', self.source)
        self.assertIn('index & 0xc0000000u', self.source)
        self.assertIn('symbols[index].n_un.n_strx', self.source)
        self.assertIn('VM_PROT_READ | VM_PROT_WRITE', self.source)
        body = re.search(r'static int write_slot.*?\n\}\n', self.source, re.S)
        self.assertIsNotNone(body)
        self.assertIn('if (kr) return -1;', body.group(0))
        self.assertIn('VM_PROT_READ);', body.group(0))
        self.assertIn('if (!*targets[t].original) *targets[t].original = current;',
                      self.source)

    def test_log_channels_and_prefix(self):
        self.assertIn('sysctlbyname("kern.x9_bt_note"', self.source)
        self.assertIn('os_log_error(OS_LOG_DEFAULT, "X9 BTAUDIO: %{public}s", text);',
                      self.source)
        self.assertIn('recorder armed process=%s; values and flow unchanged', self.source)
        self.assertIn('hooked %s image=%p rebound=%d of %zu; values and flow unchanged',
                      self.source)

    def test_build_wiring_and_contract(self):
        build = (HERE / 'build.py').read_text()
        self.assertIn("audio_diagnostic = a.out / 'X9BTAudioDiag.dylib'", build)
        self.assertIn("'-install_name', '/usr/lib/X9BTAudioDiag.dylib',", build)
        self.assertIn("str(HERE / 'X9BTAudioDiag.c'), '-o', str(audio_diagnostic)", build)
        self.assertIn("'-fblocks', '-dynamiclib',", build)
        self.assertIn("'audio_diagnostic': 'X9BTAudioDiag.dylib', "
                      "'audio_diagnostic_default': False,", build)
        self.assertIn("'driver_uuid': '9899D1C1-2F49-3AAB-9223-DFAD564CFE2E',", build)
        self.assertIn("'bluetoothd_uuid': 'AA9424E8-0648-34E2-B3CF-E6C793B9D471',", build)
        self.assertIn("'hook_count': 4, 'boot_arg': 'x9_bt_audio_diag=1',", build)
        self.assertIn("'log_interface': 'kern.x9_bt_note', 'values_modified': False,",
                      build)


if __name__ == '__main__':
    unittest.main()
