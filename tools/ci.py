#!/usr/bin/env python3
"""Cross-platform setup and bounded execution for GitHub Actions."""

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
RESULTS = BUILD / "ci-results"
WINDOWS = sys.platform == "win32"


def run(*command, cwd=ROOT, timeout=1200, log=None, env=None):
    command = [str(part) for part in command]
    print("+", subprocess.list2cmdline(command), flush=True)
    # A process group lets the timeout stop CTest and any child test processes.
    with subprocess.Popen(
        command, cwd=cwd, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        start_new_session=not WINDOWS,
    ) as process:
        expired = threading.Event()

        def terminate():
            expired.set()
            if WINDOWS:
                subprocess.run(["taskkill", "/F", "/T", "/PID", str(process.pid)], check=False)
            else:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

        timer = threading.Timer(timeout, terminate)
        output = None
        try:
            if log:
                RESULTS.mkdir(parents=True, exist_ok=True)
                output = (RESULTS / log).open("wb")
            timer.start()
            for line in process.stdout:
                sys.stdout.buffer.write(line)
                sys.stdout.buffer.flush()
                if output:
                    output.write(line)
            code = process.wait()
        finally:
            timer.cancel()
            if output:
                output.close()
        if expired.is_set():
            raise RuntimeError(f"Command exceeded {timeout} seconds")
        if code:
            raise subprocess.CalledProcessError(code, command)


def zephyr():
    workspace = ROOT / ".ci-zephyr"
    checkout = workspace / "zephyr"
    revision = (ROOT / "external/q_sdk/ZEPHYR_REVISION").read_text().strip()
    checkout.mkdir(parents=True, exist_ok=True)
    if not (checkout / ".git").exists():
        run("git", "init", checkout)
    for attempt in range(3):
        try:
            run("git", "-C", checkout, "fetch", "--depth=1",
                "https://github.com/spaceface777/zephyr.git", revision, timeout=180)
            break
        except (subprocess.CalledProcessError, RuntimeError):
            if attempt == 2:
                raise
            print("Retrying Zephyr fetch after a network failure", flush=True)
            time.sleep(5 * (attempt + 1))
    run("git", "-C", checkout, "checkout", "--detach", revision)
    actual = subprocess.check_output(["git", "-C", str(checkout), "rev-parse", "HEAD"], text=True).strip()
    if actual != revision:
        raise RuntimeError(f"Zephyr revision differs: {actual} != {revision}")
    west_env = os.environ.copy()
    west_env.pop("ZEPHYR_BASE", None)
    if not (workspace / ".west").exists():
        run("west", "init", "-l", "zephyr", cwd=workspace, env=west_env)
    west_env["ZEPHYR_BASE"] = checkout.as_posix()
    # Native images need no hardware HALs or cross compiler SDK.
    run("west", "config", "manifest.project-filter", "--", "-.*", cwd=workspace, env=west_env)
    run(sys.executable, "-m", "pip", "install", "-r", checkout / "scripts/requirements-base.txt")
    with Path(os.environ["GITHUB_ENV"]).open("a") as output:
        output.write(f"ZEPHYR_BASE={checkout.as_posix()}\n")


def python_tests():
    run(sys.executable, "tools/check_sdk.py")
    run(sys.executable, "-m", "unittest", "discover", "-s", ROOT / "test", "-p", "test_*.py")
    for pattern in ("test_qbuild.py", "test_zephyr_revision.py"):
        run(sys.executable, "-m", "unittest", "discover", "-s", ROOT / "external/q_sdk/test", "-p", pattern)


def firmware():
    run(sys.executable, ROOT / "external/q_applets/q_sdk/qbuild", "sim",
        cwd=ROOT / "external/q_applets", timeout=1800, log="applets.log")
    run(sys.executable, "external/q_sdk/qbuild", "sim", log="probes.log")
    run(sys.executable, "tools/check_sdk.py")


def binary(name):
    return BUILD / (name + (".exe" if WINDOWS else ""))


def configure():
    run("cmake", "-S", ".", "-B", BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DTESTING_CONSOLE_BUILD=OFF",
        "-DBUILD_TESTING=ON", "-DQSIM_TEST_EMBEDDED_TRACE=ON",
        f"-DPython3_EXECUTABLE={sys.executable}", log="configure.log")


def smoke():
    run(binary("simulation"), "--version", timeout=30, log="version.log")
    run(binary("simulation"), "--help", timeout=30, log="gui-help.log")
    run(binary("simulation-scenario"), "--help", timeout=30, log="scenario-help.log")
    run(binary("simulation-scenario"), "--settings", "settings_example.toml",
        "--trace", RESULTS / "module_movement.ndjson", "scenarios/module_movement.toml",
        timeout=180, log="scenario.log")
    # Exercise the GUI executable's runtime without requiring a desktop session.
    # The GUI reads settings.toml from its working directory. Use an isolated copy
    # so local settings cannot affect this check or be overwritten by it.
    settings = (ROOT / "settings_example.toml").read_text()
    (RESULTS / "settings.toml").write_text(settings)
    run(binary("simulation"), "--no-gui", "--timeout", "2", ROOT / "scenarios/module_movement.toml",
        cwd=RESULTS, timeout=60, log="simulation.log")


def test():
    RESULTS.mkdir(parents=True, exist_ok=True)
    run("ctest", "--test-dir", BUILD, "--output-on-failure", "--no-tests=error",
        "--timeout", "1200", "--output-junit", RESULTS / "ctest.xml",
        timeout=1260, log="ctest.log")


def repeat():
    run(binary("tests"), "--gtest_shuffle", "--gtest_repeat=10",
        f"--gtest_output=xml:{RESULTS / 'repeat.xml'}", timeout=600, log="repeat.log")


if __name__ == "__main__":
    commands = {"zephyr": zephyr, "python-tests": python_tests,
                "firmware": firmware, "configure": configure, "smoke": smoke, "test": test, "repeat": repeat}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=commands)
    try:
        commands[parser.parse_args().command]()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
