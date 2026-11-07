#!/usr/bin/env python3
"""Assemble the portable Windows toolchain ZIPs from pinned upstream releases.

One ZIP is produced per Windows host architecture (arm64 and x86_64). Each holds a
single relocatable root with LLVM-MinGW, CMake, Ninja, ccache and GNU Make sharing
bin/, include/, lib/ and share/, and builds i686 (32-bit x86) Windows programs.

Every download is verified against the SHA-256 recorded in components.json. Once the
downloads are cached, --offline assembles without network access. Merging fails if
two components install different files at the same path (compared case-insensitively,
as Windows does), if a component file matches no mapping rule, or if a host program
has the wrong architecture or imports a DLL that is neither a Windows system DLL nor
part of the package.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
FILES = HERE / "files"

MACHINES = {0x14C: "i386", 0x8664: "x86_64", 0xAA64: "arm64", 0x1C4: "armv7"}
TARGET_MACHINES = {
    "i686-w64-mingw32": 0x14C,
    "x86_64-w64-mingw32": 0x8664,
    "aarch64-w64-mingw32": 0xAA64,
    "armv7-w64-mingw32": 0x1C4,
}

# DLLs that every supported Windows version (Windows 10 1809 and later, x64 or Arm64)
# provides. A host program may import these and nothing else from outside the package.
SYSTEM_DLLS = {
    name.lower()
    for name in """
    advapi32 bcrypt comctl32 comdlg32 crypt32 d3d11 d3d9 dbghelp dnsapi dwmapi dwrite
    dxgi gdi32 imm32 iphlpapi kernel32 mpr msvcrt netapi32 ntdll ole32 oleaut32 opengl32
    powrprof psapi rpcrt4 secur32 setupapi shcore shell32 shlwapi user32 userenv uxtheme
    version winhttp wininet winmm winspool.drv ws2_32 wsock32 ucrtbase cfgmgr32 authz
    msimg32 normaliz synchronization wtsapi32 mswsock d3d12 icu icuuc icuin
    """.split()
}


def die(message: str) -> None:
    raise SystemExit(f"error: {message}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url: str, sha256: str, cache: Path, offline: bool) -> Path:
    """Return the cached download for url, downloading it first if needed."""
    name = PurePosixPath(url).name
    target = cache / sha256[:16] / name
    if target.is_file():
        actual = sha256_file(target)
        if actual != sha256:
            die(f"cached {target} has SHA-256 {actual}, expected {sha256}; delete it")
        return target
    if offline:
        die(f"{name} is not in the cache {cache} and --offline was given")
    target.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {url}", flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "quercus-toolchain-assembler"})
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as temp:
        try:
            with urllib.request.urlopen(request, timeout=120) as response:
                shutil.copyfileobj(response, temp, 1 << 20)
        except BaseException:
            os.unlink(temp.name)
            raise
    actual = sha256_file(Path(temp.name))
    if actual != sha256:
        os.unlink(temp.name)
        die(f"{url} has SHA-256 {actual}, expected {sha256}")
    os.replace(temp.name, target)
    return target


def safe_relative(name: str) -> PurePosixPath:
    path = PurePosixPath(name)
    if (
        not name
        or "\\" in name
        or path.is_absolute()
        or any(part in ("", ".", "..") for part in path.parts)
        or ":" in name
    ):
        die(f"unsafe path in archive: {name!r}")
    return path


class Tree:
    """A staging directory that records which component owns each file."""

    def __init__(self, root: Path):
        self.root = root
        self.owners: dict[str, tuple[str, str, str]] = {}  # folded path -> (path, owner, sha256)
        self.identical: list[tuple[str, str, str]] = []

    def add(self, path: str, data: bytes, owner: str) -> None:
        safe_relative(path)
        digest = hashlib.sha256(data).hexdigest()
        key = path.casefold()
        if key in self.owners:
            existing, previous, previous_digest = self.owners[key]
            if previous_digest != digest:
                die(f"{owner} and {previous} both install {path} with different contents")
            if existing != path:
                die(f"{owner} installs {path}, which differs only in case from {existing}")
            self.identical.append((path, previous, owner))
            return
        for parent in PurePosixPath(path).parents:
            if str(parent) != "." and str(parent).casefold() in self.owners:
                die(f"{owner} installs {path} below the file {parent}")
        self.owners[key] = (path, owner, digest)
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def files(self) -> list[str]:
        return sorted(entry[0] for entry in self.owners.values())


def map_path(rules: list[list[str]], path: str) -> str | None:
    for source, destination in rules:
        if source.endswith("/") or source == "":
            if path.startswith(source):
                return destination + path[len(source) :]
        elif path == source:
            return destination
    return None


def add_component(tree: Tree, component: dict, host: str, cache: Path, offline: bool) -> dict:
    archive = component["archives"][host]
    zip_path = fetch(archive["url"], archive["sha256"], cache, offline)
    strip = archive.get("strip", 0)
    count = 0
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            mode = info.external_attr >> 16
            if mode & 0o170000 == 0o120000:
                die(f"{zip_path.name} contains the symbolic link {info.filename}")
            parts = safe_relative(info.filename).parts
            if len(parts) <= strip:
                die(f"{zip_path.name}: {info.filename} is above the stripped prefix")
            inner = "/".join(parts[strip:])
            destination = map_path(component["map"], inner)
            if destination is None:
                die(f"{component['name']}: no mapping rule for {inner}")
            tree.add(destination, zf.read(info), component["name"])
            count += 1
    for extra in component.get("files", []):
        data = fetch(extra["url"], extra["sha256"], cache, offline).read_bytes()
        tree.add(extra["path"], data, component["name"])
        count += 1
    return {"url": archive["url"], "sha256": archive["sha256"], "files": count}


# --- PE inspection ------------------------------------------------------------------


def pe_header(data: bytes) -> tuple[int, int, list[tuple[int, int, int, int]], int] | None:
    """Return (machine, optional header offset, sections, magic) or None for non-PE files."""
    if len(data) < 64 or data[:2] != b"MZ":
        return None
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if offset + 24 > len(data) or data[offset : offset + 4] != b"PE\0\0":
        return None
    machine, count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, offset + 4)
    optional = offset + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    sections = []
    for index in range(count):
        base = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, base + 8)
        sections.append((rva, max(virtual_size, raw_size), raw, raw_size))
    return machine, optional, sections, magic


def pe_imports(data: bytes) -> list[str]:
    header = pe_header(data)
    if header is None:
        return []
    _, optional, sections, magic = header
    directories = optional + (96 if magic == 0x10B else 112)
    rva, size = struct.unpack_from("<II", data, directories + 8)
    if rva == 0 or size == 0:
        return []

    def offset_of(address: int) -> int:
        for start, length, raw, raw_size in sections:
            if start <= address < start + length and address - start < raw_size:
                return raw + address - start
        raise ValueError(f"RVA {address:#x} is outside the file")

    names = []
    position = offset_of(rva)
    while True:
        lookup, _, _, name_rva, thunk = struct.unpack_from("<IIIII", data, position)
        if lookup == 0 and name_rva == 0 and thunk == 0:
            break
        start = offset_of(name_rva)
        names.append(data[start : data.index(b"\0", start)].decode("ascii"))
        position += 20
    return names


def is_system_dll(name: str) -> bool:
    lower = name.lower()
    if lower.startswith(("api-ms-win-", "ext-ms-win-")):
        return True
    return lower.removesuffix(".dll") in SYSTEM_DLLS or lower in SYSTEM_DLLS


def verify_programs(tree: Tree, host_machine: int) -> dict:
    """Check the machine type of every PE file and the imports of host programs."""
    report = {"host_programs": 0, "target_libraries": 0}
    by_folder: dict[str, set[str]] = {}
    for path in tree.files():
        folder, _, name = path.rpartition("/")
        by_folder.setdefault(folder.casefold(), set()).add(name.casefold())
    problems = []
    for path in tree.files():
        if not path.lower().endswith((".exe", ".dll")):
            continue
        data = (tree.root / path).read_bytes()
        header = pe_header(data)
        if header is None:
            problems.append(f"{path}: not a PE file")
            continue
        machine = header[0]
        top = path.split("/", 1)[0]
        expected = TARGET_MACHINES.get(top, host_machine)
        if machine != expected:
            problems.append(
                f"{path}: machine {MACHINES.get(machine, hex(machine))}, "
                f"expected {MACHINES.get(expected, hex(expected))}"
            )
            continue
        if top in TARGET_MACHINES:
            report["target_libraries"] += 1
            continue
        report["host_programs"] += 1
        # Windows finds a DLL next to the program or on PATH, where env.cmd puts bin/.
        folder = path.rpartition("/")[0].casefold()
        visible = by_folder.get(folder, set()) | by_folder.get("bin", set())
        for dll in pe_imports(data):
            if not is_system_dll(dll) and dll.casefold() not in visible:
                problems.append(f"{path}: imports {dll}, which is neither bundled nor a system DLL")
    if problems:
        die("program check failed:\n  " + "\n  ".join(problems))
    return report


# --- generated files ----------------------------------------------------------------


def add_generated(tree: Tree, manifest: dict, host: str, components: list[dict]) -> None:
    for source in sorted(FILES.rglob("*")):
        if source.is_file():
            data = source.read_bytes()
            if source.suffix in (".cmd", ".ps1", ".txt"):
                # Batch files need CRLF line endings; keep the other text files consistent.
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            tree.add(source.relative_to(FILES).as_posix(), data, "quercus")
    host_info = manifest["hosts"][host]
    tree.add(
        "selftest/host.cmake",
        (
            "# Generated by tools/windows_sdk/assemble.py.\n"
            f'set(QTC_HOST "{host}")\n'
            f'set(QTC_HOST_MACHINE "{int(host_info["machine"], 16):04x}")\n'
            f'set(QTC_TARGET "{manifest["target"]}")\n'
        ).encode(),
        "quercus",
    )
    lines = [
        f"{manifest['name']} {manifest['version']} for {host_info['description']}",
        "",
        "A portable toolchain that builds 32-bit x86 (i686) Windows programs with the",
        "Universal C Runtime. Every program in bin/ runs natively on the host.",
        "",
        "Use",
        "",
        "  Command Prompt:  call <this folder>\\env.cmd",
        "  PowerShell:      . <this folder>\\env.ps1",
        "",
        "This puts bin/ first on PATH, selects i686-w64-mingw32-clang and",
        "i686-w64-mingw32-clang++ as the C and C++ compilers (CC and CXX), runs them",
        "through ccache in CMake builds, and selects the Ninja generator.",
        "",
        "The folder can be moved or renamed. Nothing in it refers to its own location.",
        "Run 'cmake -P selftest/run.cmake' to check an installation.",
        "",
        "Components",
        "",
    ]
    for component in components:
        lines.append(f"  {component['name']} {component['version']}  {component['homepage']}")
        lines.append(f"    License: {component['license']}")
        for path in component["license_files"]:
            lines.append(f"    License text: {path}")
        lines.append(f"    Source: {component['url']}")
        lines.append(f"    SHA-256: {component['sha256']}")
        if component.get("notes"):
            lines.append(f"    {component['notes']}")
        lines.append("")
    for copy in manifest.get("copies", []):
        lines.append(f"  {copy['to']} is a copy of {copy['from']}. {copy['reason']}")
    lines.append("")
    tree.add("README.txt", "\r\n".join(lines).encode(), "quercus")


def write_zip(tree: Tree, output: Path, top: str, date: datetime.datetime) -> None:
    timestamp = date.timetuple()[:6]
    temp = output.with_suffix(".zip.partial")
    directories = set()
    for path in tree.files():
        for parent in PurePosixPath(path).parents:
            if str(parent) != ".":
                directories.add(str(parent))
    with zipfile.ZipFile(temp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        root = zipfile.ZipInfo(f"{top}/", timestamp)
        root.external_attr = (0o40755 << 16) | 0x10
        zf.writestr(root, b"")
        for path in sorted(directories | set(tree.files())):
            if path in directories:
                info = zipfile.ZipInfo(f"{top}/{path}/", timestamp)
                info.external_attr = (0o40755 << 16) | 0x10
                zf.writestr(info, b"")
                continue
            info = zipfile.ZipInfo(f"{top}/{path}", timestamp)
            executable = path.lower().endswith((".exe", ".dll", ".cmd", ".ps1"))
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with (tree.root / path).open("rb") as source, zf.open(info, "w") as target:
                shutil.copyfileobj(source, target, 1 << 20)
    os.replace(temp, output)


def assemble(manifest: dict, host: str, cache: Path, output: Path, offline: bool) -> Path:
    if host not in manifest["hosts"]:
        die(f"unknown host {host}")
    top = f"{manifest['name']}-{host}"
    zip_path = output / f"{manifest['name']}-{manifest['version']}-{host}.zip"
    stage = output / f"stage-{host}"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    tree = Tree(stage)
    described = []
    for component in manifest["components"]:
        info = add_component(tree, component, host, cache, offline)
        described.append({**component, **info})
        print(f"  {component['name']} {component['version']}: {info['files']} files", flush=True)
    for copy in manifest.get("copies", []):
        source = copy["from"].casefold()
        if source not in tree.owners:
            die(f"cannot copy missing {copy['from']}")
        tree.add(copy["to"], (stage / tree.owners[source][0]).read_bytes(), "quercus")
    for component in described:
        for path in component["license_files"]:
            if path.casefold() not in tree.owners:
                die(f"{component['name']}: license file {path} is missing")
    add_generated(tree, manifest, host, described)
    report = verify_programs(tree, int(manifest["hosts"][host]["machine"], 16))
    record = {
        "name": manifest["name"],
        "version": manifest["version"],
        "host": host,
        "target": manifest["target"],
        "components": [
            {
                key: component[key]
                for key in ("name", "version", "homepage", "license", "license_files", "url", "sha256")
            }
            for component in described
        ],
        "identical_duplicates": [list(entry) for entry in tree.identical],
        "checked": report,
    }
    tree.add("MANIFEST.json", (json.dumps(record, indent=2) + "\n").encode(), "quercus")
    date = datetime.datetime.fromisoformat(manifest["date"].replace("Z", "+00:00"))
    write_zip(tree, zip_path, top, date)
    digest = sha256_file(zip_path)
    zip_path.with_name(zip_path.name + ".sha256").write_text(f"{digest}  {zip_path.name}\n")
    print(
        f"{zip_path.name}: {len(tree.owners)} files, {report['host_programs']} host programs, "
        f"{report['target_libraries']} target DLLs checked, SHA-256 {digest}",
        flush=True,
    )
    shutil.rmtree(stage)
    return zip_path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], allow_abbrev=False)
    parser.add_argument("--host", action="append", choices=("arm64", "x86_64"))
    parser.add_argument("--cache", type=Path, default=Path("windows-toolchain-cache"))
    parser.add_argument("--output", type=Path, default=Path("dist"))
    parser.add_argument("--offline", action="store_true", help="use only cached downloads")
    parser.add_argument(
        "--download-only", action="store_true", help="fill the cache and verify it, then stop"
    )
    args = parser.parse_args()
    manifest = json.loads((HERE / "components.json").read_text(encoding="utf-8"))
    hosts = args.host or ["arm64", "x86_64"]
    if args.download_only:
        for component in manifest["components"]:
            for host in hosts:
                archive = component["archives"][host]
                fetch(archive["url"], archive["sha256"], args.cache, args.offline)
            for extra in component.get("files", []):
                fetch(extra["url"], extra["sha256"], args.cache, args.offline)
        return
    args.output.mkdir(parents=True, exist_ok=True)
    for host in hosts:
        print(f"assembling the {host} toolchain", flush=True)
        assemble(manifest, host, args.cache, args.output, args.offline)


if __name__ == "__main__":
    main()
