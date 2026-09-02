from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
import platform
import shlex
from typing import Final


TOOL_VERSION: Final[str] = "1.0.0"
UNKNOWN: Final[str] = "<unavailable>"


class CaptureError(RuntimeError):
    pass


class FixtureError(RuntimeError):
    pass


@dataclass(frozen=True)
class Target:
    vendor_id: int
    product_id: int
    location_id: int | None

    @property
    def label(self) -> str:
        return f"0x{self.vendor_id:04X}:0x{self.product_id:04X}"


@dataclass(frozen=True)
class CaptureData:
    target: Target
    profiler_command: tuple[str, ...]
    profiler_raw: str
    profiler_target: str
    ioreg_command: tuple[str, ...]
    ioreg_raw: str
    ioreg_target: str
    normalized: tuple[tuple[str, str], ...]


def render_fixture(data: CaptureData) -> str:
    timestamp = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
    os_version = f"{platform.mac_ver()[0]} ({platform.platform()})"
    lines = (
        "# USB descriptor fixture format = 1",
        f"# captured_at_utc = {timestamp}",
        f"# machine = {platform.node()}",
        f"# os_version = {os_version}",
        f"# tool = usb_descriptor_capture.py {TOOL_VERSION}",
        f"# target = {data.target.label}",
        f"# command.system_profiler = {shlex.join(data.profiler_command)}",
        f"# command.ioreg = {shlex.join(data.ioreg_command)}",
        "[normalized]",
        *(f"{key} = {value}" for key, value in data.normalized),
        "[raw.system_profiler.target]",
        data.profiler_target,
        "[raw.ioreg.target]",
        data.ioreg_target,
        "[raw.system_profiler.full]",
        data.profiler_raw.rstrip(),
        "[raw.ioreg.full]",
        data.ioreg_raw.rstrip(),
        "",
    )
    return "\n".join(lines)


def load_normalized(path: Path) -> tuple[tuple[str, str], ...]:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise FixtureError(f"cannot read {path}: {error}") from error
    inside_normalized = False
    fields: dict[str, str] = {}
    for line_number, line in enumerate(lines, start=1):
        if line == "[normalized]":
            if inside_normalized:
                raise FixtureError(f"{path}:{line_number}: duplicate [normalized] section")
            inside_normalized = True
            continue
        if inside_normalized and line.startswith("[") and line.endswith("]"):
            break
        if not inside_normalized or not line.strip():
            continue
        key, separator, value = line.partition(" = ")
        if not separator or not key or not value:
            raise FixtureError(f"{path}:{line_number}: expected `field = value` in [normalized]")
        if key in fields:
            raise FixtureError(f"{path}:{line_number}: duplicate normalized field {key!r}")
        fields[key] = value
    if not inside_normalized:
        raise FixtureError(f"{path}: missing [normalized] section")
    if not fields:
        raise FixtureError(f"{path}: [normalized] section contains no fields")
    return tuple(sorted(fields.items()))
