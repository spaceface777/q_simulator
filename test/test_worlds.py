#!/usr/bin/env python3
"""World checks that need several runs, or that are about the world executable rather than an applet.

Each check builds its worlds from applet scenario layouts, like run_scenario.py does:

  repeatability  Repeated runs write identical traces. Settings files override only what they set.
  command_line   The world rejects invalid options and files with a configuration error.
  gui            The GUI runs, draws a moving scenario, and stops on Ctrl+C. Exits with 77 (skipped)
                 when the world cannot load SDL3.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time
import zipfile

from run_scenario import layout_text, scratch_directory

APPLETS = Path(__file__).resolve().parent / "applets"
SKIPPED = 77


class Worlds:
    """Builds one world per scenario layout, and runs it with a private SDL3 cache."""

    def __init__(self, qsim: Path, firmware_dir: Path, work: Path) -> None:
        self.qsim, self.firmware_dir, self.work = qsim, firmware_dir, work
        self.env = {**os.environ, "XDG_CACHE_HOME": str(work / "cache"), "LOCALAPPDATA": str(work / "cache")}

    def build(self, applet: str, scenario: str) -> Path:
        directory = APPLETS / applet / "scenarios" / scenario
        layout = self.work / f"{applet}-{scenario}.toml"
        layout.write_text(layout_text(directory, applet, self.firmware_dir))
        world = self.work / f"{applet}-{scenario}.exe"
        subprocess.run([sys.executable, self.qsim, "build", layout, "-o", world], check=True)
        return world

    def run(self, world: Path, *args: object) -> subprocess.CompletedProcess:
        return subprocess.run([world, *map(str, args)], cwd=self.work, env=self.env,
                              capture_output=True, text=True, errors="replace", timeout=300)

    def expect(self, world: Path, args: list[object], code: int, *messages: str) -> subprocess.CompletedProcess:
        result = self.run(world, *args)
        missing = [message for message in messages if message not in result.stderr]
        if result.returncode != code or missing:
            raise RuntimeError(f"{' '.join(map(str, args))} returned {result.returncode}, expected {code}; "
                               f"missing {missing}\n{result.stdout}{result.stderr}")
        return result


def identity(world: Path) -> tuple[int, bytes]:
    with world.open("rb") as stream:
        return world.stat().st_mtime_ns, hashlib.file_digest(stream, "sha256").digest()


def check_repeatability(worlds: Worlds) -> None:
    work = worlds.work
    (work / "partial.toml").write_text("seed = 1\n")
    # A settings file in the working directory must not affect runs that do not name it.
    (work / "settings.toml").write_text("seed = 123\n")
    for applet, scenario in [("conveyor", "route"), ("idle", "tub_arrivals"), ("messaging", "greetings")]:
        world = worlds.build(applet, scenario)
        before = identity(world)
        scenario_file = APPLETS / applet / "scenarios" / scenario / "scenario.toml"
        traces = [work / "first.ndjson", work / "second.ndjson", work / "explicit.ndjson"]
        for trace in traces[:2]:
            worlds.expect(world, [scenario_file, "--trace", trace], 0)
        if traces[0].read_bytes() != traces[1].read_bytes():
            raise RuntimeError(f"{applet}/{scenario} wrote different traces in two runs")
        worlds.expect(world, [scenario_file, "--settings", work / "partial.toml", "--trace", traces[2]], 0)
        implicit = traces[0].read_bytes().splitlines()
        explicit = traces[2].read_bytes().splitlines()
        # The header records the settings file's digest, so only the rows after it must agree.
        if json.loads(implicit[0])["seed"] != 1 or implicit[1:] != explicit[1:]:
            raise RuntimeError(f"{applet}/{scenario} did not use the built-in defaults")
        if identity(world) != before:
            raise RuntimeError(f"{world.name} changed during its runs")
        print(f"{applet}/{scenario}: repeatable", flush=True)

    (work / "partial.toml").write_text("seed = 17\n")
    worlds.expect(world, [scenario_file, "--settings", work / "partial.toml", "--trace", traces[0]], 0)
    if json.loads(traces[0].read_text().splitlines()[0])["seed"] != 17:
        raise RuntimeError("the world ignored the explicit seed override")


def check_command_line(worlds: Worlds) -> None:
    work = worlds.work
    world = worlds.build("idle", "tub_arrivals")
    scenario = APPLETS / "idle" / "scenarios" / "tub_arrivals" / "scenario.toml"
    if not worlds.expect(world, ["--help"], 0).stdout.startswith("Use: "):
        raise RuntimeError("--help did not print the usage")
    if "version 1.0.0" not in worlds.expect(world, ["--version"], 0).stdout:
        raise RuntimeError("--version did not print the version")
    worlds.expect(world, [], 2, "A scenario file is required")

    (work / "broken.spec").write_text("prop broken = always (\n")
    for name, properties in [("broken", "broken.spec"), ("unreadable", "missing.spec")]:
        (work / f"{name}.toml").write_text(f'[scenario]\nduration = "1s"\nproperties = "{properties}"\n')
    for args, message in [
        (["--seed", "-1"], "Invalid seed: -1"),
        (["--seed", "abc"], "Invalid seed: abc"),
        (["--seed"], "Missing value for option --seed"),
        (["--timeout", "0"], "Invalid timeout: 0"),
        (["--timeout", "nan"], "Invalid timeout: nan"),
        (["--timeout", "1e300"], "Timeout is too large"),
        (["--speed", "2"], "--speed requires --gui"),
        (["--gui", "--speed", "-1"], "Invalid speed: -1"),
        (["--bogus"], "Unknown option: --bogus"),
        (["--settings", work / "missing.toml"], "No settings file found"),
        (["--trace", work / "missing" / "trace.ndjson"], "Could not write the trace: Failed to open the trace file"),
    ]:
        worlds.expect(world, [scenario, *args], 2, message)
    worlds.expect(world, [work / "broken.toml"], 2, "Could not read the properties: ", "expected ';'")
    worlds.expect(world, [work / "unreadable.toml"], 2, "Could not read the properties: ", "missing.spec")
    if (work / "cache" / "qsim" / "sdl3").exists():
        raise RuntimeError("a headless run extracted SDL3")


def gui_available(world: Path) -> bool:
    """A Cosmopolitan world embeds SDL3 in its zip archive. A host-compiler world loads the system's."""
    if zipfile.is_zipfile(world):
        with zipfile.ZipFile(world) as archive:
            return any(name.startswith("sdl3/") for name in archive.namelist())
    import ctypes  # Only host-compiler worlds get here, under the host's Python: the bundle's has no ctypes.

    try:
        ctypes.CDLL("libSDL3.so.0")
    except OSError:
        return False
    return True


def run_until_interrupted(worlds: Worlds, world: Path, args: list[object], ready, seconds: float) -> Path:
    """Start a GUI run, wait until ready(log) or for the given seconds, then send Ctrl+C."""
    log = worlds.work / "gui.log"
    with log.open("w") as output:
        process = subprocess.Popen([world, *map(str, args)], cwd=worlds.work,
                                   env={**worlds.env, "SDL_VIDEODRIVER": "dummy"}, stdout=output,
                                   stderr=output, start_new_session=True)
        try:
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline and process.poll() is None and not ready(log.read_text()):
                time.sleep(0.05)
            if process.poll() is not None:
                raise RuntimeError(f"the GUI run ended early with {process.returncode}\n{log.read_text()}")
            os.killpg(process.pid, signal.SIGINT)
            if process.wait(timeout=5) != 130:
                raise RuntimeError(f"Ctrl+C did not stop the GUI run\n{log.read_text()}")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    return log


def check_gui(worlds: Worlds) -> None:
    if platform.system() == "Windows":
        raise SystemExit(SKIPPED)
    world = worlds.build("idle", "tub_arrivals")
    if not gui_available(world):
        print("skipped: this world cannot load SDL3", flush=True)
        raise SystemExit(SKIPPED)
    work = worlds.work
    scenario = APPLETS / "idle" / "scenarios" / "tub_arrivals" / "scenario.toml"

    # Paused, Ctrl+C after firmware boot must still flush the trace.
    (work / "paused.toml").write_text("simulation_speed = 0.0\n")
    trace = work / "paused.ndjson"
    run_until_interrupted(worlds, world, [scenario, "--gui", "--speed", "3", "--settings", work / "paused.toml",
                                          "--timeout", "0.001", "--trace", trace],
                          lambda log: "Booting Zephyr OS" in log, 10)
    last = json.loads(trace.read_text().splitlines()[-1])
    if last["type"] != "run_complete":
        raise RuntimeError(f"the GUI did not flush the trace after Ctrl+C: {last}")

    # Moving tubs at 4x speed for 3 s, on modules in all four orientations: every frame draws the
    # modules, their belts, lasers and readers, and the tubs.
    layout = work / "rotations.toml"
    layout.write_text(f'firmware = "{(worlds.firmware_dir / "conveyor.o").as_posix()}"\n' + "".join(
        f"\n[module.{index + 1}]\nactive = true\nneighbors = [0, 0, 0]\npos = [{2 * index}, 0]\nangle = {angle}\n"
        for index, angle in enumerate((0, 90, 180, 270))))
    world = work / "rotations.exe"
    subprocess.run([sys.executable, worlds.qsim, "build", layout, "-o", world], check=True)
    scenario = work / "rotations_scenario.toml"
    scenario.write_text('[scenario]\nduration = "5s"\n' + "".join(
        f"\n[tub.t{module}]\ntime = 0.0\nid = {module}\nmodule = {module}\nuid = 0x0{module}000000000000\n"
        f"payload = [0x0000000{module}]\n" for module in range(1, 5)))
    trace = work / "moving.ndjson"
    seconds, speed = 3, 4
    run_until_interrupted(worlds, world, [scenario, "--gui", "--speed", speed, "--trace", trace],
                          lambda log: False, seconds)
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    # Loose bound: a quarter of the requested speed, so that a loaded host still passes.
    minimum_tick = seconds * speed * 1000 // 4
    if rows[-1]["type"] != "run_complete" or rows[-1]["tick"] < minimum_tick:
        raise RuntimeError(f"the GUI run did not advance to tick {minimum_tick}: {rows[-1]}")
    if not any(row["type"] == "tub" for row in rows):
        raise RuntimeError("the GUI run did not move the tub")


CHECKS = {"repeatability": check_repeatability, "command_line": check_command_line, "gui": check_gui}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("qsim", type=Path, help="the qsim world builder of a simulator build")
    parser.add_argument("firmware_dir", type=Path, help="the directory with the built applet objects")
    parser.add_argument("check", choices=CHECKS, nargs="?", help="run only this check (default: all)")
    args = parser.parse_args()
    with scratch_directory() as directory:
        worlds = Worlds(args.qsim.resolve(), args.firmware_dir.resolve(), Path(directory))
        for name in [args.check] if args.check else list(CHECKS):
            CHECKS[name](worlds)
            print(f"{name}: passed", flush=True)


if __name__ == "__main__":
    main()
