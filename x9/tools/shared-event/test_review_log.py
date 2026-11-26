#!/usr/bin/env python3
"""Synthetic parser fixtures only; these are not device evidence."""
import runpy
import unittest
from pathlib import Path

review = runpy.run_path(str(Path(__file__).with_name('review_log.py')))['review']


class ReviewTest(unittest.TestCase):
    def test_correlate_only_same_pid_and_pre_abort(self):
        rows = [
            '[1.0] boot-args = x9ed=1 x9_surf_events=1',
            '[2.0] SWEET SURF EVENT: AegirPoster pid=9 seq=1 sel=42 async=0',
            '[2.1] SWEET SURF EVENT RESULT: AegirPoster pid=9 seq=1 sel=42 kr=0xe00002c7',
            '[2.2] SWEET SURF EVENT: AegirPoster pid=10 seq=2 sel=40 async=1',
            '[2.3] SWEET SURF EVENT: AegirPoster pid=9 seq=3 sel=36 async=0',
            '[2.4] SWEET FB: display off',
            '[2.5] CRASH AegirPoster[9] sig 6',
            '[2.6] SWEET SURF EVENT RESULT: AegirPoster pid=9 seq=3 sel=36 kr=0x0',
        ]
        summary = review(rows)
        calls = summary['same_pid_calls_before_first_abort']
        self.assertEqual([c['seq'] for c in calls], [1, 3])
        self.assertEqual(calls[0]['return_code'], '0xe00002c7')
        self.assertNotIn('return_code', calls[1])
        self.assertEqual(summary['aborts_at_or_after_first_display_off'], 1)
        self.assertFalse(review(rows + [rows[0]])['same_pid_calls_before_first_abort'])
        self.assertFalse(review(rows + [rows[2]])['same_pid_calls_before_first_abort'])

    def test_old_or_empty_trace_not_acceptance(self):
        summary = review(['[2.0] CRASH AegirPoster[9] sig 6'])
        self.assertEqual(summary['abort_count_observed'], 1)
        self.assertIsNone(summary['aborts_at_or_after_first_display_off'])
        self.assertEqual(summary['results_matched'], 0)
        self.assertIn('not evaluated', review([])['acceptance'])


if __name__ == '__main__':
    unittest.main()
