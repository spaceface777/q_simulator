"""Reuse linked worlds and check repeatability with runtime settings overrides."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import zipfile


def launch(world: Path, *args: object, cwd: Path, env: dict[str, str]) -> subprocess.CompletedProcess:
    command = [world] if sys.platform == "win32" else ["sh", world]
    return subprocess.run([*command, *map(str, args)], cwd=cwd, env=env, capture_output=True, text=True)


def world_identity(world: Path) -> tuple[int, bytes]:
    with world.open("rb") as stream:
        return world.stat().st_mtime_ns, hashlib.file_digest(stream, "sha256").digest()


def check_gui_shutdown(world: Path, work: Path, env: dict[str, str]) -> None:
    scenario = work / "gui.toml"
    scenario.write_text('[scenario]\nduration = "20ms"\n')
    settings = work / "paused.toml"
    settings.write_text("simulation_speed = 0.0\n")
    trace = work / "gui.ndjson"
    log = work / "gui.log"
    with log.open("w") as output:
        process = subprocess.Popen(
            ["sh", world, scenario, "--gui", "--speed", "3", "--settings", settings,
             "--timeout", "0.001", "--trace", trace],
            cwd=work, env={**env, "SDL_VIDEODRIVER": "dummy"}, stdout=output, stderr=output,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and process.poll() is None:
                if "Booting Zephyr OS" in log.read_text():
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError(f"the GUI did not run through firmware boot\n{log.read_text()}")
            os.killpg(process.pid, signal.SIGINT)
            if process.wait(timeout=5) != 130:
                raise RuntimeError(f"Ctrl+C did not stop the GUI runner\n{log.read_text()}")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    last = json.loads(trace.read_text().splitlines()[-1])
    if last["type"] != "run_complete" or last["tick"] <= 20:
        raise RuntimeError(f"the GUI did not flush the trace after Ctrl+C: {last}")


def check_gui_run(world: Path, scenario: Path, work: Path, env: dict[str, str]) -> None:
    """Run a moving scenario in the GUI for a few seconds, so that every frame draws modules and tubs."""
    trace = work / "gui_run.ndjson"
    log = work / "gui_run.log"
    seconds, speed = 3, 4
    with log.open("w") as output:
        process = subprocess.Popen(
            ["sh", world, scenario, "--gui", "--speed", str(speed), "--trace", trace],
            cwd=work, env={**env, "SDL_VIDEODRIVER": "dummy"}, stdout=output, stderr=output,
            start_new_session=True,
        )
        try:
            time.sleep(seconds)
            if process.poll() is not None:
                raise RuntimeError(f"the GUI run ended early with {process.returncode}\n{log.read_text()}")
            os.killpg(process.pid, signal.SIGINT)
            if process.wait(timeout=5) != 130:
                raise RuntimeError(f"Ctrl+C did not stop the GUI run\n{log.read_text()}")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    # Loose bound: a quarter of the requested speed, so that a loaded host still passes.
    minimum_tick = seconds * speed * 1000 // 4
    if rows[-1]["type"] != "run_complete" or rows[-1]["tick"] < minimum_tick:
        raise RuntimeError(f"the GUI run did not advance to tick {minimum_tick}: {rows[-1]}")
    if not any(row["type"] == "tub" for row in rows):
        raise RuntimeError("the GUI run did not move any tubs")


TUB_AT_MODULE_1 = """
[tub.the_only_tub]
time = 0.0
id = 1
module = 1
uid = 0x01000000000000
payload = [0xC0FFEE01]
"""

# Against module_movement: the servo stays in range, the LED turns red, and it is never magenta.
FAILING_PROPERTIES = """
let m = module(1);

prop servo_stays_in_range =
    always (m.servo_physical_angle >= 0.0 && m.servo_physical_angle <= 120.0);

prop led_stays_dark =
    always (m.led_color == 0x000000);

prop led_turns_magenta_quickly =
    eventually[0ms, 100ms] (m.led_color == 0xff00ff);
"""


def verdict_lines(stderr: str) -> list[str]:
    """Return the verdict reports and their indented details, without firmware or startup output."""
    lines = []
    for line in stderr.splitlines():
        if line.startswith(("PASS ", "FAIL ", "UNRESOLVED ", "CANCELLED ")):
            lines.append(line)
        elif line.startswith("  ") and lines:
            lines.append(line)
    return lines


def expect_run(world: Path, work: Path, env: dict[str, str], args: list[object], code: int,
               *messages: str) -> subprocess.CompletedProcess:
    result = launch(world, *args, cwd=work, env=env)
    missing = [message for message in messages if message not in result.stderr]
    if result.returncode != code or missing:
        raise RuntimeError(f"{' '.join(map(str, args))} returned {result.returncode}, expected {code}; "
                           f"missing {missing}\n{result.stderr}")
    return result


def check_failure_reports(world: Path, work: Path, env: dict[str, str]) -> None:
    (work / "failing.spec").write_text(FAILING_PROPERTIES)
    scenario = work / "failing.toml"
    scenario.write_text(f'[scenario]\nduration = "2s"\nproperties = "failing.spec"\n{TUB_AT_MODULE_1}')
    deadline_report = [
        "FAIL led_turns_magenta_quickly at 0.100 s",
        f"  source: {work / 'failing.spec'}:10:1",
        "  obligation created at 0.000 s",
        "  deadline: 0.100 s",
        "  formula: eventually[0ms, 100ms] (module(1).led_color == 0xff00ff)",
        "  obligation: eventually[0ms, 100ms] (module(1).led_color == 0xff00ff)",
        "  module(1).led_color == 0xff00ff = false, was 0x000000",
    ]

    # By default the first failure ends the run, and only decided properties are reported.
    result = expect_run(world, work, env, [scenario], 1, "1 of 3 properties failed after 101 ticks")
    if verdict_lines(result.stderr) != deadline_report:
        raise RuntimeError(f"unexpected report for the first failure\n{result.stderr}")

    # --keep-going runs to the scenario end and reports every verdict. Passes go to stdout.
    result = expect_run(world, work, env, [scenario, "--keep-going"], 1, "2 of 3 properties failed after 2001 ticks")
    lines = verdict_lines(result.stderr)
    always_report = [line for line in lines if line.startswith("FAIL led_stays_dark at ")]
    if ("PASS servo_stays_in_range" not in result.stdout.splitlines() or len(always_report) != 1
            or lines[-len(deadline_report):] != deadline_report
            or "  module(1).led_color == 0x000000 = false, was 0xff0000" not in lines):
        raise RuntimeError(f"unexpected reports with --keep-going\n{result.stderr}")


def check_command_line_errors(world: Path, work: Path, env: dict[str, str]) -> None:
    scenario = work / "failing.toml"
    result = expect_run(world, work, env, ["--help"], 0)
    if not result.stdout.startswith("Use: "):
        raise RuntimeError(f"--help did not print the usage\n{result.stdout}")
    result = expect_run(world, work, env, ["--version"], 0)
    if "version 1.0.0" not in result.stdout:
        raise RuntimeError(f"--version did not print the version\n{result.stdout}")

    configuration_errors = [
        (["--seed", "-1"], "Invalid seed: -1"),
        (["--seed", "abc"], "Invalid seed: abc"),
        (["--seed"], "Missing value for option --seed"),
        (["--timeout", "0"], "Invalid timeout: 0"),
        (["--timeout", "nan"], "Invalid timeout: nan"),
        (["--timeout", "1e300"], "Timeout is too large"),
        (["--speed", "2"], "--speed requires --gui"),
        (["--bogus"], "Unknown option: --bogus"),
    ]
    for args, message in configuration_errors:
        expect_run(world, work, env, [scenario, *args], 2, message)

    (work / "broken.spec").write_text("prop broken = always (\n")
    for name, properties, message in [
        ("broken", "broken.spec", "broken.spec:1:1: expected ';' after prop declaration"),
        ("unreadable", "missing.spec", "missing.spec"),
    ]:
        (work / f"{name}.toml").write_text(f'[scenario]\nduration = "2s"\nproperties = "{properties}"\n')
        expect_run(world, work, env, [work / f"{name}.toml"], 2, "Could not read the properties: ", message)

    expect_run(world, work, env, [scenario, "--trace", work / "missing" / "trace.ndjson"], 2,
               "Could not write the trace: Failed to open the trace file at")

    # A host timeout cancels pending properties, with or without --keep-going.
    for extra in ([], ["--keep-going"]):
        result = expect_run(world, work, env, [scenario, "--timeout", "0.00001", *extra], 1,
                            "The run timed out with 3 of 3 properties still pending after 1 ticks")
        cancelled = [line for line in verdict_lines(result.stderr) if line.startswith("CANCELLED ")]
        if cancelled != [f"CANCELLED {name} at 0.000 s (still pending)" for name in
                         ("servo_stays_in_range", "led_stays_dark", "led_turns_magenta_quickly")]:
            raise RuntimeError(f"the timeout did not cancel every property\n{result.stderr}")
    events_only = work / "events_only.toml"
    events_only.write_text(f'[scenario]\nduration = "2s"\n{TUB_AT_MODULE_1}')
    expect_run(world, work, env, [events_only, "--timeout", "0.00001"], 1, "The run timed out after 1 ticks")


def check_crash_containment(qsim: Path, root: Path, crash_applet: Path, work: Path, env: dict[str, str]) -> None:
    """Crash module 1 of a two-module world and check that module 2 runs on to the scenario end."""
    if not crash_applet.is_file():
        raise RuntimeError(f"{crash_applet} is missing. Build it with: "
                           "(cd test/firmware/applets && ../../../external/q_sdk/qbuild sim)")
    layout = work / "crash_layout.toml"
    layout.write_text(
        f'firmware = "{root / "external/q_applets/out/module_movement.o"}"\n\n'
        f'[module.1]\nactive = true\nfirmware = "{crash_applet}"\nneighbors = [0, 0, 0]\npos = [0, 0]\nangle = 0\n\n'
        '[module.2]\nactive = true\nneighbors = [0, 0, 0]\npos = [5, 5]\nangle = 0\n'
    )
    world = work / "crash.exe"
    subprocess.run([sys.executable, qsim, "build", layout, "-o", world], check=True)
    scenario = work / "crash.toml"
    scenario.write_text('[scenario]\nduration = "1s"\n')
    trace = work / "crash.ndjson"
    result = expect_run(world, work, env, [scenario, "--trace", trace], 1,
                        "Module 1 crashed: segmentation fault (SIGSEGV) at tick ")
    # The report symbolizes the faulting frame from the applet's debug information.
    if not any(line.startswith("  #0 ") and "write_through at " in line and "crash_app/src/main.c:10" in line
               for line in result.stderr.splitlines()):
        raise RuntimeError(f"the crash report did not name the faulting line\n{result.stderr}")
    rows = [json.loads(line) for line in trace.read_text().splitlines()]
    stops = [row for row in rows if row["type"] == "module_stopped"]
    if [(row["module"], row["reason"]) for row in stops] != [(1, "crashed: segmentation fault (SIGSEGV)")]:
        raise RuntimeError(f"the trace did not record only the crashed module stopping: {stops}")
    if rows[-1] != {"type": "run_complete", "tick": 1001}:
        raise RuntimeError(f"the run did not continue to the scenario end after the crash: {rows[-1]}")
    if not any(row.get("module") == 2 and row.get("tick", 0) > stops[0]["tick"] for row in rows):
        raise RuntimeError("the healthy module stopped running after the crash")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("qsim", type=Path)
    parser.add_argument("--gui-smoke", action="store_true", help="test Ctrl+C with bundled SDL3 on POSIX")
    parser.add_argument("--crash-applet", type=Path,
                        help="crashing firmware object (default: test/firmware/applets/out/crash_app.o)")
    args = parser.parse_args()
    if args.gui_smoke and sys.platform == "win32":
        parser.error("--gui-smoke requires POSIX signals")
    qsim = args.qsim.resolve()
    root = Path(__file__).resolve().parents[2]
    crash_applet = (args.crash_applet or root / "test/firmware/applets/out/crash_app.o").resolve()
    # The P2 process runner reports these same verdicts with the pinned applet.
    expected_unresolved = {
        "hardware_test": "reaches_async_monitoring_and_starts_every_periodic_rpc_stream",
        "hardware_test_tag_at_startup": "startup_tag_is_classified_from_a_successful_rfid_read",
    }
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        worlds = {}
        identities = {}
        runtime_env = os.environ.copy()
        runtime_env.pop("COSMOCC_TOOLCHAIN_PATH", None)
        sdl_cache = work / "cache"
        runtime_env["LOCALAPPDATA" if sys.platform == "win32" else "XDG_CACHE_HOME"] = str(sdl_cache)
        partial_settings = work / "partial.toml"
        partial_settings.write_text("seed = 1\n")
        # Its presence must not affect runs with no explicit settings file.
        (work / "settings.toml").write_text("seed = 123\n")
        layouts = {
            "hardware_test": "hardware_test",
            "hardware_test_blocked_beam": "hardware_test",
            "hardware_test_tag_at_startup": "hardware_test",
            "laser_alignment": "laser_alignment",
            "module_movement": "one_module",
            "module_movement_idle": "one_module",
            "per_module_variation": "default",
            "tub_arrivals": "default",
        }
        scenarios = {path.stem: path for path in (root / "scenarios").glob("*.toml")}
        if scenarios.keys() != layouts.keys():
            raise RuntimeError("each bundled scenario needs a matching world layout")
        for name, layout_name in layouts.items():
            scenario = scenarios[name]
            layout = root / "layouts" / f"{layout_name}.toml"
            if layout not in worlds:
                world = work / f"{layout.stem}.exe"
                subprocess.run([sys.executable, qsim, "build", layout, "-o", world], check=True)
                worlds[layout] = world
                identities[world] = world_identity(world)
                result = launch(world, cwd=work, env=runtime_env)
                if result.returncode != 2 or "A scenario file is required" not in result.stderr:
                    raise RuntimeError(f"{world.name} accepted a missing scenario argument")
            world = worlds[layout]
            traces = [work / "first.ndjson", work / "second.ndjson"]
            for trace in traces:
                result = launch(world, scenario, "--trace", trace, cwd=work, env=runtime_env)
                verdicts = [line for line in result.stderr.splitlines() if line.startswith(("FAIL ", "UNRESOLVED "))]
                expected = expected_unresolved.get(scenario.stem)
                if expected is None:
                    ok = result.returncode == 0 and not verdicts
                else:
                    ok = result.returncode == 1 and len(verdicts) == 1 and verdicts[0].startswith(
                        f"UNRESOLVED {expected} at ")
                if not ok:
                    raise RuntimeError(f"{scenario.name} returned {result.returncode}\n{result.stdout}{result.stderr}")
            if traces[0].read_bytes() != traces[1].read_bytes():
                raise RuntimeError(f"{scenario.name} produced different traces")
            result = launch(world, scenario, "--settings", partial_settings, "--trace", traces[1],
                            cwd=work, env=runtime_env)
            if result.returncode != (1 if expected else 0):
                raise RuntimeError(f"{scenario.name} failed with partial settings\n{result.stderr}")
            implicit = traces[0].read_bytes().splitlines()
            explicit = traces[1].read_bytes().splitlines()
            if json.loads(implicit[0])["seed"] != 1 or implicit[1:] != explicit[1:]:
                raise RuntimeError(f"{scenario.name} did not use the built-in defaults")
            if name == "tub_arrivals":
                if json.loads(implicit[-1]) != {"type": "run_complete", "tick": 45001}:
                    raise RuntimeError("the events-only scenario did not run through its duration")
                if not any(json.loads(row).get("type") == "tub" for row in implicit[1:]):
                    raise RuntimeError("the events-only scenario did not produce tub events")
            print(f"{scenario.name}: {'one undecided property' if expected else 'passed'}", flush=True)
        scenario = root / "scenarios/hardware_test_blocked_beam.toml"
        world = next(path for path in identities if path.stem == "hardware_test")
        result = launch(world, scenario, "--settings", work / "missing.toml", cwd=work, env=runtime_env)
        if result.returncode != 2 or "No settings file found" not in result.stderr:
            raise RuntimeError("the world accepted an explicitly missing settings file")
        if (sdl_cache / "qsim" / "sdl3").exists():
            raise RuntimeError("a headless run extracted SDL3")
        embeds_sdl = False
        if zipfile.is_zipfile(world):
            with zipfile.ZipFile(world) as archive:
                embeds_sdl = any(name.startswith("sdl3/") for name in archive.namelist())
        if not embeds_sdl:
            result = launch(world, scenario, "--gui", cwd=work, env=runtime_env)
            if result.returncode != 2 or not any(message in result.stderr for message in (
                "GUI support is unavailable", "This world does not contain",
            )):
                raise RuntimeError("the world did not report the unavailable GUI")
        result = launch(world, scenario, "--gui", "--speed", "-1", cwd=work, env=runtime_env)
        if result.returncode != 2 or "Invalid speed" not in result.stderr:
            raise RuntimeError("the world accepted a negative GUI speed")
        if args.gui_smoke:
            check_gui_shutdown(worlds[root / "layouts/one_module.toml"], work, runtime_env)
            check_gui_run(worlds[root / "layouts/default.toml"], scenarios["tub_arrivals"], work, runtime_env)
            print("GUI: ran and stopped", flush=True)
        check_failure_reports(worlds[root / "layouts/one_module.toml"], work, runtime_env)
        print("failing properties: reported", flush=True)
        check_command_line_errors(worlds[root / "layouts/one_module.toml"], work, runtime_env)
        print("command line errors: rejected", flush=True)
        check_crash_containment(qsim, root, crash_applet, work, runtime_env)
        print("firmware crash: contained", flush=True)
        partial_settings.write_text("seed = 17\n")
        result = launch(world, scenario, "--settings", partial_settings, "--trace", traces[0],
                        cwd=work, env=runtime_env)
        if result.returncode != 0:
            raise RuntimeError(f"the world failed with a custom seed\n{result.stderr}")
        with traces[0].open() as trace:
            if json.loads(trace.readline())["seed"] != 17:
                raise RuntimeError("the world ignored the explicit seed override")
        for world, identity in identities.items():
            if world_identity(world) != identity:
                raise RuntimeError(f"{world.name} changed during scenario runs")


if __name__ == "__main__":
    main()
