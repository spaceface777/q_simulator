"""Build each scenario world once and run each scenario twice."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("qsim", type=Path)
    qsim = parser.parse_args().qsim.resolve()
    root = Path(__file__).resolve().parents[2]
    # The P2 process runner reports these same verdicts with the pinned applet.
    expected_unresolved = {
        "hardware_test": "reaches_async_monitoring_and_starts_every_periodic_rpc_stream",
        "hardware_test_tag_at_startup": "startup_tag_is_classified_from_a_successful_rfid_read",
    }
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        worlds = {}
        for scenario in sorted((root / "scenarios").glob("*.toml")):
            with scenario.open("rb") as stream:
                metadata = tomllib.load(stream).get("scenario", {})
            if "layout" not in metadata:
                continue
            layout = (scenario.parent / metadata["layout"]).resolve()
            if layout not in worlds:
                world = work / f"{layout.stem}.exe"
                subprocess.run([sys.executable, qsim, "build", layout, "-o", world], check=True)
                worlds[layout] = world
            world = worlds[layout]
            launcher = [world] if sys.platform == "win32" else ["sh", world]
            traces = [work / "first.ndjson", work / "second.ndjson"]
            for trace in traces:
                result = subprocess.run([
                    *launcher, scenario.relative_to(root), "--settings", "settings_example.toml",
                    "--trace", trace,
                ], cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                verdicts = [line for line in result.stdout.splitlines() if line.startswith(("FAIL ", "UNRESOLVED "))]
                expected = expected_unresolved.get(scenario.stem)
                if expected is None:
                    ok = result.returncode == 0 and not verdicts
                else:
                    ok = result.returncode == 1 and len(verdicts) == 1 and verdicts[0].startswith(
                        f"UNRESOLVED {expected} at ")
                if not ok:
                    raise RuntimeError(f"{scenario.name} returned {result.returncode}\n{result.stdout}")
            if traces[0].read_bytes() != traces[1].read_bytes():
                raise RuntimeError(f"{scenario.name} produced different traces")
            print(f"{scenario.name}: {'one undecided property' if expected else 'passed'}", flush=True)


if __name__ == "__main__":
    main()
