#!/usr/bin/env python3
"""Host test for x9_render_coh_pick_atoms (H2 step b atom selection).

Extracts the production helper verbatim from X9RenderMemory.h, compiles
it with the system clang, and checks the properties the probe's verdict
depends on: bounded picks, distinctness, and full, uniform coverage of
the written range over one rotation cycle (so a "stale=0" result is not
an artifact of always sampling the same atoms).
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HEADER = Path(__file__).resolve().parents[1] / 'kernel/src/iokit/Kernel/arm/X9RenderMemory.h'

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
__PICKER__
static int failures = 0;
#define CHECK(cond) do { if(!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while(0)

static void check_range(uint64_t atoms, unsigned max_atoms) {
    uint64_t out[64];
    /* degenerate + identity cases */
    CHECK(x9_render_coh_pick_atoms(0, 0, max_atoms, out) == 0);
    CHECK(x9_render_coh_pick_atoms(atoms, 0, 0, out) == 0);
    if (atoms <= max_atoms) {
        unsigned n = x9_render_coh_pick_atoms(atoms, 7, max_atoms, out);
        CHECK(n == atoms);
        for (uint64_t i = 0; i < atoms; i++) CHECK(out[i] == i);
        return;
    }
    unsigned n = x9_render_coh_pick_atoms(atoms, 0, max_atoms, out);
    CHECK(n == max_atoms);
    /* full rotation cycle: every atom picked exactly max_atoms times */
    uint64_t *hits = calloc(atoms, sizeof(uint64_t));
    for (uint64_t rot = 0; rot < atoms; rot++) {
        uint64_t picks[64];
        unsigned m = x9_render_coh_pick_atoms(atoms, rot, max_atoms, picks);
        CHECK(m == max_atoms);
        for (unsigned i = 0; i < m; i++) {
            CHECK(picks[i] < atoms);
            for (unsigned j = i + 1; j < m; j++) CHECK(picks[i] != picks[j]);
            hits[picks[i]]++;
        }
        /* periodicity: rotation and rotation+atoms agree */
        uint64_t again[64];
        unsigned m2 = x9_render_coh_pick_atoms(atoms, rot + atoms, max_atoms, again);
        CHECK(m2 == m && memcmp(picks, again, m * sizeof(uint64_t)) == 0);
    }
    for (uint64_t a = 0; a < atoms; a++) CHECK(hits[a] == max_atoms);
    free(hits);
}

int main(void) {
    check_range(1, 4);
    check_range(4, 4);
    check_range(5, 4);
    check_range(16, 4);
    check_range(17, 4);
    check_range(100, 4);
    check_range(1000, 4);
    check_range(7, 3);
    check_range(255, 8);
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("all picker checks passed\n");
    return 0;
}
'''


def extract_picker() -> str:
    text = HEADER.read_text()
    start = text.index('static inline unsigned x9_render_coh_pick_atoms')
    end = text.index('\n}\n', start) + 3
    return text[start:end]


class CohPickTest(unittest.TestCase):
    def test_picker_properties(self):
        source = HARNESS.replace('__PICKER__', extract_picker())
        with tempfile.TemporaryDirectory() as td:
            c_path = Path(td) / 'harness.c'
            exe = Path(td) / 'harness'
            c_path.write_text(source)
            subprocess.run(['/usr/bin/clang', '-std=c11', '-Wall', '-Wextra',
                            '-Werror', '-O1', str(c_path), '-o', str(exe)],
                           check=True)
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('all picker checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
