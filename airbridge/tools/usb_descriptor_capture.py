#!/usr/bin/env python3
from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
import platform
import re
import subprocess
import sys
from typing import Final

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.usb_descriptor_fixture import CaptureError, Target, render_fixture
from tools.usb_descriptor_macos import capture, parse_integer


DESCRIPTION: Final[str] = "Capture macOS USB descriptor evidence into a stable comparison fixture."


@dataclass(frozen=True)
class CaptureArguments:
    vid_pid: str
    output: Path
    location_id: str | None


def parse_target(value: str, location: str | None) -> Target:
    match = re.fullmatch(r"(?:0x)?([0-9A-Fa-f]{1,4}):(?:0x)?([0-9A-Fa-f]{1,4})", value)
    if match is None:
        raise CaptureError(f"VID:PID must look like 03f0:5341, got {value!r}")
    location_id = parse_integer(location) if location is not None else None
    if location is not None and location_id is None:
        raise CaptureError(f"--location-id must be a hexadecimal or decimal integer, got {location!r}")
    return Target(int(match.group(1), 16), int(match.group(2), 16), location_id)


def usage() -> str:
    return "Usage: usb_descriptor_capture.py VID:PID --output FIXTURE [--location-id LOCATION_ID]"


def parse_arguments(argv: Sequence[str]) -> CaptureArguments:
    if not argv or argv[0] in {"-h", "--help"}:
        print(f"{DESCRIPTION}\n{usage()}")
        raise SystemExit(0)
    vid_pid = argv[0]
    options: dict[str, str] = {}
    cursor = 1
    while cursor < len(argv):
        option = argv[cursor]
        if cursor + 1 >= len(argv):
            raise CaptureError(f"{option} requires a value\n{usage()}")
        value = argv[cursor + 1]
        if option not in {"--output", "--location-id"}:
            raise CaptureError(f"unknown option {option!r}\n{usage()}")
        if option in options:
            raise CaptureError(f"duplicate option {option!r}\n{usage()}")
        options[option] = value
        cursor += 2
    output = options.get("--output")
    if output is None:
        raise CaptureError(f"--output is required\n{usage()}")
    return CaptureArguments(vid_pid, Path(output), options.get("--location-id"))


def main(argv: Sequence[str]) -> int:
    if platform.system() != "Darwin":
        raise CaptureError("usb_descriptor_capture.py supports macOS only; see tests/fixtures/README.md for Windows")
    args = parse_arguments(argv)
    data = capture(parse_target(args.vid_pid, args.location_id))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    _ = args.output.write_text(render_fixture(data), encoding="utf-8")
    print(f"Wrote {args.output} ({len(data.normalized)} normalized fields for {data.target.label})")
    return 0


if __name__ == "__main__":
    try:
        exit_code = main(sys.argv[1:])
        raise SystemExit(exit_code)
    except (CaptureError, OSError, subprocess.SubprocessError) as error:
        print(f"usb_descriptor_capture.py: {error}", file=sys.stderr)
        raise SystemExit(2)
