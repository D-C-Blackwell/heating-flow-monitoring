import importlib.util
from pathlib import Path
import unittest

class PublicDefaultsTests(unittest.TestCase):
    def test_sources_are_not_installation_specific(self):
        spec = importlib.util.spec_from_file_location("public_model", "custom_components/ha_panel/model.py")
        model = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(model)
        self.assertEqual(len(model.default_rows()), 6)
        self.assertTrue(all(not row["entity"] and not row["subtract"] for row in model.default_rows()))

    def test_broker_is_entered_at_setup(self):
        source = Path("firmware/ha_panel/config.h").read_text()
        self.assertIn('DEFAULT_BROKER[] = ""', source)
