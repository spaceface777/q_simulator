#!/usr/bin/env python3
"""Run one applet scenario in a world, and match the world's output against the expected lines.

A scenario lives in test/applets/<applet>/scenarios/<scenario>/:

  scenario.toml  The scenario. It may name a property file next to it.
  expected.txt   The expected output. Every line is a Python regular expression that must match a
                 whole output line, and the matches must come in the file's order. Output lines
                 that no expression matches are ignored. Blank lines and lines that start with '#'
                 are skipped.
  layout.toml    Optional. Without it, the world has one module that runs the applet. A firmware
                 value without a '/' names an applet, and resolves to its object in FIRMWARE_DIR.
  args           Optional. Extra world arguments after the scenario, split like a shell would.

The output is the world's stdout and stderr in one stream, followed by "exit status: N". The world
runs in the scenario directory, so that the paths it reports stay short and stable.
"""

import argparse
import contextlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

DEFAULT_LAYOUT = """firmware = "{firmware}"

[module.1]
active = true
neighbors = [0, 0, 0]
pos = [0, 0]
angle = 0
"""

FIRMWARE_NAME = re.compile(r'^(\s*firmware\s*=\s*)"([^"/]+)"(\s*(#.*)?)$', re.MULTILINE)


@contextlib.contextmanager
def scratch_directory():
    """A temporary directory, removed afterwards. Windows keeps a program's file locked for a moment
    after the program exits, up to seconds under Arm's x64 emulation, so removal retries there."""
    directory = tempfile.mkdtemp()
    try:
        yield directory
    finally:
        for attempt in range(50):
            try:
                shutil.rmtree(directory)
                break
            except PermissionError:
                if os.name != "nt" or attempt == 49:
                    raise
                time.sleep(0.2)


def world_command(world: Path) -> list[str]:
    """Cosmopolitan worlds are APE files, which start through a shell outside Windows."""
    with world.open("rb") as stream:
        ape = stream.read(6) == b"MZqFpD"
    return ["/bin/sh", str(world)] if ape and os.name != "nt" else [str(world)]


def layout_text(scenario: Path, applet: str, firmware_dir: Path) -> str:
    layout = scenario / "layout.toml"
    if not layout.exists():
        return DEFAULT_LAYOUT.format(firmware=(firmware_dir / f"{applet}.o").as_posix())

    def resolve(match: re.Match) -> str:
        if match.group(2).endswith(".o"):
            return match.group(0)
        return f'{match.group(1)}"{(firmware_dir / f"{match.group(2)}.o").as_posix()}"{match.group(3)}'

    return FIRMWARE_NAME.sub(resolve, layout.read_text())


def expected_lines(scenario: Path) -> list[tuple[int, re.Pattern]]:
    patterns = []
    for number, line in enumerate((scenario / "expected.txt").read_text().splitlines(), 1):
        if line.strip() and not line.startswith("#"):
            try:
                patterns.append((number, re.compile(line)))
            except re.error as error:
                raise SystemExit(f"expected.txt:{number}: invalid regular expression: {error}")
    return patterns


def match(patterns: list[tuple[int, re.Pattern]], output: list[str]) -> tuple[int, re.Pattern] | None:
    """Return the first expected line that has no match after the previous match, if any."""
    position = 0
    for number, pattern in patterns:
        while position < len(output) and not pattern.fullmatch(output[position]):
            position += 1
        if position == len(output):
            return number, pattern
        position += 1
    return None


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("qsim", type=Path, help="the qsim world builder of a simulator build")
    parser.add_argument("firmware_dir", type=Path, help="the directory with the built applet objects")
    parser.add_argument("scenario", type=Path, help="the scenario directory")
    parser.add_argument("-v", "--verbose", action="store_true", help="print the output even when it matches")
    args = parser.parse_args()
    scenario = args.scenario.resolve()
    applet = scenario.parent.parent.name
    patterns = expected_lines(scenario)
    extra = shlex.split((scenario / "args").read_text()) if (scenario / "args").exists() else []

    with scratch_directory() as directory:
        work = Path(directory)
        layout = work / "layout.toml"
        layout.write_text(layout_text(scenario, applet, args.firmware_dir.resolve()))
        world = work / "world.exe"
        subprocess.run([sys.executable, args.qsim, "build", layout, "-o", world], check=True)
        # A GUI run extracts SDL3 into the cache, so keep it out of the user's.
        env = {**os.environ, "XDG_CACHE_HOME": str(work / "cache"), "LOCALAPPDATA": str(work / "cache")}
        result = subprocess.run([*world_command(world), "scenario.toml", *extra], cwd=scenario, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                                timeout=300)

    output = [*result.stdout.splitlines(), f"exit status: {result.returncode}"]
    missing = match(patterns, output)
    if args.verbose:
        print("\n".join(output))
    if missing is None:
        print(f"{scenario.parent.parent.name}/{scenario.name}: {len(patterns)} expected lines matched")
        return
    number, pattern = missing
    if not args.verbose:
        print("\n".join(output))
    print(f"\n{scenario / 'expected.txt'}:{number}: no output line after the previous match matches\n"
          f"  {pattern.pattern}", file=sys.stderr)
    raise SystemExit(1)


if __name__ == "__main__":
    main()
