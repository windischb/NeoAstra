#!/usr/bin/env python3
"""Build and run the Apple request-filter probes in a disposable iOS simulator."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[1]
BUNDLE_ID = "org.neoastra.tests.request-filter"


def run(*command: str, timeout: int = 300) -> str:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=timeout)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    return result.stdout.strip()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", required=True, help="Installed simctl runtime identifier")
    parser.add_argument("--device-type", default="com.apple.CoreSimulator.SimDeviceType.iPhone-17")
    parser.add_argument("--mode", choices=("baseline", "gates", "api", "all"), default="all")
    args = parser.parse_args()
    build = ROOT / "artifacts/native/ios-request-filter-probe"
    run("cmake", "--preset", "iossimulator-arm64-release", "-B", str(build),
        "-DNEOASTRA_BUILD_IOS_REQUEST_FILTER_PROBE=ON")
    run("cmake", "--build", str(build), "--target", "neoastra_ios_request_filter_probe")
    app = build / "neoastra_ios_request_filter_probe.app"
    run("codesign", "--force", "--sign", "-", str(app))
    device = run("xcrun", "simctl", "create", f"NeoAstra Request Filter {uuid.uuid4().hex[:8]}",
                 args.device_type, args.runtime)
    try:
        run("xcrun", "simctl", "boot", device)
        run("xcrun", "simctl", "bootstatus", device, "-b")
        run("xcrun", "simctl", "install", device, str(app))
        data = Path(run("xcrun", "simctl", "get_app_container", device, BUNDLE_ID, "data"))
        result = data / "Documents/request-filter-result.txt"
        modes = ("baseline", "gates", "api") if args.mode == "all" else (args.mode,)
        for mode in modes:
            result.unlink(missing_ok=True)
            output = run("xcrun", "simctl", "launch", "--console", device, BUNDLE_ID, f"--{mode}")
            (build / f"{mode}.log").write_text(output + "\n", encoding="utf-8")
            if not result.exists() or result.read_text().strip() != f"--{mode}:0":
                raise RuntimeError(f"{mode}: probe did not report successful completion")
            print(f"PASS iOS request-filter {mode}", flush=True)
    finally:
        # Only the device created above is ours to shut down and delete.
        try:
            run("xcrun", "simctl", "shutdown", device)
        finally:
            run("xcrun", "simctl", "delete", device)


if __name__ == "__main__":
    main()
