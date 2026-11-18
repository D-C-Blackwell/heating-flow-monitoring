from datetime import datetime, timezone
from pathlib import Path
from types import ModuleType, SimpleNamespace
import importlib
import json
import sys
import unittest

package = ModuleType("panel_test_package")
package.__path__ = [str(Path("custom_components/ha_panel").resolve())]
sys.modules["panel_test_package"] = package
weather_model = importlib.import_module("panel_test_package.weather_model")
panel_model = importlib.import_module("panel_test_package.model")
now = datetime(2026, 11, 18, 12, tzinfo=timezone.utc).timestamp()

def state(condition="cloudy"):
    return SimpleNamespace(state=condition, name="Forecast Home",
                           attributes={"temperature": 10.4, "temperature_unit": "°C"})

def forecast(hours, condition="cloudy", probability=None, precipitation=None):
    entry = {"datetime": datetime.fromtimestamp(now + hours * 3600, timezone.utc).isoformat(),
             "condition": condition, "temperature": 11.2}
    if probability is not None:
        entry["precipitation_probability"] = probability
    if precipitation is not None:
        entry["precipitation"] = precipitation
    return entry

class WeatherTests(unittest.TestCase):
    def build(self, entries=(), condition="cloudy", fresh=True, kind="hourly", sensor=None):
        return weather_model.build_weather(state(condition), sensor, entries, kind, now, fresh)

    def test_live_provider_and_local_sensor(self):
        sensor = SimpleNamespace(state="24.08", name="Outdoor Temperature",
                                 attributes={"unit_of_measurement": "°C"})
        result = self.build(sensor=sensor)
        self.assertEqual(result["temperature"], "10.4")
        self.assertEqual(result["sensor_temperature"], "24.1")
        self.assertEqual(result["unit"], "°C")

    def test_future_rain_and_probability(self):
        entries = [forecast(i, probability=0) for i in range(12)]
        entries[3] = forecast(3, "rainy", probability=80)
        result = self.build(entries)
        self.assertEqual(result["rain_summary"], "Rain likely - next 12h")
        self.assertEqual(result["next_rain"], now + 3 * 3600)
        self.assertEqual(result["rain_probability"], 80)
        self.assertEqual(len(result["forecast"]), 3)

    def test_rain_now_without_forecast(self):
        self.assertEqual(self.build(condition="pouring")["rain_summary"], "Rain now")
        self.assertIsNone(self.build(condition="pouring")["rain_probability"])

    def test_missing_and_old_forecasts_are_not_dry_predictions(self):
        for entries, fresh in (([], True), ([forecast(2)], False), ([forecast(-48)], True)):
            self.assertEqual(self.build(entries, fresh=fresh)["rain_summary"], "Forecast unavailable")
        incomplete = {"datetime": forecast(1)["datetime"], "temperature": 10}
        self.assertEqual(self.build([incomplete])["rain_summary"], "Forecast unavailable")

    def test_precipitation_without_probability(self):
        result = self.build([forecast(2, "rainy", precipitation=0.4)])
        self.assertIsNone(result["rain_probability"])
        self.assertEqual(result["rain_summary"], "Rain possible - next hours")
        self.assertEqual(result["next_rain"], now + 2 * 3600)

    def test_daily_fallback_and_short_hourly_coverage(self):
        daily = self.build([forecast(8, "rainy", probability=60)], kind="daily")
        self.assertEqual(daily["rain_summary"], "Rain likely - next day")
        yesterday = self.build([forecast(-20, "rainy", probability=90)], kind="daily")
        self.assertEqual(yesterday["forecast"], [])
        short = self.build([forecast(1, probability=0)])
        self.assertEqual(short["rain_summary"], "No rain forecast - next hours")

    def test_unavailable_sources_and_invalid_timestamps(self):
        result = weather_model.build_weather(None, None,
                    [{"datetime": "bad"}, {"datetime": "2026-11-18T13:00:00"}],
                    "hourly", now, True)
        self.assertFalse(result["available"])
        self.assertEqual(result["temperature"], "--")
        self.assertEqual(result["forecast"], [])

    def test_snow_is_not_reported_as_rain(self):
        result = self.build([forecast(1, "snowy", probability=90, precipitation=2)])
        self.assertTrue(result["rain_summary"].startswith("Snow forecast"))
        self.assertEqual(result["next_rain"], 0)

    def test_two_page_payload_fits_mqtt_buffer(self):
        weather = self.build([forecast(i, "rainy", probability=90) for i in range(12)])
        rows = [{"label": "L" * 28, "value": "1" * 18, "unit": "°C", "color": "#65d7ff"}
                for _ in range(6)]
        payload = {"v": 1, "title": "Heating", "slots": 6, "rows": rows, "weather": weather,
                   "updated": int(now), "timezone": "GMT0BST,M3.5.0/1,M10.5.0", "page_seconds": 20}
        self.assertLess(len(json.dumps(panel_model.display_payload(payload)).encode()), 3996)

class DisplayEncodingTests(unittest.TestCase):
    def test_degree_units_survive_wire_normalization(self):
        self.assertEqual(panel_model.display_payload({"unit": "\u00b0C", "other": "\u00b0F"}),
                         {"unit": "\u00b0C", "other": "\u00b0F"})
        self.assertEqual(panel_model.display_payload("\u2603"), "?")

if __name__ == "__main__":
    unittest.main()
