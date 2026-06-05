import unittest

from npb_monitor.profiles import PROFILES, profile_payload


class ChargeProfileTests(unittest.TestCase):
    def test_starter_profiles_are_available(self):
        ids = {profile["id"] for profile in profile_payload()}

        self.assertIn("lifepo4_24v", ids)
        self.assertIn("lead_acid_24v", ids)

    def test_profile_values_fit_npb_24v_ranges(self):
        for profile in PROFILES.values():
            self.assertGreaterEqual(profile.cv_voltage_v, 21.0)
            self.assertLessEqual(profile.cv_voltage_v, 42.0)
            self.assertGreaterEqual(profile.float_voltage_v, 21.0)
            self.assertLessEqual(profile.float_voltage_v, 42.0)
            self.assertGreaterEqual(profile.cc_current_a, 0.0)
            self.assertLessEqual(profile.cc_current_a, 13.5)


if __name__ == "__main__":
    unittest.main()
