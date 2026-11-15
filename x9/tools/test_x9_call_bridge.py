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
    'late_cnf', 'sms_priority', 'staged_ios',
    'dial_golden', 'dial_invalid', 'dial_whitelist',
    'dial_roundtrip', 'dial_tx_failure', 'dial_cnf_invalid',
    'progress_framing', 'outbound_states', 'current_arrays', 'status_fence',
    'dial_join_order', 'ios_join', 'ios_timeout', 'ios_deleted', 'dial_presend_fence',
    'incoming_states', 'incoming_state_invalid', 'incoming_observer',
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

    def test_voice_not_advertised(self):
        source = (ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Baseband.cpp').read_text()
        table = source.partition('const x9bb_service x9bb_services[] = {')[2].partition('};')[0]
        self.assertTrue(table, 'service table must be reviewed if its declaration changes')
        services = [int(v, 16) for v in re.findall(r'\{\s*(0x[0-9a-fA-F]+)', table)]
        self.assertNotIn(9, services, 'VOICE cannot be published before Android flow is reconciled')


for _case in CASES:
    def _test(self, name=_case):
        self.run_case(name)
    setattr(CallBridgeTests, 'test_' + _case, _test)

if __name__ == '__main__':
    unittest.main()
