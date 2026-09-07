#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "bleak>=2,<3",
# ]
# ///

# ─── How to run ───
# 1. Install uv (if not installed):
#      curl -LsSf https://astral.sh/uv/install.sh | sh
# 2. Run directly (no venv, no pip install needed):
#      uv run airbridge/tools/ble_storm.py --target 100
# 3. Or make executable and run:
#      chmod +x airbridge/tools/ble_storm.py && ./airbridge/tools/ble_storm.py
# ──────────────────
"""Stress AirBridge BLE connect and notification registration teardown.

The Flipper is a blind byte relay: browser crypto begins only when a peer writes
protocol frames. This harness writes no frames, so subscribing to TX is benign
while still exercising the firmware's subscribed-client and teardown paths.
"""

from __future__ import annotations

import argparse
import asyncio
import json
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Final

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.exc import BleakError

DEFAULT_NAME: Final = "HP 725 K+M"
SERVICE_UUID: Final = "7b871228-baf0-c5b4-5f46-9c2613d627a3"
TX_UUID: Final = "87825ec0-7398-8cb7-3242-b083eaa34f27"
SCAN_TIMEOUT_SECONDS: Final = 8.0
CONNECT_TIMEOUT_SECONDS: Final = 8.0
DISCONNECT_TIMEOUT_SECONDS: Final = 5.0
ATTEMPTS_PER_CYCLE: Final = 6
RETRY_SETTLE_SECONDS: Final = 0.75


@dataclass(frozen=True, slots=True)
class StormConfig:
    target: int
    name: str
    settle_seconds: float
    retry_settle_seconds: float = RETRY_SETTLE_SECONDS


class StormCycleError(RuntimeError):
    stage: str
    detail: str

    def __init__(self, stage: str, detail: str) -> None:
        self.stage = stage
        self.detail = detail
        super().__init__(f"{stage}: {detail}")


@dataclass(frozen=True, slots=True)
class StormResult:
    target: int
    successes: int
    failures: int
    attempts: int
    recovered_cycles: int
    last_error: str | None
    failure_cycle: int | None

    @property
    def passed(self) -> bool:
        return self.successes == self.target and self.failures == 0

    def render(self) -> str:
        return (
            f"target={self.target}, successes={self.successes}, "
            f"failures={self.failures}, attempts={self.attempts}, "
            f"recoveredCycles={self.recovered_cycles}, "
            f"lastError={json.dumps(self.last_error)}"
        )


CycleRunner = Callable[[StormConfig], Awaitable[None]]


async def bleak_cycle(config: StormConfig) -> None:
    """Scan, connect, subscribe, unsubscribe, and await clean disconnect."""
    try:
        device = await BleakScanner.find_device_by_name(
            config.name,
            timeout=SCAN_TIMEOUT_SECONDS,
            service_uuids=[SERVICE_UUID],
        )
        if device is None:
            raise StormCycleError(stage="scan", detail=f"{config.name!r} not found")

        disconnected = asyncio.Event()

        def on_disconnected(_: BleakClient) -> None:
            disconnected.set()

        def on_notification(_: BleakGATTCharacteristic, _data: bytearray) -> None:
            return None

        async with BleakClient(
            device,
            disconnected_callback=on_disconnected,
            timeout=CONNECT_TIMEOUT_SECONDS,
            services={SERVICE_UUID},
        ) as client:
            await client.start_notify(TX_UUID, on_notification)
            _ = await client.stop_notify(TX_UUID)

        _ = await asyncio.wait_for(
            disconnected.wait(),
            timeout=DISCONNECT_TIMEOUT_SECONDS,
        )
        await asyncio.sleep(config.settle_seconds)
    except StormCycleError:
        raise
    except (BleakError, OSError, TimeoutError) as error:
        raise StormCycleError(
            stage=type(error).__name__,
            detail=str(error),
        ) from error


async def execute_storm(config: StormConfig, run_cycle: CycleRunner) -> StormResult:
    """Run successful cycles or stop after one cycle exhausts its retry budget."""
    successes = 0
    attempts = 0
    recovered_cycles = 0
    last_error: str | None = None

    for cycle_number in range(1, config.target + 1):
        for retry_number in range(ATTEMPTS_PER_CYCLE):
            attempts += 1
            try:
                await run_cycle(config)
            except StormCycleError as error:
                last_error = str(error)
                await asyncio.sleep(config.retry_settle_seconds)
                continue

            successes += 1
            if retry_number > 0:
                recovered_cycles += 1
            last_error = None
            break
        else:
            return StormResult(
                config.target,
                successes,
                1,
                attempts,
                recovered_cycles,
                last_error,
                cycle_number,
            )

    return StormResult(
        config.target,
        successes,
        0,
        attempts,
        recovered_cycles,
        last_error,
        None,
    )


class StormArguments(argparse.Namespace):
    target: int = 100
    name: str = DEFAULT_NAME
    settle: float = 0.5


def parse_args() -> StormConfig:
    parser = argparse.ArgumentParser(description=__doc__)
    _ = parser.add_argument("--target", type=int)
    _ = parser.add_argument("--name")
    _ = parser.add_argument("--settle", type=float)
    args = StormArguments()
    _ = parser.parse_args(namespace=args)
    if args.target < 1:
        parser.error("--target must be at least 1")
    if args.settle < 0:
        parser.error("--settle must be non-negative")
    return StormConfig(target=args.target, name=args.name, settle_seconds=args.settle)


def main() -> int:
    """Run the BLE storm and return nonzero on the first hard cycle failure."""
    result = asyncio.run(execute_storm(parse_args(), bleak_cycle))
    print(result.render())
    if result.failure_cycle is not None:
        print(f"failureCycle={result.failure_cycle}")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
