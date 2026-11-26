#!/usr/bin/env python3
"""Focused host regression for SMS-DELIVER message-class decoding."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'
HEADER = ROOT / 'x9/kernel/src/bsd/dev/x9_sms_storage.h'


class SmsPduClassTests(unittest.TestCase):
    def test_real_header_decoder(self):
        out = ROOT / 'x9/out'
        out.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='sms-pdu-class-', dir=out) as tmp:
            source = Path(tmp) / 'test.c'
            binary = Path(tmp) / 'test_sms_pdu_class'
            source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include "x9_sms_storage.h"

int main(void) {
    uint8_t pdu[24] = {0};
    pdu[1] = 0x04; /* SMS-DELIVER */
    pdu[2] = 2;    /* two address semi-octets */
    pdu[3] = 0x81; /* international, digit semi-octets */
    pdu[4] = 0x21;
    for (unsigned cls = 0; cls <= 3; cls++) {
        pdu[5] = 0x10 | cls;
        assert(x9ms_pdu_class(pdu, 6) == cls);
    }
    pdu[5] = 0x00;
    assert(x9ms_pdu_class(pdu, 6) == 4);

    pdu[2] = 3;
    pdu[4] = 0x21;
    pdu[5] = 0xf3;
    pdu[6] = 0xf2;
    assert(x9ms_pdu_class(pdu, 7) == 2);

    pdu[2] = 10;
    pdu[3] = 0xd0; /* alphanumeric */
    pdu[8] = 0x00;
    pdu[9] = 0x11;
    assert(x9ms_pdu_class(pdu, 10) == 1);

    assert(x9ms_pdu_class(NULL, 6) == 6);
    assert(x9ms_pdu_class(pdu, 2) == 6);
    pdu[0] = 13;
    assert(x9ms_pdu_class(pdu, 24) == 6);
    pdu[0] = 0;
    pdu[2] = 21;
    assert(x9ms_pdu_class(pdu, 24) == 6);
    assert(x9ms_pdu_class(pdu, 5) == 6);
    return 0;
}
''')
            compiler = os.environ.get('CC', shutil.which('clang') or shutil.which('cc'))
            self.assertTrue(compiler, 'host C compiler required')
            subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(HEADER.parent), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_delivery_uses_shared_decoder(self):
        source = SOURCE.read_text()
        self.assertIn('unsigned cls=mti==2?4:x9ms_pdu_class(in.pdu,in.len);', source)
        self.assertNotIn('x9m_class(', source)
        self.assertNotIn('start+4+(addr+1)/2', SOURCE.read_text() + HEADER.read_text())


if __name__ == '__main__':
    unittest.main()
