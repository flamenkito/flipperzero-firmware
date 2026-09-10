#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///

# ─── How to run ───
# Install uv if needed: curl -LsSf https://astral.sh/uv/install.sh | sh
# Direct: python3 airbridge/tests/hil/hil_e2e.py --help
# Isolated: uv run airbridge/tests/hil/hil_e2e.py --selftest
# ──────────────────
"""One-gate-at-a-time macOS HIL runner for USB identity QA.

The orchestrator fires ``question`` gates between one-step invocations. This
script never prompts, changes device state, polls, or waits for transitions.
"""

from __future__ import annotations

import argparse
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, replace
from datetime import UTC, datetime
from enum import StrEnum
import glob
import os
from pathlib import Path
import platform
import re
import select
import subprocess
import sys
import tempfile
import termios
import time
from typing import Final, final


ROOT: Final[Path] = Path(__file__).resolve().parents[3]
DEFAULT_EVIDENCE_DIR: Final[Path] = ROOT / ".omo/evidence/usb-identity-spoof"
FIXTURE: Final[Path] = ROOT / "airbridge/tests/fixtures/selftest_0bda_1100_macos15.txt"
CAPTURE_TOOL: Final[Path] = ROOT / "airbridge/tools/usb_descriptor_capture.py"
SAMPLER_TOOL: Final[Path] = ROOT / "airbridge/tools/qa_usb_supervised.py"
UNKNOWN: Final[str] = "<unavailable>"
# Tunables for ``read_cli_prompt``. The Flipper CLI banner and the first
# ``>:`` prompt arrive in separate CDC packets, so a single read
# truncates at the banner head. Accumulate until ``>:`` is seen, the
# idle gap closes, or the total deadline elapses.
CLI_PROMPT_TOTAL_DEADLINE_S: Final[float] = 10.0
CLI_PROMPT_IDLE_GAP_S: Final[float] = 1.0
CLI_PROMPT_POLL_TIMEOUT_S: Final[float] = 0.5


@dataclass(frozen=True, slots=True)
class Identity:
    vendor_id: int
    product_id: int
    manufacturer: str
    product: str

# Sources: targets/f7/furi_hal/furi_hal_usb_spoof.c:74-100 and
# applications_user/pocket_airbridge/airbridge_usb.c:33-39,63-68,299-376.
IDENTITIES: Final[dict[str, Identity]] = {
    "boot-logitech": Identity(0x046D, 0xC31C, "Logitech", "USB Keyboard"),
    "boot-dell": Identity(0x413C, 0x2113, "Dell", "KB216 Keyboard"),
    "fap-logitech": Identity(0x046D, 0xC31C, "Logitech", "USB Keyboard"),
    "fap-dell": Identity(0x413C, 0x2113, "Dell", "KB216 Keyboard"),
    "fap-hp": Identity(0x03F0, 0x5341, "PIXART", "HP Wireless Keyboard and Mouse"),
    "selftest": Identity(0x0BDA, 0x1100, "Realtek", "HID Device"),
}

class Step(StrEnum):
    E1_SPOOF = "e1-expect-spoof"
    E1_HIDDEN = "e1-expect-not-discoverable"
    E1_CDC = "e1-expect-cdc"
    E1_SAMPLE = "e1-sample-boot"
    E2_FAP = "e2-expect-fap-profile"
    E2_SPOOF = "e2-expect-spoof"

@dataclass(frozen=True, slots=True)
class AssertionRow:
    field: str
    expected: str
    actual: str
    passed: bool

@dataclass(frozen=True, slots=True)
class EvidenceRecord:
    step: str
    passed: bool
    rows: tuple[AssertionRow, ...]
    raw: str

@final
class CliArgs(argparse.Namespace):
    selftest: bool = False
    selftest_wrong_vid: bool = False
    step: str | None = None
    identity: str | None = None
    profile: str | None = None
    evidence: Path | None = None

def normalized_fields(capture: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    active = False
    for line in capture.splitlines():
        if line == "[normalized]":
            active = True
            continue
        if active and line.startswith("["):
            break
        if active and " = " in line:
            name, value = line.split(" = ", 1)
            fields[name] = value
    return fields

def identity_rows(fields: Mapping[str, str], identity: Identity) -> tuple[AssertionRow, ...]:
    required = (("device.VID", f"0x{identity.vendor_id:04X}"), ("device.PID", f"0x{identity.product_id:04X}"), ("device.iManufacturer", identity.manufacturer), ("device.iProduct", identity.product))
    rows = [AssertionRow(name, expected, fields.get(name, "<missing>"), fields.get(name) == expected) for name, expected in required]
    interface_classes = tuple(value for name, value in fields.items() if re.fullmatch(r"interface\.\d+\.class", name))
    rows.append(AssertionRow("interface.*.class", "contains 0x03", ", ".join(interface_classes) or "<missing>", "0x03" in interface_classes))
    count_text = fields.get("interfaces.count", "<missing>")
    second_present = count_text.isdecimal() and int(count_text) >= 2
    rows.append(AssertionRow("interfaces.count", ">= 2", count_text, second_present))
    serial = fields.get("device.iSerial")
    rows.append(AssertionRow("device.iSerial", "absent", serial or "<missing>", serial in {None, "", UNKNOWN, "<absent>"}))
    serial_index = fields.get("device.iSerial.index", "<missing>")
    rows.append(AssertionRow("device.iSerial.index", "0", serial_index, serial_index == "0"))
    report_values = tuple(value for name, value in fields.items() if name.endswith(".hid_report_descriptor"))
    rows.append(AssertionRow("interface.*.hid_report_descriptor", "optional; unavailable allowed", ", ".join(report_values) or "<not exposed>", True))
    return tuple(rows)

def render_table(rows: Sequence[AssertionRow]) -> str:
    headings = ("FIELD", "EXPECTED", "ACTUAL", "RESULT")
    values = [(row.field, row.expected, row.actual, "PASS" if row.passed else "FAIL") for row in rows]
    widths = tuple(max(len(headings[index]), *(len(row[index]) for row in values)) for index in range(4))
    lines = [" | ".join(headings[index].ljust(widths[index]) for index in range(4))]
    lines.append("-+-".join("-" * width for width in widths))
    lines.extend(" | ".join(row[index].ljust(widths[index]) for index in range(4)) for row in values)
    return "\n".join(lines)

def finish(evidence: Path, record: EvidenceRecord, exit_code: int) -> int:
    timestamp = datetime.now(UTC).isoformat()
    status = "PASS" if record.passed else "FAIL"
    table = render_table(record.rows)
    evidence.parent.mkdir(parents=True, exist_ok=True)
    with evidence.open("a", encoding="utf-8") as stream:
        _ = stream.write(f"\n=== {timestamp} {status} step={record.step} ===\n{table}\n--- raw capture ---\n{record.raw}\n")
    print(f"{timestamp} {status} {record.step} evidence={evidence}")
    if not record.passed:
        print(table)
    return exit_code

def run_capture_step(step: Step, identity: Identity, evidence: Path) -> int:
    with tempfile.TemporaryDirectory(prefix="airbridge-hil-") as directory:
        output = Path(directory) / "capture.txt"
        target = f"{identity.vendor_id:04X}:{identity.product_id:04X}"
        command = (sys.executable, str(CAPTURE_TOOL), target, "--output", str(output))
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90, check=False)
        raw = output.read_text(encoding="utf-8") if output.exists() else f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    if result.returncode != 0:
        row = AssertionRow("descriptor capture", f"{target} captured", f"exit {result.returncode}", False)
        return finish(evidence, EvidenceRecord(step, False, (row,), raw), 2)
    rows = identity_rows(normalized_fields(raw), identity)
    passed = all(row.passed for row in rows)
    return finish(evidence, EvidenceRecord(step, passed, rows, raw), 0 if passed else 2)


def fresh_profiler() -> str:
    command = ("system_profiler", "SPUSBDataType")
    result = subprocess.run(command, capture_output=True, text=True, timeout=45, check=False)
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
    return result.stdout


def read_cli_prompt(port: str) -> bytes:
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    with os.fdopen(fd, "rb", buffering=0) as serial:
        attributes = termios.tcgetattr(serial.fileno())
        attributes[4] = termios.B115200
        attributes[5] = termios.B115200
        termios.tcsetattr(serial.fileno(), termios.TCSANOW, attributes)
        buffer = bytearray()
        deadline = time.monotonic() + CLI_PROMPT_TOTAL_DEADLINE_S
        last_byte_at = time.monotonic()
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            poll = min(CLI_PROMPT_POLL_TIMEOUT_S, remaining)
            readable, _, _ = select.select((serial,), (), (), poll)
            if not readable:
                if buffer and time.monotonic() - last_byte_at >= CLI_PROMPT_IDLE_GAP_S:
                    break
                continue
            chunk = serial.read(4096)
            if not chunk:
                continue
            buffer.extend(chunk)
            last_byte_at = time.monotonic()
            if b">:" in buffer:
                break
        return bytes(buffer)


def run_visibility_step(evidence: Path, expect_cdc: bool) -> int:
    profiler = fresh_profiler()
    nodes = tuple(sorted(glob.glob("/dev/cu.usbmodem*")))
    vid_present = re.search(r"\b(?:0x)?0483\b", profiler, re.IGNORECASE) is not None
    expected = "present" if expect_cdc else "absent"
    rows = [AssertionRow("system_profiler 0483", expected, "present" if vid_present else "absent", vid_present == expect_cdc), AssertionRow("/dev/cu.usbmodem*", expected, ", ".join(nodes) or "absent", bool(nodes) == expect_cdc)]
    prompt = read_cli_prompt(nodes[0]) if expect_cdc and nodes else b""
    if expect_cdc:
        rows.append(AssertionRow("read-only CLI prompt", "contains >:", repr(prompt), b">:" in prompt))
    passed = all(row.passed for row in rows)
    raw = f"system_profiler SPUSBDataType:\n{profiler}\nserial nodes:\n" + "\n".join(nodes) + f"\nserial read:\n{prompt!r}"
    step = Step.E1_CDC if expect_cdc else Step.E1_HIDDEN
    return finish(evidence, EvidenceRecord(step, passed, tuple(rows), raw), 0 if passed else 2)


def run_sample_step(evidence: Path) -> int:
    with tempfile.TemporaryDirectory(prefix="airbridge-hil-") as directory:
        output = Path(directory) / "sample.log"
        command = (sys.executable, str(SAMPLER_TOOL), "sample", "--budget-s", "15", "--output", str(output))
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=False)
        sample = output.read_text(encoding="utf-8") if output.exists() else "<sampler produced no output file>"
    passed = result.returncode == 0
    expected_exit = result.returncode in {0, 2}
    actual = "clean (exit 0)" if passed else f"0483 sighting/failure (exit {result.returncode})"
    row = AssertionRow("supervised boot sample", "clean (exit 0)", actual, passed)
    raw = f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}\nsampler output:\n{sample}"
    return finish(evidence, EvidenceRecord(Step.E1_SAMPLE, passed, (row,), raw), result.returncode if expected_exit else 1)


def selftest_fields(wrong_vid: bool) -> tuple[str, tuple[AssertionRow, ...]]:
    raw = FIXTURE.read_text(encoding="utf-8")
    fields = normalized_fields(raw)
    fields.update({"interfaces.count": "2", "interface.00.class": "0x03", "interface.01.class": "0x03", "interface.00.hid_report_descriptor": UNKNOWN})
    identity = replace(IDENTITIES["selftest"], vendor_id=0xFFFF) if wrong_vid else IDENTITIES["selftest"]
    return raw, identity_rows(fields, identity)


def run_selftest_wrong_vid() -> int:
    _, rows = selftest_fields(wrong_vid=True)
    print(f"{datetime.now(UTC).isoformat()} EXPECTED FAIL selftest-wrong-vid\n{render_table(rows)}")
    return 2 if not all(row.passed for row in rows) else 1


def run_selftest() -> int:
    raw, rows = selftest_fields(wrong_vid=False)
    if not all(row.passed for row in rows):
        print(f"SELFTEST FAIL fixture assertion\n{render_table(rows)}\n{raw}")
        return 1
    child = subprocess.run((sys.executable, str(Path(__file__).resolve()), "--_selftest-wrong-vid"), capture_output=True, text=True, check=False)
    print(f"{datetime.now(UTC).isoformat()} PASS selftest fixture per-field and EXPECTED-ABSENT handling\n{render_table(rows)}\n--- raw fixture ---\n{raw}")
    print(child.stdout, end="")
    if child.stderr:
        print(child.stderr, file=sys.stderr, end="")
    if child.returncode != 2:
        print(f"SELFTEST FAIL wrong VID returned {child.returncode}, expected 2", file=sys.stderr)
        return 1
    print(f"{datetime.now(UTC).isoformat()} PASS deliberately wrong VID produced FAIL exit 2")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    _ = parser.add_argument("--selftest", action="store_true", help="run fixture-only assertion-engine tests")
    _ = parser.add_argument("--_selftest-wrong-vid", dest="selftest_wrong_vid", action="store_true", help=argparse.SUPPRESS)
    subparsers = parser.add_subparsers(dest="step")
    for step in (Step.E1_SPOOF, Step.E2_SPOOF):
        child = subparsers.add_parser(step, help="assert a boot spoof composite identity")
        _ = child.add_argument("--identity", choices=("logitech", "dell"), required=True)
        _ = child.add_argument("--evidence", type=Path)
    child = subparsers.add_parser(Step.E2_FAP, help="assert a FAP composite identity")
    _ = child.add_argument("--profile", choices=("logitech", "dell", "hp"), required=True)
    _ = child.add_argument("--evidence", type=Path)
    for step, help_text in ((Step.E1_HIDDEN, "assert no Flipper VID or CDC node"), (Step.E1_CDC, "assert maintenance CDC and read-only CLI prompt"), (Step.E1_SAMPLE, "run one bounded supervised boot sample")):
        child = subparsers.add_parser(step, help=help_text)
        _ = child.add_argument("--evidence", type=Path)
    return parser


def parse_cli(argv: Sequence[str]) -> CliArgs:
    parser = build_parser()
    args = CliArgs()
    try:
        _ = parser.parse_args(argv, namespace=args)
    except SystemExit as error:
        raise SystemExit(1 if error.code == 2 else error.code) from None
    if args.step is None and not args.selftest and not args.selftest_wrong_vid:
        parser.print_usage(sys.stderr)
        raise SystemExit(1)
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_cli(argv)
    if args.selftest:
        return run_selftest()
    if args.selftest_wrong_vid:
        return run_selftest_wrong_vid()
    if platform.system() != "Darwin":
        print("hil_e2e.py: live HIL steps support macOS only", file=sys.stderr)
        return 1
    step = Step(args.step)
    evidence = args.evidence or DEFAULT_EVIDENCE_DIR / f"task-8-hil-e2e-{step}.log"
    try:
        if args.identity is not None:
            return run_capture_step(step, IDENTITIES[f"boot-{args.identity}"], evidence)
        if args.profile is not None:
            return run_capture_step(step, IDENTITIES[f"fap-{args.profile}"], evidence)
        if step in {Step.E1_HIDDEN, Step.E1_CDC}:
            return run_visibility_step(evidence, expect_cdc=step is Step.E1_CDC)
        return run_sample_step(evidence)
    except (OSError, UnicodeError, subprocess.SubprocessError) as error:
        row = AssertionRow("step execution", "completed", repr(error), False)
        return finish(evidence, EvidenceRecord(step, False, (row,), repr(error)), 1)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, UnicodeError, subprocess.SubprocessError) as error:
        print(f"hil_e2e.py: internal error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
