import unittest

from npb_monitor import protocol
from npb_monitor.state import MonitorState


class StateHistoryTests(unittest.TestCase):
    def test_voltage_current_readback_creates_history_sample(self):
        state = MonitorState()
        state.record_decoded(protocol.decode_frame(0xC0003, [0x60, 0x00, 0x60, 0x09], expected_address=0x03))
        state.record_decoded(protocol.decode_frame(0xC0003, [0x61, 0x00, 0xF4, 0x01], expected_address=0x03))

        history = state.history_snapshot()
        self.assertEqual(len(history["samples"]), 1)
        self.assertAlmostEqual(history["samples"][0]["voltage"], 24.0)
        self.assertAlmostEqual(history["samples"][0]["current"], 5.0)
        self.assertAlmostEqual(history["samples"][0]["power"], 120.0)

    def test_status_bit_changes_create_annotation_events(self):
        state = MonitorState()
        state.record_decoded(protocol.decode_frame(0xC0003, [0xB8, 0x00, 0x00, 0x00], expected_address=0x03))
        state.record_decoded(protocol.decode_frame(0xC0003, [0xB8, 0x00, 0x02, 0x00], expected_address=0x03))
        state.record_decoded(protocol.decode_frame(0xC0003, [0xB8, 0x00, 0x04, 0x00], expected_address=0x03))

        events = state.history_snapshot()["events"]
        messages = [event["message"] for event in events]
        self.assertIn("Constant current active", messages)
        self.assertIn("Constant current cleared", messages)
        self.assertIn("Constant voltage active", messages)
        self.assertTrue(all(event["annotate"] for event in events))


if __name__ == "__main__":
    unittest.main()

