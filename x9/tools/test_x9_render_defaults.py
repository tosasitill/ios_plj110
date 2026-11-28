#!/usr/bin/env python3
"""Anti-regression: render pipeline defaults (x9if, x9_gpu_acelite).

Both switches are default-on since the 2026-11-28 user directive
("fix or nearly-fix -> default on; =0 is the exit"); each keeps its own
single-token opt-out so either can be disabled alone on a test boot.
Source-asserted in the x9_sms_ind style: the behaviour is decided by
sweet_x9_arg() fallbacks inside the kernel, which host tests cannot run.
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc'
RESIDENT = ROOT / 'x9/kernel/src/iokit/Kernel/arm/SweetX9CSFResident.inc'


class RenderDefaultsTests(unittest.TestCase):
    def test_x9if_default_on_source(self):
        """Two jobs in flight unless x9if=0; prerequisites stay default-on
        (the pipeline publish path needs the parked resume + async submit)."""
        src = ENGINE.read_text()
        self.assertIn('sweet_x9_arg("x9if",1)==1?1:0', src)
        self.assertNotIn('sweet_x9_arg("x9if",0)', src)
        self.assertIn('sweet_x9_arg("x9_gpu_park",1)==1?1:0', src)
        self.assertIn('sweet_x9_arg("x9_submit_async",1)!=1', src)
        self.assertIn('render_pipeline_enabled()&&render_park_enabled()', src)

    def test_acelite_default_on_source(self):
        """ACE-Lite requested unless x9_gpu_acelite=0; activation still
        gated on the hardware checks, and the CIVAC skip still derives
        from (acelite active && x9_gpu_coherent), whose own default is 1
        so x9_gpu_coherent=0 remains the maintenance-only exit."""
        src = RESIDENT.read_text()
        self.assertIn('sweet_x9_arg("x9_gpu_acelite",1)', src)
        self.assertNotIn('sweet_x9_arg("x9_gpu_acelite",0)', src)
        self.assertIn('if(acelite_arg==1&&!x9_gpu_acelite_active)', src)
        self.assertIn('sweet_x9_arg("x9_gpu_coherent",1)', src)
        self.assertIn('s->coherent=x9_gpu_acelite_active&&coherent_arg==1', src)

    def test_coh_probe_stays_opt_in(self):
        """Flipping acelite must not flip the H2 probe: x9_coh_probe
        remains opt-in and the skip accounting stays probe-gated."""
        src = ENGINE.read_text()
        self.assertIn('render_coh_probe_enabled()', src)
        self.assertIn('if(render_coh_probe_enabled())', src)


if __name__ == '__main__':
    unittest.main()
