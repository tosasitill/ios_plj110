#!/usr/bin/env python3
"""Guard the scope of call registration and MT observation log labels."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Calls.inc'


class CallObservationLabels(unittest.TestCase):
    def test_no_obsolete_service_or_ui_claim_in_runtime_labels(self):
        source = SOURCE.read_text()
        self.assertNotIn('VOICE09 still unannounced', source)
        self.assertNotIn('VOICE09 unannounced, no iOS call UI', source)
        self.assertIn('boot registration start (Android RIL init subset; not voice readiness)', source)
        self.assertIn('MD observation only; not iOS call UI acceptance', source)

    def test_observation_remains_scoped_to_owned_reducer(self):
        source = SOURCE.read_text()
        start = source.index('static void x9c_mt_observe(')
        end = source.index('/* CallTechnology provenance', start)
        observer = source[start:end]
        self.assertIn('if(!in.pending&&!in.seen)return;', observer)
        self.assertIn('in.sim+1,in.native_id,s', observer)
        self.assertNotIn('x9c_dial_submit(', observer)


if __name__ == '__main__':
    unittest.main()
