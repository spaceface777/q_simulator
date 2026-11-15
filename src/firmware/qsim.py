#!/usr/bin/env python3
"""Assemble a world from a simulator object and one firmware object per active module."""

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import tomllib
import zipfile


def images(layout: Path, contents: bytes) -> list[Path]:
    data = tomllib.loads(contents.decode("utf-8"))
    modules = data.get("module", {})
    if not isinstance(modules, dict):
        raise ValueError("module must be a table")
    module_ids = {str(i) for i in range(1, 25)} | {"255"}
    result = []
    for key, module in sorted(modules.items(), key=lambda entry: int(entry[0])):
        if key not in module_ids:
            raise ValueError(f"invalid module ID {key!r}")
        if not isinstance(module, dict) or not isinstance(module.get("active"), bool):
            raise ValueError(f"module {key} must set active to true or false")
        if module["active"]:
            name = module.get("firmware", data.get("firmware"))
            if not isinstance(name, str) or not name:
                raise ValueError(f"module {key} requires a firmware object")
            image = (layout.parent / name).resolve()
            if image.suffix != ".o" or not image.is_file():
                raise ValueError(f"module {key}: firmware object not found: {image}")
            result.append(image)
    if not result:
        raise ValueError("layout has no active modules")
    return result


def embed_sdl3(world: Path, toolchain: Path, system: str, arch: str) -> None:
    names = {
        ("Darwin", "aarch64"): "libSDL3.dylib",
        ("Linux", "aarch64"): "libSDL3.so",
        ("Linux", "x86_64"): "libSDL3.so",
        ("Windows", "x86_64"): "SDL3.dll",
    }
    name = names.get((system, arch))
    if name is None:
        return
    configured = "@QSIM_SDL3_ASSETS@"
    # The toolchain keeps native libraries for each platform in native/<os>-<cpu>.
    platform = {"Darwin": "macos", "Linux": "linux", "Windows": "windows"}[system] + "-" + \
        {"aarch64": "arm64", "x86_64": "amd64"}[arch]
    assets = Path(configured) if configured else toolchain / "native" / platform
    if not configured and not assets.exists():
        return
    files = (name, "dlopen-helper") if system == "Linux" else (name,)
    with zipfile.ZipFile(world, "a", allowZip64=False) as archive:
        for filename in files:
            contents = (assets / filename).read_bytes()
            digest = (assets / (filename + ".sha256")).read_text(encoding="ascii").strip()
            if not contents or hashlib.sha256(contents).hexdigest() != digest:
                raise ValueError(f"SDL3 asset checksum mismatch: {assets / filename}")
            archive.writestr(zipfile.ZipInfo(f"sdl3/{filename}"), contents, zipfile.ZIP_DEFLATED)
            archive.writestr(zipfile.ZipInfo(f"sdl3/{filename}.sha256"), digest)


def main() -> None:
    parser = argparse.ArgumentParser(prog="qsim")
    command = parser.add_subparsers(dest="command", required=True)
    build = command.add_parser("build")
    build.add_argument("layout", type=Path)
    build.add_argument("-o", "--output", type=Path, default=Path("world.exe"))
    args = parser.parse_args()
    try:
        layout = args.layout.resolve()
        contents = layout.read_bytes()
        firmware = images(layout, contents)
        output = args.output.resolve()
        # A world is always a *.exe file, whatever it contains, so that it starts on every platform.
        if output.suffix != ".exe":
            output = output.with_name(output.name + ".exe")
        # A Cosmopolitan world keeps its symbols in a separate debug file.
        cosmopolitan = "@QUERCUS_COSMOPOLITAN@" == "ON"
        debug = output.with_name(output.name + ".dbg")
        toolchain = Path(os.getenv("COSMOCC_TOOLCHAIN_PATH", "@COSMOCC_TOOLCHAIN_PATH@"))
        tools = toolchain / "bin"
        arch = "@CMAKE_SYSTEM_PROCESSOR@"
        simulator = Path(__file__).resolve().with_name("qsim.o")
        # cosmocc's compiler driver is a shell script, and its binary tools are APE executables,
        # which start through a shell outside Windows and by their .exe name on Windows.
        compile = [Path("@world_cxx@")]
        objcopy = [Path("@world_objcopy@")]
        if cosmopolitan and os.name == "nt":
            compile.insert(0, tools / "bash.exe")
            objcopy = [objcopy[0].with_name(objcopy[0].name + ".exe")]
        elif cosmopolitan:
            compile.insert(0, "sh")
            objcopy.insert(0, "sh")
        if output in [layout, simulator, *firmware] or debug in [layout, simulator, *firmware]:
            raise ValueError("output would overwrite an input")

        with tempfile.TemporaryDirectory(dir=output.parent) as directory:
            scratch = Path(directory)
            (scratch / "layout.toml").write_bytes(contents)
            digests = {}
            for image in firmware:
                if image not in digests:
                    with image.open("rb") as stream:
                        digests[image] = hashlib.file_digest(stream, "sha256").digest()
            (scratch / "firmware_digests").write_bytes(b"".join(digests[image] for image in firmware))
            for name in ("layout.toml", "firmware_digests"):
                subprocess.run([
                    *objcopy, "-I", "binary",
                    "-O", {"aarch64": "elf64-littleaarch64", "x86_64": "elf64-x86-64"}[arch],
                    "-B", {"aarch64": "aarch64", "x86_64": "i386:x86-64"}[arch],
                    "--rename-section", ".data=.rodata,alloc,load,readonly,data,contents",
                    name, name + ".o",
                ], cwd=scratch, check=True)
            # Objects from different toolchains have different ABIs, so they must not share a world.
            for image in [simulator, *dict.fromkeys(firmware)]:
                subprocess.run([*objcopy, "-O", "binary", "--only-section=.qsim_toolchain", image, "marker"],
                               cwd=scratch, check=True)
                if (marker := (scratch / "marker").read_bytes().rstrip(b"\0").decode()) != "@world_toolchain@":
                    raise ValueError(f"{image} is from the {marker or 'unknown'} toolchain, not @world_toolchain@")
            world = scratch / "world.exe"
            # Firmware objects are not position independent.
            options = ["-mtiny"] if cosmopolitan else ["-no-pie", "-ldl"]
            subprocess.run([
                *compile, *options, "-o", world, simulator,
                scratch / "layout.toml.o", scratch / "firmware_digests.o", *firmware, "-pthread", "-lgcov",
            ], check=True)
            if cosmopolitan:
                symbols = scratch / "world.exe.dbg"
                if "@CMAKE_HOST_SYSTEM_NAME@" == "Darwin" and arch == "aarch64":
                    subprocess.run([
                        "sh", tools / "apelink", "-l", tools / "ape-aarch64.elf", "-M",
                        tools / "ape-m1.c", "-o", world, symbols,
                    ], check=True)
                embed_sdl3(world, toolchain, "@CMAKE_HOST_SYSTEM_NAME@", arch)
                os.replace(symbols, debug)
            os.replace(world, output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"qsim: {error}\n")


if __name__ == "__main__":
    main()
