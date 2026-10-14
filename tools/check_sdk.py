#!/usr/bin/env python3
"""Check that both simulator SDK checkouts use one clean revision."""
from pathlib import Path
import subprocess
import sys


def git(path, *arguments):
    return subprocess.check_output(["git", "-C", str(path), *arguments], text=True).strip()


def check(root):
    paths = [root / "external/q_sdk", root / "external/q_applets/q_sdk"]
    revisions = [git(path, "rev-parse", "HEAD") for path in paths]
    if revisions[0] != revisions[1]:
        raise RuntimeError(f"SDK revisions differ: {paths[0]}={revisions[0]}, {paths[1]}={revisions[1]}")
    for path in paths:
        if git(path, "status", "--porcelain"):
            raise RuntimeError(f"SDK checkout is not clean: {path}")
    print(f"Both SDK checkouts are clean at {revisions[0]}")


if __name__ == "__main__":
    try:
        check(Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else Path(__file__).resolve().parents[1])
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
