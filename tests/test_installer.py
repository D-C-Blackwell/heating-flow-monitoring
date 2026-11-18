import unittest

class InstallerTests(unittest.TestCase):
    def test_repeat_install_and_backup(self):
        from pathlib import Path
        import subprocess
        import sys
        import tempfile

        base = Path(".")
        with tempfile.TemporaryDirectory(prefix="ha-panel-install-") as directory:
            target = Path(directory)
            (target / "configuration.yaml").write_text("default_config:\n")
            command = [sys.executable, str(base / "tools/install_ha_panel.py"), "--config", str(target)]
            first = subprocess.run(command, check=True, capture_output=True, text=True)
            second = subprocess.run(command, check=True, capture_output=True, text=True)
            expected = [path for folder in ("custom_components", "www")
                        for path in (base / folder).rglob("*")
                        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc"]
            for original in expected:
                destination = target / original.relative_to(base)
                assert destination.read_text(encoding="utf-8") == original.read_text(encoding="utf-8-sig")
            assert list((target / ".ha-panel-backups").glob("ha-panel-*"))
            assert (target / "configuration.yaml").read_text() == "default_config:\n"
            print(f"Installer verified: {len(expected)} files, repeat install and backups.")
