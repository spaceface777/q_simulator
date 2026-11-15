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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("qsim", type=Path)
    parser.add_argument("--gui-smoke", action="store_true", help="test Ctrl+C with bundled SDL3 on POSIX")
    args = parser.parse_args()
    if args.gui_smoke and sys.platform == "win32":
        parser.error("--gui-smoke requires POSIX signals")
    qsim = args.qsim.resolve()
    root = Path(__file__).resolve().parents[2]
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
