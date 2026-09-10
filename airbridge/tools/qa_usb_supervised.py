#!/usr/bin/env python3
"""Bounded macOS USB QA supervisor for Pocket AirBridge.

``sample`` bounds each class-filtered ``ioreg`` probe and its overall
monotonic sampling window. ioreg is used in place of ``system_profiler
SPUSBDataType`` because system_profiler serves a stale USB-tree cache
under rapid repeated invocation, which makes it unreliable for
disappearance/reappearance detection. ``vendor-stress`` performs every
hidapi write in a child process that the parent terminates and reaps at
its deadline, so a blocked native ``hid_write`` cannot hang the QA row.
Neither in-process cancellation nor hidapi read nonblocking mode is used
as a bound.
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
from dataclasses import dataclass
import math, multiprocessing, platform, re, subprocess, sys, time
from pathlib import Path
from typing import Final, Literal, Never, Protocol, TypedDict, override, runtime_checkable

TOOL_VERSION: Final[str] = "1.0"
PROFILER_TIMEOUT_S: Final[float] = 5.0
__all__ = (
    "TOOL_VERSION",
    "PROFILER_TIMEOUT_S",
    "IOREG_DEVICE_COMMAND",
    "STM32_VENDOR_LINE",
    "subprocess",
    "time",
    "write_log",
    "flipper_contexts",
    "sample_record",
    "run_sample",
    "run_vendor_stress",
    "build_parser",
)
# Class-filtered to IOUSBHostDevice so each invocation stays small/fast
# and reads the live IORegistry (system_profiler serves a stale cache
# under rapid repeated invocation).
IOREG_DEVICE_COMMAND: Final[tuple[str, ...]] = ("ioreg", "-p", "IOUSB", "-l", "-w", "0", "-c", "IOUSBHostDevice")
# 0x0483 = 1155 decimal. ioreg emits ``"idVendor" = 1155`` with optional
# tree-indent pipe prefixes, so the pattern tolerates both.
STM32_VENDOR_LINE: Final[re.Pattern[str]] = re.compile(
    r'^\s*\|*\s*"idVendor"\s*=\s*1155\b'
)


class QaArgumentParser(argparse.ArgumentParser):
    @override
    def error(self, message: str) -> Never:
        self.print_usage(sys.stderr); self.exit(1, f"{self.prog}: error: {message}\n")


@dataclass(frozen=True, slots=True)
class VendorStressConfig:
    vid: int; pid: int; usage_page: int; interface: int
    count: int; report_len: int; deadline_s: float
    output: Path | None


type Mode = Literal["sample", "vendor-stress"]; type EventKind = Literal["open", "open_error", "write_success", "write_exception", "done"]
type ChildEvent = tuple[EventKind, str]


class Arguments(argparse.Namespace):
    mode: Mode = "sample"; budget_s: float = 15.0; output: Path | None = None
    vid: int = 0; pid: int = 0; usage_page: int = 0xFF00; interface: int = 0
    count: int = 100; report_len: int = 64; deadline_s: float = 30.0


class HidInfo(TypedDict):
    path: bytes; usage_page: int; interface_number: int


class HidDevice(Protocol):
    def open_path(self, path: bytes) -> None: ...
    def write(self, report: bytes) -> int: ...
    def close(self) -> None: ...


@runtime_checkable
class HidModule(Protocol):
    def enumerate(self, vid: int, pid: int) -> list[HidInfo]: ...
    def device(self) -> HidDevice: ...


class EventChannel(Protocol):
    def send(self, obj: ChildEvent) -> None: ...
    def poll(self) -> bool: ...
    def recv(self) -> ChildEvent: ...
    def close(self) -> None: ...


def require_hid() -> HidModule:
    import hid
    if not isinstance(hid, HidModule): raise ImportError("hidapi module lacks enumerate/device API")
    return hid


def positive_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0: raise argparse.ArgumentTypeError("must be finite and greater than zero")
    return parsed


def positive_int(value: str) -> int:
    parsed = int(value, 0)
    if parsed <= 0: raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def usb_integer(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as error: raise argparse.ArgumentTypeError("must be decimal or 0x-prefixed") from error
    if not 0 <= parsed <= 0xFFFF: raise argparse.ArgumentTypeError("must be between 0 and 0xFFFF")
    return parsed


def write_log(lines: Sequence[str], output: Path | None) -> None:
    text = "\n".join(lines) + "\n"
    if output is None:
        print(text, end=""); return
    output.parent.mkdir(parents=True, exist_ok=True)
    _ = output.write_text(text, encoding="utf-8")
    print(f"Wrote {output}")


def flipper_contexts(raw: str) -> tuple[str, ...]:
    lines = raw.splitlines()
    return tuple(
        "\n".join(lines[max(0, index - 2):min(len(lines), index + 4)])
        for index, line in enumerate(lines) if STM32_VENDOR_LINE.search(line) is not None
    )


def sample_record(number: int, started: float, ended: float, status: str, verdict: str) -> tuple[str, ...]:
    return (f"sample.{number}.start_monotonic={started:.9f}", f"sample.{number}.end_monotonic={ended:.9f}",
            f"sample.{number}.status={status}", f"sample.{number}.0483_verdict={verdict}")


def run_sample(budget_s: float, output: Path | None) -> int:
    if platform.system() != "Darwin":
        print("qa_usb_supervised.py sample supports macOS only", file=sys.stderr)
        return 1
    started = time.monotonic()
    deadline = started + budget_s
    completed_count = missed_count = 0
    sighting = False
    records: list[str] = []
    while (remaining := deadline - time.monotonic()) > 0:
        number = completed_count + missed_count + 1
        sample_started = time.monotonic()
        try:
            result = subprocess.run(IOREG_DEVICE_COMMAND, capture_output=True, check=False, text=True,
                                    timeout=min(PROFILER_TIMEOUT_S, remaining))
        except subprocess.TimeoutExpired:
            missed_count += 1
            records.extend(sample_record(number, sample_started, time.monotonic(),
                                         "MISSED_TIMEOUT", "UNKNOWN_MISSED"))
            continue
        if result.returncode != 0:
            detail = result.stderr.strip() or "no stderr output"
            print(f"qa_usb_supervised.py: ioreg exit {result.returncode}: {detail}", file=sys.stderr)
            return 1
        completed_count += 1
        contexts = flipper_contexts(result.stdout)
        sample_sighting = bool(contexts)
        sighting |= sample_sighting
        verdict = "SIGHTING" if sample_sighting else "CLEAN"
        records.extend(sample_record(number, sample_started, time.monotonic(), "COMPLETED", verdict))
        records.extend(
            f"sample.{number}.0483_context.{ordinal}=\n{context}"
            for ordinal, context in enumerate(contexts, start=1)
        )
    ended = time.monotonic()
    write_log([
        "Pocket AirBridge USB QA supervisor", f"tool_version={TOOL_VERSION}",
        "mode=sample", "scope=macOS IOUSB plane IOUSBHostDevice nodes via ioreg",
        "notice=sampled evidence, not a formal non-enumeration proof",
        f"parameters.budget_s={budget_s}",
        f"parameters.per_invocation_timeout_s={PROFILER_TIMEOUT_S}",
        f"start_monotonic={started:.9f}", *records, f"end_monotonic={ended:.9f}",
        f"completed_sample_count={completed_count}", f"missed_sample_count={missed_count}",
        f"overall_0483_verdict={'SIGHTING' if sighting else 'CLEAN'}",
    ], output)
    return 2 if sighting else 0


def vendor_child(config: VendorStressConfig, events: EventChannel) -> None:  # noqa: BROAD_EXCEPT_OK
    try:
        hid = require_hid()
        matches = [
            item for item in hid.enumerate(config.vid, config.pid)
            if item.get("usage_page") == config.usage_page
            and item.get("interface_number") == config.interface
        ]
        if len(matches) != 1:
            events.send(("open_error", f"exact interface matched {len(matches)} devices"))
            return
        path = matches[0]["path"]
        device = hid.device()
        device.open_path(path)
        detail = (f"vid=0x{config.vid:04X} pid=0x{config.pid:04X} "
                  f"usage_page=0x{config.usage_page:04X} interface={config.interface} path={path!r}")
        events.send(("open", detail))
        try:
            for write_number in range(config.count):
                report = bytes([0]) + bytes([write_number & 0xFF]) * (config.report_len - 1)
                try:
                    _ = device.write(report)
                    events.send(("write_success", ""))
                except Exception as error:  # noqa: BROAD_EXCEPT_OK - hidapi exception classes vary.
                    events.send(("write_exception", f"{type(error).__name__}: {error}"))
        finally:
            device.close()
        events.send(("done", ""))
    except Exception as error:  # noqa: BROAD_EXCEPT_OK - native child boundary reports failures.
        events.send(("open_error", f"{type(error).__name__}: {error}"))
    finally:
        events.close()


def collect_events(events: EventChannel) -> tuple[str, int, int, str, bool]:
    open_result, first_exception = "NOT_REPORTED", "NONE"
    success_count = exception_count = 0
    completed = False
    while events.poll():
        try:
            event, detail = events.recv()
        except EOFError:
            break
        match event:  # noqa: MATCH_OK - EventKind cases are exhaustive; wildcard logs corrupted IPC.
            case "open": open_result = f"MATCHED {detail}"
            case "open_error": open_result = f"ERROR {detail}"
            case "write_success": success_count += 1
            case "write_exception":
                exception_count += 1
                if first_exception == "NONE": first_exception = detail
            case "done": completed = True
    return open_result, success_count, exception_count, first_exception, completed


def run_vendor_stress(config: VendorStressConfig) -> int:
    try:
        _ = require_hid()
    except (ImportError, ModuleNotFoundError):
        print("qa_usb_supervised.py: hidapi is required; pip install hidapi", file=sys.stderr)
        return 1
    context = multiprocessing.get_context("spawn")
    parent_events, child_events = context.Pipe(duplex=False)
    child = context.Process(target=vendor_child, args=(config, child_events))
    started = time.monotonic()
    child.start()
    child_events.close()
    child.join(timeout=max(0.0, config.deadline_s - (time.monotonic() - started)))
    deadline_hit = child.is_alive()
    if deadline_hit:
        child.terminate()
        child.join()
    ended = time.monotonic()
    open_result, successes, exceptions, first_exception, completed = collect_events(parent_events)
    parent_events.close()
    reaped, exitcode = not child.is_alive(), child.exitcode
    child.close()
    write_log([
        "Pocket AirBridge USB QA supervisor", f"tool_version={TOOL_VERSION}",
        "mode=vendor-stress", "scope=hidapi exact VID/PID + usage_page + interface match",
        f"parameters.vid=0x{config.vid:04X}", f"parameters.pid=0x{config.pid:04X}",
        f"parameters.usage_page=0x{config.usage_page:04X}",
        f"parameters.interface={config.interface}", f"parameters.count={config.count}",
        f"parameters.report_len={config.report_len}", f"parameters.deadline_s={config.deadline_s}",
        f"start_monotonic={started:.9f}", f"end_monotonic={ended:.9f}",
        f"open_result={open_result}", f"write_success_count={successes}",
        f"write_exception_count={exceptions}", f"first_write_exception={first_exception}",
        f"deadline_hit={str(deadline_hit).lower()}", f"child_reaped={str(reaped).lower()}",
        f"child_completed={str(completed).lower()}", f"child_exitcode={exitcode}",
    ], config.output)
    if deadline_hit:
        return 3
    return 0 if completed and open_result.startswith("MATCHED") and exitcode == 0 else 1


def build_parser() -> QaArgumentParser:
    parser = QaArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    sample = modes.add_parser("sample", help="sample macOS USB enumeration")
    _ = sample.add_argument("--budget-s", type=positive_float, default=15.0)
    _ = sample.add_argument("--output", type=Path)
    vendor = modes.add_parser("vendor-stress", help="run bounded vendor-OUT writes")
    _ = vendor.add_argument("--vid", type=usb_integer, required=True)
    _ = vendor.add_argument("--pid", type=usb_integer, required=True)
    _ = vendor.add_argument("--usage-page", type=usb_integer, default=0xFF00)
    _ = vendor.add_argument("--interface", type=usb_integer, required=True)
    _ = vendor.add_argument("--count", type=positive_int, default=100)
    _ = vendor.add_argument("--report-len", type=positive_int, default=64)
    _ = vendor.add_argument("--deadline-s", type=positive_float, default=30.0)
    _ = vendor.add_argument("--output", type=Path)
    return parser


def main(argv: Sequence[str]) -> int:
    args = build_parser().parse_args(argv, namespace=Arguments())
    match args.mode:  # noqa: MATCH_OK - Mode cases are exhaustive; wildcard rejects parser corruption.
        case "sample": return run_sample(args.budget_s, args.output)
        case "vendor-stress":
            config = VendorStressConfig(
                args.vid, args.pid, args.usage_page, args.interface, args.count,
                args.report_len, args.deadline_s, args.output,
            )
            return run_vendor_stress(config)


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, subprocess.SubprocessError) as error:
        print(f"qa_usb_supervised.py: {error}", file=sys.stderr)
        raise SystemExit(1)
