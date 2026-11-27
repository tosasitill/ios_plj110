#!/usr/bin/env python3
"""Host checks for the 433 teardown null-slot guard in X9H4Transport.c.

The audited 20H392 constants below come from read-only disassembly of the
stock bluetoothd (UUID AA9424E8-0648-34E2-B3CF-E6C793B9D471): the teardown
function at 0x1000f3b44, the transport thunk at 0x1000e6aac, and the H4
table at 0x100844228 whose +0x60 slot is NULL while +0x20 is 0x1000685c0.
"""
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = (HERE / 'X9H4Transport.c').read_text()

TEARDOWN_WORDS = [0xa9bf7bfd, 0x910003fd, 0x97ffcbd8, 0x94027bea]
THUNK_WORDS = [0xd0003ae8, 0xf9442908, 0xb9400109, 0x34000069,
               0xf9403100, 0xd61f0000]
CALL_VA = 0x1000f3b4c
THUNK_VA = 0x1000e6aac
TABLE_GLOBAL_VA = 0x100844850


def encode_bl(call, target):
    offset = target - call
    if offset % 4 or not (-(1 << 27) <= offset < (1 << 27)):
        return None
    return 0x94000000 | ((offset // 4) & 0x3FFFFFF)


def decode_bl(word):
    imm = word & 0x3FFFFFF
    if imm & (1 << 25):
        imm -= 1 << 26
    return imm * 4


class TeardownGuard(unittest.TestCase):
    def test_contract_words_pinned(self):
        squashed = re.sub(r'\s+', '', SOURCE)
        for words in (TEARDOWN_WORDS, THUNK_WORDS):
            text = '{' + ','.join('0x%08x' % w for w in words) + '}'
            self.assertIn(text, squashed)
        self.assertIn('0x1000f3b44', SOURCE)
        self.assertIn('0x1000e6aac', SOURCE)
        self.assertIn('0x100844850', SOURCE)

    def test_stock_bl_encoding_matches_audit(self):
        # The stock BL at the call site targets the thunk exactly.
        stock = TEARDOWN_WORDS[2]
        self.assertEqual(stock >> 26, 0x25)  # BL opcode
        self.assertEqual(CALL_VA + decode_bl(stock), THUNK_VA)
        self.assertEqual(encode_bl(CALL_VA, THUNK_VA), stock)

    def test_bl_range_boundaries(self):
        self.assertIsNotNone(encode_bl(CALL_VA, CALL_VA + (1 << 27) - 4))
        self.assertIsNotNone(encode_bl(CALL_VA, CALL_VA - (1 << 27)))
        self.assertIsNone(encode_bl(CALL_VA, CALL_VA + (1 << 27)))
        self.assertIsNone(encode_bl(CALL_VA, CALL_VA - (1 << 27) - 4))
        self.assertIsNone(encode_bl(CALL_VA, CALL_VA + 2))

    def test_guard_is_callsite_local_and_fail_closed(self):
        # Only the single BL is rewritten; the table and the thunk's other
        # caller are never referenced as patch targets.
        self.assertIn('*(uint32_t *)call=word;', SOURCE)
        # The thunk's second caller is named once, in the design comment,
        # and is never a patch target.
        self.assertEqual(SOURCE.count('0x1003ca25c'), 1)
        self.assertIn('offset<-(INT64_C(1)<<27)', SOURCE)
        self.assertIn('VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY', SOURCE)
        # The naked wrapper checks table, type word and the +0x60 slot,
        # clobbering only the thunk's own registers (x8/x9) before the
        # tail jump, and returns with x30 untouched otherwise.
        body = re.search(r'teardown_guard\(void\)\s*\{(.*?)\n\}', SOURCE, re.S)
        self.assertIsNotNone(body)
        asm = body.group(1)
        for needle in ('ldr  w9, [x8]', 'cbz  w9, 1f', 'ldr  x9, [x8, #0x60]',
                       'cbz  x9, 1f', 'br   x8', '1: ret'):
            self.assertIn(needle, asm)
        self.assertNotIn('x30', asm)
        self.assertNotIn(' bl ', asm)


if __name__ == '__main__':
    unittest.main()
