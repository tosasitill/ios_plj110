#!/usr/bin/env python3
"""Pin the SMS host-ACK discrimination traces (2026-11-28 A-round follow-up).

The A round proved an MT 8601 reaches the bridge, is admitted to the host
queue, and is never ACKed: no WMS37 ever reaches x9m_request, and no
service-5 request is rejected either (boot-1 reject budget was 125/256).
The break sits between queue admission in bb_x9_sms_event and CommCenter's
sendAck, inside host consumption the bridge could not observe. These
tests pin the three bounded trace points that split the remaining
possibilities on the next device round; they assert structure only (log
text, bounds, no payload logging), not device behaviour.
"""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
COACHZ = ROOT / 'x9/baseband/qcom/baseband/coachz_baseband.cpp'
MESSAGES = ROOT / 'x9/kernel/src/iokit/Kernel/arm/X9Messages.inc'


def body(source, start_marker, end_marker):
    a = source.index(start_marker)
    b = source.index(end_marker, a)
    return source[a:b]


class SmsAckTrace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.coachz = COACHZ.read_text()
        cls.messages = MESSAGES.read_text()

    def test_enqueue_trace_coordinates_and_bound(self):
        fn = body(self.coachz, 'static bool bb_x9_sms_event(',
                  'static int bb_x9_sms_event_depth(')
        self.assertIn('MT indication queued slot %u cid %u channel %u depth %u '
                      'reads-waiting %u len %u', fn)
        self.assertIn('sms_event_traced < 4', fn)
        # Admission is reported only after the real enqueue.
        self.assertLess(fn.index('admitted = true;'),
                        fn.index('MT indication queued'))

    def test_enqueue_trace_logs_no_payload(self):
        fn = body(self.coachz, 'static bool bb_x9_sms_event(',
                  'static int bb_x9_sms_event_depth(')
        # The only printf in the publish path is the header-coordinate trace.
        self.assertEqual(fn.count('printf('), 1)
        self.assertNotIn('%02x', fn)

    def test_depth_accessor_scoped_to_wms_client(self):
        fn = body(self.coachz, 'static int bb_x9_sms_event_depth(', '#endif')
        self.assertIn('c.service == 5 && c.channel < 2', fn)
        self.assertIn('return -1', fn)

    def test_host_read_trace_in_bb_complete(self):
        fn = body(self.coachz, 'static void bb_complete(',
                  'static void bb_abort_reads(')
        self.assertIn('host read MT indication cid %u len %u', fn)
        self.assertIn('sms_read_traced < 4', fn)
        self.assertIn('frame.data[4] == 5', fn)
        self.assertIn('CBQ_QMI_INDICATION', fn)
        # The trace observes the read; it must not precede or replace it.
        self.assertLess(fn.index('host read MT indication'),
                        fn.index('bb_finish_read(&read, kIOReturnSuccess, '
                                 'frame.data, frame.len);'))

    def test_ack_wait_line_reports_queue_depth(self):
        at = self.messages.index('host ACK still absent after 30s')
        window = self.messages[at:at + 400]
        self.assertIn('host queue depth %d', window)
        self.assertIn('bb_x9_sms_event_depth(CBQ_PORT_BASE+in.slot)', window)


if __name__ == '__main__':
    unittest.main()
