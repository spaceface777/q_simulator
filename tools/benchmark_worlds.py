#!/usr/bin/env python3
"""Compare prebuilt worlds on the same scenarios in alternating order."""

import argparse
import csv
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile
import time
import tomllib


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenarios", type=Path, help="scenario TOML files or directories containing scenario.toml",
                        nargs="+")
    parser.add_argument("--world", nargs=2, action="append", required=True, metavar=("LABEL", "PATH"))
    parser.add_argument("--settings", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.repeat < 1 or len({label for label, _ in args.world}) != len(args.world):
        parser.error("use a positive repeat count and unique world labels")
    scenarios = []
    for path in args.scenarios:
        if path.is_dir():
            scenarios.extend(
                p for p in sorted(path.glob("*/scenario.toml"))
                if not p.parent.name.startswith("_") and not p.parent.name.endswith(".skip")
            )
        else:
            scenarios.append(path)
    if not scenarios:
        parser.error("no scenarios found")

    settings = ["--settings", str(args.settings.resolve())] if args.settings else []
    outcomes = {}
    totals = {label: [] for label, _ in args.world}
    with tempfile.TemporaryDirectory() as scratch, args.output.open("w", newline="") as output:
        prepared = []
        for index, scenario in enumerate(scenarios):
            scenario = scenario.resolve()
            contents = scenario.read_text(encoding="utf-8")
            section = tomllib.loads(contents)["scenario"]
            seed = []
            if "seed" in section:
                seed = ["--seed", str(section["seed"])]
                # Preserve relative property imports by pointing to the original spec.
                if "properties" in section:
                    spec = (scenario.parent / section["properties"]).resolve().as_posix()
                    contents = re.sub(r'(?m)^properties\s*=.*$', lambda _: "properties = " + json.dumps(spec, ensure_ascii=False), contents, count=1)
                contents = re.sub(r"(?m)^seed\s*=.*\n", "", contents, count=1)
                copy = Path(scratch) / f"{index}.toml"
                copy.write_text(contents, encoding="utf-8")
            else:
                copy = scenario
            prepared.append((scenario, copy, seed))
        writer = csv.writer(output)
        writer.writerow(("world", "repetition", "scenario", "elapsed_seconds", "exit"))
        for repeat in range(args.repeat):
            order = args.world if repeat % 2 == 0 else reversed(args.world)
            for label, world in order:
                total = 0.0
                for scenario, path, seed in prepared:
                    start = time.perf_counter()
                    result = subprocess.run(
                        [str(Path(world).resolve()), *settings, *seed, "--quiet", str(path)],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180,
                    )
                    elapsed = time.perf_counter() - start
                    verdicts = [line for line in result.stdout.splitlines()
                                if line.startswith(("FAIL ", "UNRESOLVED "))]
                    if result.returncode not in (0, 1) or (result.returncode == 1 and not verdicts):
                        sys.exit(f"{label}: {scenario} did not complete\n{result.stdout}")
                    actual = result.returncode, verdicts
                    if actual != outcomes.setdefault(scenario, actual):
                        sys.exit(f"{label}: {scenario} changed verdicts\n{result.stdout}")
                    writer.writerow((label, repeat + 1, scenario, f"{elapsed:.6f}", result.returncode))
                    total += elapsed
                totals[label].append(total)
                print(f"{label} run {repeat + 1}: {total:.3f} s", flush=True)
    for label, times in totals.items():
        print(f"{label} median: {statistics.median(times):.3f} s")


if __name__ == "__main__":
    main()
