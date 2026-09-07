from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable

from airbridge.tools.ble_storm import StormConfig, StormCycleError, execute_storm


def test_execute_storm_reports_exact_success_summary() -> None:
    # Given
    config = StormConfig(
        target=3,
        name="HP 725 K+M",
        settle_seconds=0.0,
        retry_settle_seconds=0.0,
    )

    async def cycle(_: StormConfig) -> None:
        return None

    # When
    result = asyncio.run(execute_storm(config, cycle))

    # Then
    assert result.render() == (
        "target=3, successes=3, failures=0, attempts=3, "
        "recoveredCycles=0, lastError=null"
    )


def test_execute_storm_counts_recovered_cycle() -> None:
    # Given
    config = StormConfig(
        target=2,
        name="HP 725 K+M",
        settle_seconds=0.0,
        retry_settle_seconds=0.0,
    )
    attempts = 0

    async def cycle(_: StormConfig) -> None:
        nonlocal attempts
        attempts += 1
        if attempts == 1:
            raise StormCycleError(stage="connect", detail="transient")

    # When
    result = asyncio.run(execute_storm(config, cycle))

    # Then
    assert result.successes == 2
    assert result.attempts == 3
    assert result.recovered_cycles == 1
    assert result.last_error is None


def test_execute_storm_stops_at_first_hard_failure() -> None:
    # Given
    config = StormConfig(
        target=100,
        name="HP 725 K+M",
        settle_seconds=0.0,
        retry_settle_seconds=0.0,
    )

    async def cycle(_: StormConfig) -> None:
        raise StormCycleError(stage="scan", detail="not found")

    typed_cycle: Callable[[StormConfig], Awaitable[None]] = cycle

    # When
    result = asyncio.run(execute_storm(config, typed_cycle))

    # Then
    assert result.successes == 0
    assert result.failures == 1
    assert result.attempts == 6
    assert result.failure_cycle == 1
    assert result.last_error == "scan: not found"
