#!/usr/bin/env python3
"""Offline regression for X9 software display-cadence validation."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ARM = HERE.parent / 'kernel/src/iokit/Kernel/arm'
OUT = HERE.parent / 'out'


class CadenceTests(unittest.TestCase):
    def test_real_cpp_helpers(self):
        compiler = os.environ.get('CXX', shutil.which('c++'))
        self.assertTrue(compiler, 'host C++ compiler required')
        OUT.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='cadence-', dir=OUT) as tmp:
            source = Path(tmp) / 'test.cpp'
            binary = Path(tmp) / 'test_cadence'
            source.write_text(r'''
#include <cassert>
#include <cstdint>
#include "X9DisplayCadence.h"

int main(void) {
    assert(x9_display_cadence_supported(60));
    assert(x9_display_cadence_supported(120));
    const uint32_t invalid[] = {0, 1, 59, 61, 90, 119, 121, UINT32_MAX};
    for (uint32_t hz : invalid) {
        assert(!x9_display_cadence_supported(hz));
        assert(x9_display_cadence_select(hz, true, 8333) == 0);
    }
    assert(x9_display_cadence_select(60, true, 8333) == 60);
    assert(x9_display_cadence_select(120, true, 16667) == 120);

    assert(x9_display_cadence_select(60, false, 0) == 60);
    assert(x9_display_cadence_select(60, false, 9099) == 120);
    assert(x9_display_cadence_select(60, false, 9100) == 60);
    assert(x9_display_cadence_select(60, false, 11111) == 60);
    assert(x9_display_cadence_select(60, false, 16667) == 60);

    assert(x9_display_cadence_period_ns(60) == 16666667ULL);
    assert(x9_display_cadence_period_ns(120) == 8333333ULL);
    return 0;
}
''')
            flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] \
                if os.environ.get('X9_TEST_SANITIZE') == '1' else []
            subprocess.run([compiler, '-std=c++11', '-O2', '-Wall', '-Wextra', '-Werror',
                            *shlex.split(os.environ.get('CXXFLAGS', '')), *flags,
                            '-I', str(ARM), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_kernel_uses_validated_cadence(self):
        source = (ARM / 'SweetFramebuffer.cpp').read_text()
        self.assertIn('#include "X9DisplayCadence.h"', source)
        self.assertIn('!x9_display_cadence_supported((uint32_t)want)', source)
        self.assertIn('fTickHz = x9_display_cadence_select(hz, hz_explicit, after)', source)
        self.assertNotIn('fTickHz = hz == 120 ? 120 : 60', source)


if __name__ == '__main__':
    unittest.main()
