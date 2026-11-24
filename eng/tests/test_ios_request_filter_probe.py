from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from eng import run_ios_request_filter_probe as probe


class IosRequestFilterProbeTests(unittest.TestCase):
    def exercise(self, *, success: bool, fail_boot: bool = False) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "artifacts/native/ios-request-filter-probe").mkdir(parents=True)
            data = root / "app-data"
            (data / "Documents").mkdir(parents=True)
            result = data / "Documents/request-filter-result.txt"
            result.write_text("--gates:0\n")  # A stale success must never pass.
            commands = []

            def run(*command: str) -> str:
                commands.append(command)
                if command[:3] == ("xcrun", "simctl", "create"):
                    return "owned-device"
                if command[:3] == ("xcrun", "simctl", "boot") and fail_boot:
                    raise subprocess.CalledProcessError(1, command)
                if command[:3] == ("xcrun", "simctl", "get_app_container"):
                    return str(data)
                if command[:3] == ("xcrun", "simctl", "launch"):
                    self.assertFalse(result.exists())
                    if success:
                        result.write_text("--gates:0\n")
                return ""

            with patch.object(probe, "ROOT", root), patch.object(probe, "run", side_effect=run), \
                    patch.object(sys, "argv", ["probe", "--runtime", "test-runtime", "--mode", "gates"]):
                if fail_boot:
                    with self.assertRaises(subprocess.CalledProcessError):
                        probe.main()
                elif not success:
                    with self.assertRaisesRegex(RuntimeError, "did not report successful completion"):
                        probe.main()
                else:
                    probe.main()
            self.assertEqual(commands[-2:], [
                ("xcrun", "simctl", "shutdown", "owned-device"),
                ("xcrun", "simctl", "delete", "owned-device"),
            ])

    def test_success_requires_a_fresh_app_result_and_cleans_up(self) -> None:
        self.exercise(success=True)

    def test_process_start_without_result_is_not_success(self) -> None:
        self.exercise(success=False)

    def test_failed_boot_still_cleans_up_owned_device(self) -> None:
        self.exercise(success=False, fail_boot=True)


if __name__ == "__main__":
    unittest.main()
