"""Offline tests of the exact production call codec / MIPC owner; no device IO.

Synthetic vectors intentionally use distinct approve/call-id/sequence values.
No boot image, Apple/vendor binary, captured number or .local input is needed.
Compiler is configurable via CXX, otherwise Xcode clang++; output stays in
gitignored x9/out/call-ios-bridge/host-tests (project worktree convention).
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'x9/out/call-ios-bridge/host-tests'
CASES = (
    'codec_golden', 'codec_invalid', 'incoming_routes', 'action_codec', 'request_whitelist',
    'no_auto_policy', 'command_roundtrip', 'failure_retained',
    'duplicate_pending', 'changed_duplicate', 'duplicate_replied',
    'overflow_isolated', 'bounded_tombstones', 'fifo', 'fragmented',
    'late_cnf', 'finish_reason_timeout', 'sms_priority', 'staged_ios', 'voice20_dispatch',
    'calllist_client_isolation', 'calllist_stale_publication', 'calllist_generation_fence',
    'dial_golden', 'dial_invalid', 'dial_whitelist',
    'dial_roundtrip', 'dial_tx_failure', 'dial_cnf_invalid',
    'progress_framing', 'outbound_states', 'finish_reason_diagnostic', 'sip_diagnostic_isolation',
    'dial_ims_snapshot', 'end_reason_provenance', 'outbound_end_reason', 'incoming_end_reason',
    'current_arrays', 'status_fence',
    'dial_join_order', 'ios_join', 'ios_timeout', 'ios_deleted', 'dial_presend_fence', 'dial_presend_release',
    'dial_guard_admission_release', 'dial_guard_admission_timeout',
    'dial_guard_writer_release', 'dial_guard_writer_timeout',
    'dial_guard_selected_release', 'dial_guard_selected_timeout',
    'incoming_states', 'incoming_state_invalid', 'incoming_observer',
    'voice_bind_codec', 'end_target_codec', 'ios_end_roundtrip',
    'current_metadata', 'current_number_owner', 'progress_number_owner',
    'ios_end_stale_query', 'ios_end_presend', 'ios_end_abandon',
    'ios_answer_receipt', 'ios_end_late_cnf', 'ios_end_duplicate', 'ios_end_malformed_duplicate', 'ios_answer_malformed_duplicate',
    'register_error_owner_retained', 'register_malformed_duplicate', 'register_codec', 'register_transition', 'register_sequence',
    'register_timeout', 'register_deleted', 'register_unsupported',
    'handover_codec', 'handover_concurrent_bootstrap',
    'handover_repeat_enable_cursor', 'handover_subscription_fence', 'handover_pending_timeout', 'handover_rx_fence',
    'handover_pending_overflow', 'handover_transport_loss', 'handover_request_identity',
)


class CallBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        compiler = shlex.split(os.environ.get('CXX', 'clang++'))
        flags = ['-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-Wall', '-Wextra',
                 '-Wno-unused-function', '-Wno-unused-variable']
        cls.binary = OUT / 'call_bridge_host'
        cls.command = compiler + flags + [str(ROOT / 'x9/tools/tests/call_bridge_host.cpp'),
                                          '-o', str(cls.binary)]
        p = subprocess.run(cls.command, capture_output=True, text=True)
        (OUT / 'compile.log').write_text(shlex.join(cls.command) + '\n' + p.stdout + p.stderr)
        if p.returncode:
            raise RuntimeError(p.stderr)
        # Compile the shared header in C as well as C++ (kernel transport is C).
        c = OUT / 'codec_c.c'
        c.write_text('#include "' + str(ROOT / 'x9/kernel/src/bsd/dev/x9_call_state.h') +
                     '"\nint main(void){return !x9cw_frame(0,0,0);}\n')
        subprocess.run(compiler + ['-x', 'c', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                   '-fsyntax-only', str(c)], check=True, capture_output=True)

    def run_case(self, name):
        p = subprocess.run([str(self.binary), name], capture_output=True, text=True)
        (OUT / (name + '.log')).write_text(p.stdout + p.stderr)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)

    def test_voice_advertised_once(self):
        source = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Baseband.cpp').read_text()
        table = source.partition('const x9bb_service x9bb_services[] = {')[2].partition('};')[0]
        self.assertTrue(table, 'service table must be reviewed if its declaration changes')
        services = [int(v, 16) for v in re.findall(r'\{\s*(0x[0-9a-fA-F]+)', table)]
        self.assertEqual(services.count(9), 1, 'VOICE09 must have one real service entry')


for _case in CASES:
    def _test(self, name=_case):
        self.run_case(name)
    setattr(CallBridgeTests, 'test_' + _case, _test)

if __name__ == '__main__':
    unittest.main()
