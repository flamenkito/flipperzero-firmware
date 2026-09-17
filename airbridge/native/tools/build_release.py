#!/usr/bin/env python3
"""Build and package abt with repository paths removed from executable diagnostics."""

import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tomllib
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("macos", "windows", "all"))
    args = parser.parse_args()
    native = Path(__file__).resolve().parents[1]
    root = native.parent.parent
    version = tomllib.loads((native / "Cargo.toml").read_text())["package"]["version"]
    env = os.environ.copy()
    flags = env.get("CARGO_ENCODED_RUSTFLAGS", "").split("\x1f") if env.get("CARGO_ENCODED_RUSTFLAGS") else shlex.split(env.get("RUSTFLAGS", ""))
    flags.append(f"--remap-path-prefix={root}=/workspace/airbridge")
    env["CARGO_ENCODED_RUSTFLAGS"] = "\x1f".join(flags)
    env.pop("RUSTFLAGS", None)
    platforms = ("macos", "windows") if args.platform == "all" else (args.platform,)
    for platform in platforms:
        command = ["cargo", "build", "--release", "--locked", "--manifest-path", str(native / "Cargo.toml")]
        if platform == "windows":
            env.setdefault("CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER", "x86_64-w64-mingw32-gcc")
            command.extend(["--target", "x86_64-pc-windows-gnu"])
            binary = native / "target/x86_64-pc-windows-gnu/release/abt.exe"
            package_name = "abt-windows-x64"
        else:
            binary = native / "target/release/abt"
            package_name = "abt-macos"
        subprocess.run(command, env=env, check=True)
        package = native / "target/dist" / package_name
        package.mkdir(parents=True, exist_ok=True)
        files = [binary, native / "README.md", root / "LICENSE", native / "tools/bench.py"]
        names = []
        for source in files:
            shutil.copy2(source, package / source.name)
            names.append(source.name)
        (package / "README.txt").write_text(
            f"abt {version} — native AirBridge TCP tunnel\n\n"
            "See README.md for setup, SSH, WSL and troubleshooting.\n"
            "Keep the AirBridge device on its Bridge screen.\n"
            "Windows USB: abt.exe usb --connect 127.0.0.1:2222\n"
            "Mac BLE: abt ble --listen 127.0.0.1:2222 --scan-seconds 20\n"
            "Start the target SSH/API server separately. Close competing browser clients.\n",
            encoding="utf-8",
        )
        names.append("README.txt")
        sums = "".join(f"{hashlib.sha256((package / name).read_bytes()).hexdigest()}  {name}\n" for name in names)
        (package / "SHA256SUMS").write_text(sums, encoding="ascii")
        archive = package.with_suffix(".zip")
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as output:
            for name in [*names, "SHA256SUMS"]:
                output.write(package / name, name)
        print(f"Built {binary}\nPackaged {archive}", flush=True)


if __name__ == "__main__":
    main()
