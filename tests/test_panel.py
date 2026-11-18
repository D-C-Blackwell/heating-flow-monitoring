import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location("panel_model", "custom_components/ha_panel/model.py")
model = importlib.util.module_from_spec(spec)
spec.loader.exec_module(model)

class PanelModelTests(unittest.TestCase):
    def state(self, value, unit="°C"):
        return SimpleNamespace(state=value, attributes={"unit_of_measurement": unit}, name="Temperature")

    def render(self, config, states):
        return model.render_row(config, states.get)

    def test_delta_and_thresholds(self):
        states = {"sensor.flow": self.state("65.4"), "sensor.return": self.state("48.2")}
        row = self.render({"entity": "sensor.flow", "subtract": "sensor.return",
                           "warning": 10, "critical": 20}, states)
        self.assertEqual(row["value"], "17.2")
        self.assertEqual(row["color"], model.COLORS["amber"])
        self.assertEqual(row["unit"], "°C")
        states["sensor.flow"] = self.state("70.4")
        self.assertEqual(self.render({"entity": "sensor.flow", "subtract": "sensor.return",
                                     "critical": 20}, states)["color"], model.COLORS["red"])

    def test_invalid_or_missing_delta_operand(self):
        for value in ("unavailable", "unknown", "nan", "inf", "invalid"):
            row = self.render({"entity": "sensor.flow", "subtract": "sensor.return"},
                              {"sensor.flow": self.state("40"), "sensor.return": self.state(value)})
            self.assertFalse(row["available"])
            self.assertEqual(row["value"], "--")

    def test_mismatched_units_are_explicit(self):
        row = self.render({"entity": "sensor.flow", "subtract": "sensor.return"},
                          {"sensor.flow": self.state("40"), "sensor.return": self.state("100", "°F")})
        self.assertFalse(row["available"])
        self.assertEqual(row["value"], "Unit mismatch")

    def test_negative_zero_and_wide_values(self):
        self.assertEqual(self.render({"entity": "sensor.flow"},
                         {"sensor.flow": self.state("-0.001")})["value"], "0.0")
        self.assertLessEqual(len(self.render({"entity": "sensor.flow"},
                             {"sensor.flow": self.state("12345678901234567890")})["value"]), 12)

    def test_text_boolean_and_absent_sources(self):
        self.assertEqual(self.render({"entity": "binary_sensor.door"},
                         {"binary_sensor.door": self.state("on", "")})["color"], model.COLORS["red"])
        self.assertFalse(self.render({"entity": "sensor.missing"}, {})["available"])

    def test_topic_identifiers(self):
        for panel_id in ("weather", "boiler-1", "panel_test"):
            self.assertTrue(model.valid_panel_id(panel_id))
        for panel_id in ("", "Upper", "panel/other", "bad+#", "x" * 25):
            self.assertFalse(model.valid_panel_id(panel_id))

if __name__ == "__main__":
    unittest.main()
