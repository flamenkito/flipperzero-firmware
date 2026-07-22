#!/usr/bin/env python3
from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path
import sys
from typing import Final

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.usb_descriptor_fixture import FixtureError, load_normalized

MISSING: Final[str] = "<missing>"


@dataclass(frozen=True)
class Difference:
    field: str
    genuine: str
    flipper: str


def compare(genuine: Iterable[tuple[str, str]], flipper: Iterable[tuple[str, str]]) -> tuple[Difference, ...]:
    genuine_fields = dict(genuine)
    flipper_fields = dict(flipper)
    differences: list[Difference] = []
    for field in sorted(set(genuine_fields) | set(flipper_fields)):
        genuine_value = genuine_fields.get(field, MISSING)
        flipper_value = flipper_fields.get(field, MISSING)
        if genuine_value != flipper_value:
            differences.append(Difference(field, genuine_value, flipper_value))
    return tuple(differences)


def table_rows(genuine: Iterable[tuple[str, str]], flipper: Iterable[tuple[str, str]]) -> tuple[tuple[str, str, str, str], ...]:
    genuine_fields = dict(genuine)
    flipper_fields = dict(flipper)
    rows: list[tuple[str, str, str, str]] = []
    for field in sorted(set(genuine_fields) | set(flipper_fields)):
        genuine_value = genuine_fields.get(field, MISSING)
        flipper_value = flipper_fields.get(field, MISSING)
        status = "MATCH" if genuine_value == flipper_value else "DIFF"
        rows.append((field, genuine_value, flipper_value, status))
    return tuple(rows)


def render_table(rows: Iterable[tuple[str, str, str, str]]) -> str:
    header = ("FIELD", "GENUINE", "FLIPPER", "MATCH/DIFF")
    materialized = (header, *tuple(rows))
    widths = tuple(max(len(row[index]) for row in materialized) for index in range(len(header)))
    def format_row(row: tuple[str, str, str, str]) -> str:
        return " | ".join(value.ljust(widths[index]) for index, value in enumerate(row))
    divider = "-+-".join("-" * width for width in widths)
    return "\n".join((format_row(header), divider, *(format_row(row) for row in materialized[1:])))


def meaning(field: str) -> str:
    exact = {
        "device.VID": "Vendor ID is the USB identity assigned by USB-IF; a mismatch immediately names another vendor.",
        "device.PID": "Product ID selects the vendor's product identity; a mismatch immediately names another product.",
        "device.bcdDevice": "Device release number is exposed during enumeration and can identify a firmware or hardware revision.",
        "device.bcdUSB": "USB specification revision changes the device descriptor and host compatibility behavior.",
        "device.bMaxPacketSize0": "Endpoint zero packet size is a device-descriptor field used during control transfers.",
        "device.bNumConfigurations": "Configuration count changes the descriptor tree a host can enumerate.",
        "interfaces.count": "The number of interfaces reveals a different composite-device shape.",
    }
    if field in exact:
        return exact[field]
    if field.startswith("device.i"):
        return "USB string descriptor or index differs; strings and descriptor-index layout are directly inspectable."
    if field in {"device.class", "device.subclass", "device.protocol"}:
        return "Device class, subclass, or protocol changes the device-level USB classification."
    if field.endswith(".hid_report_descriptor"):
        return "HID report descriptors define every report collection and usage; a byte mismatch is directly inspectable."
    if field.endswith(".class") or field.endswith(".subclass") or field.endswith(".protocol"):
        return "Interface class, subclass, or protocol changes the host-visible function of this interface."
    if field.endswith(".endpoint_count"):
        return "Endpoint count changes the interface's transfer topology."
    if field.endswith(".address") or field.endswith(".direction"):
        return "Endpoint address or direction changes the host-visible data-path layout."
    if field.endswith(".interval"):
        return "Endpoint polling interval changes transfer scheduling and the endpoint descriptor bytes."
    if field.endswith(".max_packet_size"):
        return "Endpoint maximum packet size changes transfer capacity and the endpoint descriptor bytes."
    return "This normalized descriptor field is inspectable in the captured USB descriptor evidence."


@dataclass(frozen=True)
class DiffArguments:
    genuine: Path
    flipper: Path
    explain: bool


def parse_arguments(argv: Sequence[str]) -> DiffArguments:
    if not argv or argv[0] in {"-h", "--help"}:
        print("Usage: usb_descriptor_diff.py GENUINE_FIXTURE FLIPPER_FIXTURE [--explain]")
        raise SystemExit(0)
    if len(argv) < 2:
        raise FixtureError("expected genuine and Flipper fixture paths")
    extras = tuple(argv[2:])
    if any(extra != "--explain" for extra in extras):
        raise FixtureError("only --explain is supported after the two fixture paths")
    return DiffArguments(Path(argv[0]), Path(argv[1]), "--explain" in extras)


def main(argv: Sequence[str]) -> int:
    args = parse_arguments(argv)
    genuine = load_normalized(args.genuine)
    flipper = load_normalized(args.flipper)
    rows = table_rows(genuine, flipper)
    differences = compare(genuine, flipper)
    print(render_table(rows))
    if args.explain and differences:
        print("\nDIFF EXPLANATIONS:")
        for difference in differences:
            print(f"- {difference.field}: {meaning(difference.field)}")
    print(f"\n{len(differences)} differing field(s).")
    return 1 if differences else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except FixtureError as error:
        print(f"usb_descriptor_diff.py: {error}", file=sys.stderr)
        raise SystemExit(2)
