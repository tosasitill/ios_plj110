#!/usr/bin/env python3
"""Anti-regression: metal2vulkan frontend heap aging (0105 patch).

The A image died at 168 s with backboardd's per-client budget full of
blocks nothing had placed in for minutes (kernel LIVE old30s 649 MiB):
the sub-allocator's leave-one rule kept each pool's last empty block
for the process's life. The cure lives in the metal2vulkan patch
series (0105-x9-heap-aging.patch): kept empty blocks idle for
AGED_EMPTY_FOR (30 s, the kernel old30s age) are returned even the
last one — swept bounded as a block is made in place(), drained
unbounded from the room keeper, and counted as FreeCause::KeptAged in
WorkCounts::memory_frees. The OOM purge itself (release_kept + one
retry in place()) predates the patch and is not its claim.

Source-asserted against the patch file: the runtime source itself is
a pinned third-party checkout under .local, not this tree, so the
patch is the maintained form of the change. The behaviour-level
coverage is the Rust unit test the patch adds
(a_kept_empty_block_aged_out_goes_even_the_last_one).
"""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'x9/build/third_party/metal2vulkan-963f/0105-x9-heap-aging.patch'
BUILD = ROOT / 'x9/build/scripts/build-general-metal.py'


class HeapAgingPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.patch = PATCH.read_text()
        cls.added = [line[1:] for line in cls.patch.splitlines()
                     if line.startswith('+') and not line.startswith('+++')]

    def joined(self):
        return '\n'.join(self.added)

    def test_registered_in_series_after_0104(self):
        """build-general-metal.py applies 0105 after 0104, so the stamp
        covers it and a stale runtime source is refused."""
        build = BUILD.read_text()
        self.assertIn("'0105-x9-heap-aging.patch'", build)
        self.assertLess(build.index("'0104-refresh-host-test-fixtures.patch'"),
                        build.index("'0105-x9-heap-aging.patch'"))

    def test_aged_horizon_is_the_kernel_old_age(self):
        """30 s, aligned with the kernel LIVE old30s age so both layers
        agree on "old"; the sweep bound on the allocation path is 4."""
        src = self.joined()
        self.assertIn('pub(super) const AGED_EMPTY_FOR: Duration = Duration::from_secs(30);', src)
        self.assertIn('const AGED_SWEEP_PER_PLACE: usize = 4;', src)

    def test_pool_aged_takes_even_the_last_kept_block(self):
        """Pool::aged exists beside Pool::stale and is not gated on
        another kept block staying (that gate is stale's leave-one
        rule, the thing being cured)."""
        src = self.joined()
        self.assertIn('pub(super) fn aged(&mut self, now: Instant) -> Option<T>', src)
        aged_body = src[src.index('pub(super) fn aged'):][:1200]
        self.assertIn('AGED_EMPTY_FOR', aged_body)
        self.assertNotIn('kept < 2', aged_body)
        self.assertNotIn('KEPT_EMPTY_BLOCKS', aged_body)

    def test_sweep_sites_bounded_on_place_unbounded_from_room(self):
        """place() sweeps (bounded) before it makes a block; the room
        keeper's age_spares drains unbounded; image blocks are freed,
        not re-spared, by the aged sweep."""
        src = self.joined()
        sweep = 'memory.age_aged(device, Instant::now(), AGED_SWEEP_PER_PLACE);'
        self.assertIn(sweep, src)
        lines = self.patch.splitlines()
        swept = next(i for i, line in enumerate(lines)
                     if line.startswith('+') and sweep in line)
        made = next(i for i, line in enumerate(lines)
                    if line.startswith(' ') and line.strip() == "// The type's kept empty blocks go, and the frees posted complete, before a block of it")
        self.assertLess(swept, made, 'the sweep is added ahead of the block make in place()')
        self.assertIn('memory.age_aged(device, now, usize::MAX);', src)
        self.assertIn('fn age_aged(&mut self, device: &ash::Device, now: Instant, limit: usize) -> (usize, u64)', src)

    def test_counted_as_kept_aged(self):
        """The sweep's frees are observable: a dedicated FreeCause,
        counted in WorkCounts::memory_frees as kept_aged."""
        src = self.joined()
        self.assertIn('KeptAged = 7,', src)
        self.assertIn('pub(super) const COUNT: usize = 8;', src)
        self.assertIn('pub kept_aged: u64,', src)
        self.assertIn('kept_aged: frees[FreeCause::KeptAged as usize],', src)
        self.assertIn('FreeCause::KeptAged, None', src)

    def test_rust_unit_test_present(self):
        """The patch carries the Pool-level behaviour test."""
        self.assertIn('fn a_kept_empty_block_aged_out_goes_even_the_last_one()', self.joined())
        self.assertIn('AGED_EMPTY_FOR, KEPT_EMPTY_BLOCKS, KEPT_EMPTY_FOR', self.joined())


if __name__ == '__main__':
    unittest.main()
