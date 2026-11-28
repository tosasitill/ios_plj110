#!/usr/bin/env python3
"""Anti-regression: render pipeline defaults (x9if, x9_gpu_acelite).

x9if is default-on since the 2026-11-28 user directive ("fix or
nearly-fix -> default on; =0 is the exit"). x9_gpu_acelite was flipped
default-on the same day and reverted to opt-in hours later: on-device
A/B/C boots convicted it (B acelite-only died at 133 s, C both-on at
36 s, A both-off survived; submission uncertain -> DeviceLost), so it
stays opt-in until the completion-path root-cause fix is verified.
Source-asserted in the x9_sms_ind style: the behaviour is decided by
sweet_x9_arg() fallbacks inside the kernel, which host tests cannot run.
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/SweetX9RenderEngine.inc'
RESIDENT = ROOT / 'x9/kernel/src/iokit/Kernel/arm/SweetX9CSFResident.inc'
SERVICE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/SweetX9RenderService.cpp'


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

    def test_acelite_default_off_source(self):
        """ACE-Lite opt-in only (x9_gpu_acelite=1): the 2026-11-28
        default-on was convicted on device (B/C boots DeviceLost,
        A both-off survived) and reverted the same day. Activation is
        still gated on the hardware checks, and the CIVAC skip still
        derives from (acelite active && x9_gpu_coherent), whose own
        default is 1 so x9_gpu_coherent=0 remains the
        maintenance-only exit."""
        src = RESIDENT.read_text()
        self.assertIn('sweet_x9_arg("x9_gpu_acelite",0)', src)
        self.assertNotIn('sweet_x9_arg("x9_gpu_acelite",1)', src)
        self.assertIn('if(acelite_arg==1&&!x9_gpu_acelite_active)', src)
        self.assertIn('sweet_x9_arg("x9_gpu_coherent",1)', src)
        self.assertIn('s->coherent=x9_gpu_acelite_active&&coherent_arg==1', src)

    def test_coh_probe_stays_opt_in(self):
        """Flipping acelite must not flip the H2 probe: x9_coh_probe
        remains opt-in and the skip accounting stays probe-gated."""
        src = ENGINE.read_text()
        self.assertIn('render_coh_probe_enabled()', src)
        self.assertIn('if(render_coh_probe_enabled())', src)


class RenderCoherentFixTests(unittest.TestCase):
    def test_pre_phase_never_skips_on_coherent(self):
        """2026-11-28 B/C acelite deaths: the publish (CPU->GPU) phase must
        always clean CPU-exposed client bytes; the coherent CIVAC skip is
        retained only in the reap's post phase (GPU->CPU, probe-validated)."""
        src = ENGINE.read_text()
        self.assertNotIn('if(coherent&&r.cpu_exposed)', src)
        self.assertIn('if(st->coherent&&r.cpu_exposed)', src)

    def test_fence_sync_set64_targets_own_slot(self):
        """Per-slot fences (rev252cn): the packet body's SYNC_SET64 target
        must be render_fence_offset(st->fence_slot) in both body branches;
        the hardcoded slot-0 address stranded paired job 2's completion."""
        src = ENGINE.read_text()
        self.assertEqual(
            src.count('render_va(g,i)+render_fence_offset(st->fence_slot)'), 2)
        self.assertNotIn('render_va(g,i)+0x8000', src)

    def test_pipeline_pairs_counted(self):
        """Pair formation is counted in the ASYNC stats line so an
        on-device boot can prove the two-jobs-in-flight path (and its
        per-slot fence) was actually exercised."""
        src = ENGINE.read_text()
        self.assertIn('render_pipeline_pairs++', src)
        self.assertIn('pairs=%llu', src)


class RenderBudgetTests(unittest.TestCase):
    def test_soft_budget_staged_refusal(self):
        """Soft backpressure (2026-11-28 A-image budget death): large
        native allocations are refused recoverably at 7/8 of the client
        budget, small ones keep the last 1/8; default on, =0 disables,
        imports exempt, hard cap unchanged below it."""
        src = SERVICE.read_text()
        self.assertIn('x9_gpu_soft_budget', src)
        self.assertIn('client_budget()*7/8', src)
        self.assertIn('X9 RENDER MEM: soft-budget', src)
        self.assertIn(
            'selector==X9RenderAllocate&&soft_budget_enabled()'
            '&&request.bytes>=(4u<<20)', src)



class RenderScopePreFixTests(unittest.TestCase):
    def test_publish_phase_never_skips_on_scope(self):
        """2026-11-28 round3: the frontend-scope (x9_cache_scope) skip in
        the publish walk was a second CPU->GPU maintenance skip the
        COH-PROBE never counted (it counts only the coherent class), and
        the round3 acceptance boot died with it at ~16.9 MB/job while the
        probe read 0. Publish must not skip on scope; the covered bytes
        fall through to the scope_hold accounting and full maintenance."""
        src = ENGINE.read_text()
        self.assertNotIn('scope_skip[0]+=r.bytes;continue', src)
        self.assertIn('st->scope_hold_bytes[0]+=r.bytes', src)
        self.assertIn('scope_skip[0] therefore stays 0', src)

    def test_post_phase_scope_skip_retained(self):
        """The reap (post) phase keeps its scope skip: a job's own
        acquires are still held at reap time, so it never fired in any
        recorded boot (post_skip_avg=0 in A/B/C/round3), and the
        predicate/snapshot machinery stays for it and the accounting."""
        src = ENGINE.read_text()
        self.assertIn('scope_skip[1]+=r.bytes;continue', src)
        self.assertEqual(src.count('render_scope_skips(st,r)'), 1)
        self.assertIn('x9_render_scope_snapshot', src)

    def test_frontend_scope_telemetry_kept(self):
        """The cache-frontend-scope line must keep printing with its
        pre_skip_avg sourced from scope_total[0]: after the fix the
        on-device acceptance reads pre_skip_avg=0 on that line (and a
        matching rise in cache-scope-hold pre_avg) instead of the
        counter being deleted and the signal lost."""
        src = ENGINE.read_text()
        self.assertIn('cache-frontend-scope', src)
        self.assertIn('scope_total[0]/count', src)
        self.assertIn('cache-scope-hold', src)


if __name__ == '__main__':
    unittest.main()
