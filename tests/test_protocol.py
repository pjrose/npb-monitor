import unittest

from npb_monitor import protocol


class ProtocolTests(unittest.TestCase):
    def test_can_ids_use_extended_mean_well_address_layout(self):
        self.assertEqual(protocol.controller_can_id(0x03), 0x000C0103)
        self.assertEqual(protocol.charger_can_id(0x03), 0x000C0003)

    def test_read_command_is_little_endian(self):
        self.assertEqual(protocol.command_bytes(0x0060), [0x60, 0x00])
        self.assertEqual(protocol.command_bytes(0x00C2), [0xC2, 0x00])

    def test_scaled_write_value_is_little_endian(self):
        self.assertEqual(protocol.scaled_command_bytes(0x0020, 30.0), [0x20, 0x00, 0xB8, 0x0B])
        self.assertEqual(protocol.scaled_command_bytes(0x0030, 5.0), [0x30, 0x00, 0xF4, 0x01])

    def test_operation_write(self):
        self.assertEqual(protocol.operation_bytes(True), [0x00, 0x00, 0x01])
        self.assertEqual(protocol.operation_bytes(False), [0x00, 0x00, 0x00])

    def test_wican_tx_payload_shape(self):
        payload = protocol.wican_tx_payload(0xC0103, [0x60, 0x00])
        self.assertEqual(payload["bus"], "0")
        self.assertEqual(payload["type"], "tx")
        self.assertEqual(payload["frame"][0]["id"], 0xC0103)
        self.assertEqual(payload["frame"][0]["dlc"], 2)
        self.assertTrue(payload["frame"][0]["extd"])
        self.assertFalse(payload["frame"][0]["rtr"])

    def test_decodes_voltage_current_and_temperature_readback(self):
        voltage = protocol.decode_frame(0xC0003, [0x60, 0x00, 0x60, 0x09], expected_address=0x03)
        current = protocol.decode_frame(0xC0003, [0x61, 0x00, 0xF4, 0x01], expected_address=0x03)
        temp = protocol.decode_frame(0xC0003, [0x62, 0x00, 0xFA, 0x00], expected_address=0x03)
        self.assertEqual(voltage["name"], "READ_VOUT")
        self.assertAlmostEqual(voltage["value"], 24.0)
        self.assertAlmostEqual(current["value"], 5.0)
        self.assertAlmostEqual(temp["value"], 25.0)

    def test_decodes_status_flags(self):
        fault = protocol.decode_frame(0xC0003, [0x40, 0x00, 0x64, 0x00], expected_address=0x03)
        self.assertTrue(fault["flags"]["output_over_voltage"])
        self.assertTrue(fault["flags"]["ac_abnormal"])
        self.assertTrue(fault["flags"]["output_off"])
        self.assertFalse(fault["flags"]["output_short_circuit"])

        charge = protocol.decode_frame(0xC0003, [0xB8, 0x00, 0x05, 0x08], expected_address=0x03)
        self.assertTrue(charge["flags"]["fully_charged"])
        self.assertTrue(charge["flags"]["constant_voltage_mode"])
        self.assertTrue(charge["flags"]["no_battery_detected"])

        system = protocol.decode_frame(0xC0003, [0xC1, 0x00, 0x42, 0x00], expected_address=0x03)
        self.assertTrue(system["flags"]["dc_output_normal_range"])
        self.assertTrue(system["flags"]["eeprom_access_error"])

    def test_config_flags(self):
        curve = protocol.decode_frame(0xC0003, [0xB4, 0x00, 0x84, 0x08], expected_address=0x03)
        self.assertTrue(curve["flags"]["charger_mode"])
        self.assertTrue(curve["flags"]["restart_after_full_enabled"])

        system = protocol.decode_frame(0xC0003, [0xC2, 0x00, 0x03, 0x04], expected_address=0x03)
        self.assertEqual(system["flags"]["operation_init"], 1)
        self.assertTrue(system["flags"]["disable_eeprom_writes"])


if __name__ == "__main__":
    unittest.main()

