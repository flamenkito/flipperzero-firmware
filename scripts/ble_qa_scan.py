#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak"]
# ///
# ─── How to run ───
# Install the sole third-party dependency: pip3 install bleak
# Run the zero-hardware unit tests: python3 scripts/ble_qa_scan.py --selftest
# Scan the active FAP: python3 scripts/ble_qa_scan.py scan --timeout 8
# Enumerate GATT (accept the OS numeric-comparison pairing prompt):
#   python3 scripts/ble_qa_scan.py gatt --timeout 12
# ──────────────────
# noqa: SIZE_OK — W8 requires this one standalone scanner to own passive, GATT, stock, and synthetic QA modes.
"""Assert the BLE identity contract while Pocket AirBridge is active."""

from __future__ import annotations

import argparse
import asyncio
import importlib
import platform
import re
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from collections.abc import Iterable
from typing import Final

ROOT: Final = Path(__file__).resolve().parents[1]
CONFIG_PATH: Final = ROOT / "config" / "pocket_airbridge.conf"
IDENTITY_PATH: Final = ROOT / "web" / "airbridge-identity.js"
BASE_UUID: Final = "-0000-1000-8000-00805f9b34fb"
HIDS_UUID: Final = "00001812" + BASE_UUID
STANDARD_SERVICES: Final = frozenset(
    {"00001800" + BASE_UUID, "00001801" + BASE_UUID, "0000180a" + BASE_UUID,
     "0000180f" + BASE_UUID, HIDS_UUID},
)
DIS_CHARS: Final = frozenset(
    {"00002a24" + BASE_UUID, "00002a25" + BASE_UUID, "00002a29" + BASE_UUID,
     "00002a50" + BASE_UUID},
)
HP_OUIS: Final = frozenset({"3C:52:82", "48:0F:CF", "94:57:A5", "3C:D9:2B",
                            "B4:B6:76", "2C:44:FD", "A0:D3:C1", "40:B0:34"})
MAC_PATTERN: Final = re.compile(r"^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$", re.IGNORECASE)
GIT_HASH: Final = re.compile(r"[0-9a-f]{7,40}", re.IGNORECASE)
IDENTITY_UUID: Final = re.compile(
    r"\b(SERIAL_(?:SERVICE|TX_CHAR|RX_CHAR|FLOW_CONTROL|STATUS)_UUID)\s*:\s*['\"]"
    r"([0-9a-f-]{36})['\"]",
    re.IGNORECASE,
)


class SetupError(RuntimeError):
    pass


class BleOperationError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class IdentityConfig:
    name: str
    mac: str
    appearance: int
    company_id: int
    dis_mfr: str
    dis_model: str
    dis_serial: str


@dataclass(frozen=True, slots=True)
class SerialIdentity:
    service: str
    characteristics: frozenset[str]


@dataclass(frozen=True, slots=True)
class Advertisement:
    address: str
    device_name: str | None
    local_name: str | None
    service_uuids: tuple[str, ...]
    manufacturer_data: tuple[tuple[int, bytes], ...]
    rendered: str


@dataclass(frozen=True, slots=True)
class Evidence:
    expected: str
    actual: str


@dataclass(frozen=True, slots=True)
class Check:
    status: str
    assertion: str
    evidence: Evidence

    @property
    def passed(self) -> bool:
        return self.status != "FAIL"


@dataclass(frozen=True, slots=True)
class GattService:
    uuid: str
    characteristics: tuple[str, ...]
    readable: tuple[str, ...]


@dataclass(frozen=True, slots=True)
class GattSnapshot:
    connected: bool
    services: tuple[GattService, ...]
    values: tuple[tuple[str, bytes], ...]
    unreadable: tuple[str, ...]


def check(assertion: str, passed: bool, evidence: Evidence) -> Check:
    return Check("PASS" if passed else "FAIL", assertion, evidence)


def parse_config_text(text: str, source: str) -> IdentityConfig:
    """Tolerantly parse top-level key=value config, preserving the bare profile line."""

    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
        else:
            _ = values.setdefault("profile", line)
    required = ("ble_name", "ble_mac", "ble_appearance", "ble_mfg_company",
                "ble_dis_mfr", "ble_dis_model", "ble_dis_serial")
    missing = [key for key in required if not values.get(key)]
    if missing:
        raise SetupError(f"{source}: missing required config keys: {', '.join(missing)}")
    try:
        appearance = int(values["ble_appearance"], 0)
        company_id = int(values["ble_mfg_company"], 0)
    except ValueError as error:
        raise SetupError(f"{source}: BLE numeric config is not an integer") from error
    mac = values["ble_mac"].upper()
    if not MAC_PATTERN.fullmatch(mac):
        raise SetupError(f"{source}: ble_mac is not a six-octet colon-separated address")
    return IdentityConfig(values["ble_name"], mac, appearance, company_id,
                          values["ble_dis_mfr"], values["ble_dis_model"],
                          values["ble_dis_serial"])


def load_config(path: Path) -> IdentityConfig:
    return parse_config_text(path.read_text(encoding="utf-8"), str(path))


def normalize_uuid(value: str) -> str:
    compact = value.casefold()
    if re.fullmatch(r"[0-9a-f]{4}", compact):
        return "0000" + compact + BASE_UUID
    if re.fullmatch(r"[0-9a-f]{8}", compact):
        return compact + BASE_UUID
    return compact


def load_serial_identity(path: Path) -> SerialIdentity:
    """Parse all five generated AirBridge UUID constants without hard-coding them."""

    values = {name.upper(): normalize_uuid(uuid) for name, uuid in IDENTITY_UUID.findall(
        path.read_text(encoding="utf-8"),
    )}
    expected = {"SERIAL_SERVICE_UUID", "SERIAL_TX_CHAR_UUID", "SERIAL_RX_CHAR_UUID",
                "SERIAL_FLOW_CONTROL_UUID", "SERIAL_STATUS_UUID"}
    if set(values) != expected:
        raise SetupError(f"{path}: missing or malformed AirBridge identity UUID constants")
    return SerialIdentity(values["SERIAL_SERVICE_UUID"], frozenset(
        value for name, value in values.items() if name != "SERIAL_SERVICE_UUID"
    ))


def display_uuids(values: Iterable[str]) -> str:
    return ", ".join(sorted(value[:8] if value.endswith(BASE_UUID) else value for value in values))


def mac_integer(value: str) -> int | None:
    return int(value.replace(":", ""), 16) if MAC_PATTERN.fullmatch(value) else None


def candidate_checks(advertisement: Advertisement, config: IdentityConfig, system: str) -> tuple[Check, ...]:
    """Evaluate all per-advertisement identity assertions without BLE I/O."""

    actual_services = frozenset(normalize_uuid(value) for value in advertisement.service_uuids)
    company_ids = frozenset(company for company, _ in advertisement.manufacturer_data)
    hidden_name = "flipper" not in advertisement.rendered.casefold()
    exact_name = advertisement.local_name == config.name
    truncated_name = (advertisement.local_name is not None and bool(advertisement.local_name)
                      and advertisement.local_name != config.name and config.name.startswith(advertisement.local_name)
                      and len(advertisement.local_name) < len(config.name))
    name_actual = "exact advertised local name" if exact_name else (
        f"stack-truncated prefix — full name verified via GAP char in gatt mode ({advertisement.local_name!r})"
        if truncated_name else f"ADV name {advertisement.local_name!r}")
    mac_visible = system != "Darwin"
    mac_evidence = Evidence(config.mac, advertisement.address)
    mac_check = check("MAC equals configured address", advertisement.address.upper() == config.mac,
                      mac_evidence) if mac_visible else Check(
        "WARN", "MAC equals configured address", Evidence(config.mac,
        f"{advertisement.address} (CoreBluetooth exposes an opaque UUID on Darwin)"))
    oui_actual = advertisement.address[:8].upper()
    oui_check = check("MAC OUI is in the HP allowlist", oui_actual in HP_OUIS,
                      Evidence("{" + ", ".join(sorted(HP_OUIS)) + "}", oui_actual)) if mac_visible else Check(
        "WARN", "MAC OUI is in the HP allowlist", Evidence("mandatory on Linux/Windows",
        "skipped on Darwin: CoreBluetooth does not expose the on-air MAC"))
    return (
        check("advertised local name is exact or scan-response-verified prefix", exact_name or truncated_name,
              Evidence(config.name, name_actual)),
        check('"flipper" absent from name, advertising data, and scan response', hidden_name,
              Evidence("case-insensitive absence", "absent" if hidden_name else "found in advertisement data")),
        mac_check,
        oui_check,
        check("advertised service UUIDs are HIDS only", actual_services == frozenset({HIDS_UUID}),
              Evidence("0x1812", display_uuids(actual_services))),
        check("HP manufacturer data is present", config.company_id in company_ids,
              Evidence(f"0x{config.company_id:04X}", ", ".join(f"0x{id_:04X}" for id_ in sorted(company_ids)) or "none")),
    )


def scan_checks(advertisements: tuple[Advertisement, ...], config: IdentityConfig, system: str) -> tuple[Advertisement | None, tuple[Check, ...]]:
    """Find the named target and assert that a second beacon is not present."""

    targets = tuple(item for item in advertisements if item.local_name == config.name or (
        item.local_name is not None and bool(item.local_name) and config.name.startswith(item.local_name)))
    if not targets:
        return None, (Check("FAIL", "TARGET NOT FOUND", Evidence(
            f"advertisement local name {config.name!r}", "no matching advertisement observed")),)
    target = targets[0]
    target_value = mac_integer(target.address)
    nearby = tuple(item.address for item in advertisements if item != target and target_value is not None
                   and (candidate_value := mac_integer(item.address)) is not None
                   and abs(candidate_value - target_value) <= 1)
    source_check = check("exact-name advertisement source is unique", len(targets) == 1,
                         Evidence("1", str(len(targets))))
    nearby_check = check("no adjacent-MAC advertisement source exists", not nearby,
                         Evidence("none", ", ".join(nearby) or "none")) if system != "Darwin" else Check(
        "WARN", "no adjacent-MAC advertisement source exists", Evidence("mandatory on Linux/Windows",
        "skipped on Darwin: CoreBluetooth address is opaque"))
    return target, (check("TARGET FOUND", True, Evidence(config.name, target.address)),
                    *candidate_checks(target, config, system), source_check, nearby_check)


async def discover_advertisements(timeout: float) -> tuple[Advertisement, ...]:
    """Passively collect Bleak's merged advertisement and scan-response records."""

    try:
        scanner_class = getattr(importlib.import_module("bleak"), "BleakScanner")
        bleak_error = getattr(importlib.import_module("bleak.exc"), "BleakError")
    except ModuleNotFoundError as error:
        raise SetupError("bleak is required for BLE I/O; install it with: pip3 install bleak") from error
    try:
        discovered = await scanner_class.discover(timeout=timeout, return_adv=True)
    except (bleak_error, OSError, TimeoutError) as error:
        raise BleOperationError(f"passive scan failed: {error}") from error
    records: list[Advertisement] = []
    for device, data in discovered.values():
        payloads = tuple(data.manufacturer_data.values()) + tuple(data.service_data.values())
        rendered = "\n".join((str(device.name), str(data.local_name), repr(data),
                               *(bytes(value).decode("latin-1") for value in payloads)))
        records.append(Advertisement(str(device.address), device.name, data.local_name,
                                     tuple(data.service_uuids), tuple((int(key), bytes(value))
                                     for key, value in data.manufacturer_data.items()), rendered))
    return tuple(records)


async def _enumerate_gatt(client, bleak_error) -> GattSnapshot:
    """Enumerate services and attempt to read every readable characteristic."""

    services: list[GattService] = []
    values: list[tuple[str, bytes]] = []
    unreadable: list[str] = []
    for service in client.services:
        chars = tuple(normalize_uuid(characteristic.uuid) for characteristic in service.characteristics)
        readable = tuple(normalize_uuid(characteristic.uuid) for characteristic in service.characteristics
                         if "read" in characteristic.properties)
        services.append(GattService(normalize_uuid(service.uuid), chars, readable))
        for characteristic in service.characteristics:
            if "read" not in characteristic.properties:
                continue
            uuid = normalize_uuid(characteristic.uuid)
            try:
                values.append((uuid, bytes(await client.read_gatt_char(characteristic.uuid))))
            except (bleak_error, OSError, TimeoutError):
                unreadable.append(uuid)
    return GattSnapshot(client.is_connected, tuple(services), tuple(values), tuple(unreadable))


async def inspect_gatt(address: str, timeout: float) -> GattSnapshot:
    """Connect, trigger pairing via a protected read, enumerate, and retry once on permission errors."""

    try:
        client_class = getattr(importlib.import_module("bleak"), "BleakClient")
        bleak_error = getattr(importlib.import_module("bleak.exc"), "BleakError")
    except ModuleNotFoundError as error:
        raise SetupError("bleak is required for BLE I/O; install it with: pip3 install bleak") from error
    pair_requested = platform.system() != "Darwin"
    try:
        async with client_class(address, pair=pair_requested, timeout=timeout) as client:
            # Trigger CoreBluetooth pairing on first authenticated access
            if platform.system() == "Darwin":
                try:
                    await client.read_gatt_char("00002a29" + BASE_UUID)
                except (bleak_error, OSError, TimeoutError):
                    pass
            snapshot = await _enumerate_gatt(client, bleak_error)
            if snapshot.unreadable:
                print(f"  GATT retry: {len(snapshot.unreadable)} characteristic(s) unreadable on first pass; waiting 2s for pairing to complete...")
                await asyncio.sleep(2)
                snapshot = await _enumerate_gatt(client, bleak_error)
                print(f"  GATT retry: second pass has {len(snapshot.unreadable)} unreadable characteristic(s)")
            return snapshot
    except (bleak_error, OSError, TimeoutError) as error:
        raise BleOperationError(f"GATT pairing/connection failed: {error}") from error


def gatt_checks(snapshot: GattSnapshot, config: IdentityConfig, serial: SerialIdentity, system: str) -> tuple[Check, ...]:
    """Evaluate the exact active-profile GATT contract from a captured snapshot."""

    services = {service.uuid: service for service in snapshot.services}
    values = dict(snapshot.values)
    dis = services.get("0000180a" + BASE_UUID)
    serial_service = services.get(serial.service)
    def text(uuid: str) -> str:
        return values.get(uuid, b"").decode("utf-8", errors="replace") if uuid in values else "<not read>"
    appearance = int.from_bytes(values.get("00002a01" + BASE_UUID, b""), "little") if "00002a01" + BASE_UUID in values else -1
    all_text = "\n".join(value.decode("latin-1") for _, value in snapshot.values)
    dis_actual = frozenset(dis.characteristics) if dis else frozenset()
    serial_actual = frozenset(serial_service.characteristics) if serial_service else frozenset()
    vendor_dis = tuple(value for value in dis_actual if value not in DIS_CHARS)
    expected_reads = frozenset(value for service in snapshot.services for value in service.readable)
    actual_reads = frozenset(value for value, _ in snapshot.values)
    pairing_actual = "connected; pairing requested" if snapshot.connected and system != "Darwin" else (
        "connected; CoreBluetooth pairs on authenticated access" if snapshot.connected else "not connected")
    if system == "Darwin":
        expected_services = {"0000180a" + BASE_UUID, "0000180f" + BASE_UUID, serial.service}
        system_claimed = {"00001800" + BASE_UUID, "00001801" + BASE_UUID, HIDS_UUID}
        visible_ok = frozenset(services) == expected_services
        claimed_present = frozenset(services) & system_claimed
        service_assertion = check("GATT visible services are DIS, Battery, and AirBridge serial on Darwin",
                                  visible_ok, Evidence(display_uuids(expected_services), display_uuids(services)))
        claimed_assertion = check("system-claimed services (GAP/GATT/HIDS) are absent on Darwin",
                                  not claimed_present, Evidence("none present", display_uuids(claimed_present) or "none")) if claimed_present else Check(
            "WARN", "system-claimed services (GAP/GATT/HIDS) are absent on Darwin", Evidence("absent (expected on Darwin)",
            "absent — macOS CoreBluetooth hides these from GATT clients"))
        gap_name_assertion = Check("WARN", "GAP device-name characteristic equals config",
                                   Evidence(config.name, "<GAP service hidden by CoreBluetooth>"))
        gap_appearance_assertion = Check("WARN", "GAP appearance characteristic equals config",
                                         Evidence(f"0x{config.appearance:04X}", "<GAP service hidden by CoreBluetooth>"))
    else:
        expected_services = STANDARD_SERVICES | {serial.service}
        visible_ok = frozenset(services) == expected_services
        service_assertion = check("GATT services are exactly GAP, GATT, DIS, Battery, HIDS, and AirBridge serial",
                                  visible_ok, Evidence(display_uuids(expected_services), display_uuids(services)))
        claimed_assertion = check("system-claimed services check", True, Evidence("N/A on Linux/Windows", "N/A"))
        gap_name_assertion = check("GAP device-name characteristic equals config", text("00002a00" + BASE_UUID) == config.name,
                                   Evidence(config.name, text("00002a00" + BASE_UUID)))
        gap_appearance_assertion = check("GAP appearance characteristic equals config", appearance == config.appearance,
                                         Evidence(f"0x{config.appearance:04X}", f"0x{appearance:04X}" if appearance >= 0 else "<not read>"))
    return (
        check("pair/connect target", snapshot.connected, Evidence("connected after OS pairing flow", pairing_actual)),
        service_assertion,
        claimed_assertion,
        check("all readable characteristics were inspected", not snapshot.unreadable and actual_reads == expected_reads,
              Evidence(display_uuids(expected_reads), display_uuids(actual_reads) or "none")),
        check("DIS characteristic set has no RPC-version vendor characteristic", not vendor_dis and dis_actual == DIS_CHARS,
              Evidence(display_uuids(DIS_CHARS), display_uuids(dis_actual))),
        check("DIS manufacturer string equals config", text("00002a29" + BASE_UUID) == config.dis_mfr,
              Evidence(config.dis_mfr, text("00002a29" + BASE_UUID))),
        check("DIS model string equals config", text("00002a24" + BASE_UUID) == config.dis_model,
              Evidence(config.dis_model, text("00002a24" + BASE_UUID))),
        check("DIS serial string equals config", text("00002a25" + BASE_UUID) == config.dis_serial,
              Evidence(config.dis_serial, text("00002a25" + BASE_UUID))),
        check("no readable characteristic exposes a git-hash-shaped string", not GIT_HASH.search(all_text),
              Evidence("no [0-9a-f]{7,40}", "absent" if not GIT_HASH.search(all_text) else "git-hash-shaped value found")),
        gap_name_assertion,
        gap_appearance_assertion,
        check("AirBridge serial service has exactly the four canonical characteristics", serial_actual == serial.characteristics,
              Evidence(display_uuids(serial.characteristics), display_uuids(serial_actual))),
    )


def stock_checks(advertisements: tuple[Advertisement, ...]) -> tuple[Check, ...]:
    """Confirm the stock Flipper identity has returned after the FAP has stopped."""

    targets = tuple(item for item in advertisements if (item.local_name or "").startswith("Flipper"))
    stock_services = tuple(item for item in targets if any(
        normalize_uuid(value)[:8] in {"0000fe60", "0000fe61", "0000fe62", "0000fe63", "0000fe64"}
        for value in item.service_uuids))
    return (
        check("STOCK TARGET FOUND", bool(targets), Evidence("local name prefix 'Flipper'",
              ", ".join(item.local_name or "<unnamed>" for item in targets) or "none")),
        check("stock fe60-family serial service is advertised", bool(stock_services),
              Evidence("0xFE60–0xFE64 present", display_uuids(
              value for item in stock_services for value in item.service_uuids) or "none")),
    )


def print_checks(title: str, checks: Iterable[Check]) -> bool:
    results = tuple(checks)
    print(f"\n{title}\nSTATUS | ASSERTION | EXPECTED | ACTUAL\n" + "-" * 104)
    for result in results:
        print(f"{result.status:<6} | {result.assertion} | {result.evidence.expected} | {result.evidence.actual}")
    return all(result.passed for result in results)


def selftest() -> int:
    """Run config, identity, and advertisement assertion tests without BLE hardware."""

    config = load_config(CONFIG_PATH)
    serial = load_serial_identity(IDENTITY_PATH)
    config_text = CONFIG_PATH.read_text(encoding="utf-8")
    profile_line = next(line for line in config_text.splitlines() if line.strip().startswith("profile="))
    bare_profile = parse_config_text(config_text.replace(profile_line, profile_line.partition("=")[2].strip()), "synthetic-bare-profile")
    good = Advertisement(config.mac, config.name, config.name, ("1812",),
                         ((config.company_id, b"HP"),), "complete name and HP scan response")
    good_target, good_checks = scan_checks((good,), config, "Linux")
    truncated = Advertisement(config.mac, config.name, config.name[:16], ("1812",),
                              ((config.company_id, b"HP"),), "truncated advertisement name")
    truncated_target, truncated_checks = scan_checks((truncated,), config, "Linux")
    longer = Advertisement(config.mac, config.name, config.name + "X", ("1812",),
                           ((config.company_id, b"HP"),), "name longer than expected")
    longer_name = candidate_checks(longer, config, "Linux")[0]
    bad = Advertisement("00:11:22:33:44:55", "Flipper", "Flipper Beacon", ("fe60",),
                        ((0x0001, b"Flipper"),), "Flipper scan response")
    bad_checks = candidate_checks(bad, config, "Linux")
    gap_uuid = "00001800" + BASE_UUID
    dis_uuid = "0000180a" + BASE_UUID
    name_uuid, appearance_uuid = "00002a00" + BASE_UUID, "00002a01" + BASE_UUID
    mfr_uuid, model_uuid, serial_uuid, pnp_uuid = ("00002a29" + BASE_UUID, "00002a24" + BASE_UUID,
                                                     "00002a25" + BASE_UUID, "00002a50" + BASE_UUID)
    gatt_services = tuple(GattService(uuid, (name_uuid, appearance_uuid), (name_uuid, appearance_uuid)) if uuid == gap_uuid else
                          GattService(uuid, tuple(DIS_CHARS), tuple(DIS_CHARS)) if uuid == dis_uuid else
                          GattService(uuid, tuple(serial.characteristics), tuple(serial.characteristics)) if uuid == serial.service else
                          GattService(uuid, (), ()) for uuid in STANDARD_SERVICES | {serial.service})
    gatt_values = ((name_uuid, config.name.encode()), (appearance_uuid, config.appearance.to_bytes(2, "little")),
                   (mfr_uuid, config.dis_mfr.encode()), (model_uuid, config.dis_model.encode()),
                   (serial_uuid, config.dis_serial.encode()), (pnp_uuid, b"\x01\xf0\x03\x41\x53\x26\x01"),
                   *((uuid, b"ok") for uuid in serial.characteristics))
    gatt_good = gatt_checks(GattSnapshot(True, gatt_services, gatt_values, ()), config, serial, "Linux")
    darwin_gatt_services = tuple(GattService(uuid, tuple(DIS_CHARS), tuple(DIS_CHARS)) if uuid == dis_uuid else
                                 GattService(uuid, tuple(serial.characteristics), tuple(serial.characteristics)) if uuid == serial.service else
                                 GattService(uuid, (), ()) for uuid in {"0000180a" + BASE_UUID, "0000180f" + BASE_UUID, serial.service})
    darwin_gatt_values = ((mfr_uuid, config.dis_mfr.encode()), (model_uuid, config.dis_model.encode()),
                          (serial_uuid, config.dis_serial.encode()), (pnp_uuid, b"\x01\xf0\x03\x41\x53\x26\x01"),
                          *((uuid, b"ok") for uuid in serial.characteristics))
    gatt_darwin = gatt_checks(GattSnapshot(True, darwin_gatt_services, darwin_gatt_values, ()), config, serial, "Darwin")
    parsing = check("config (including bare profile) and browser UUID module parse", bare_profile == config and good_target is not None and len(serial.characteristics) == 4,
                    Evidence("config identity + 1 service/4 characteristic UUIDs", f"{serial.service}; {len(serial.characteristics)} characteristics"))
    truncation = check("truncated ADV name (prefix shorter than expected) is accepted", truncated_target is not None and all(item.passed for item in truncated_checks),
                       Evidence("stack-truncated prefix accepted", "accepted" if truncated_target is not None and all(item.passed for item in truncated_checks) else "rejected"))
    longer_rejected = check("name longer than expected is rejected", not longer_name.passed,
                            Evidence("FAIL for longer name", longer_name.status))
    darwin_mac = candidate_checks(good, config, "Darwin")[2:4]
    darwin_skip = check("Darwin MAC and OUI checks warn-and-skip", all(item.status == "WARN" for item in darwin_mac),
                        Evidence("WARN, WARN", ", ".join(item.status for item in darwin_mac)))
    darwin_gatt_ok = check("Darwin GATT shows reduced visible set and warns for hidden system services",
                           all(item.passed for item in gatt_darwin),
                           Evidence("all Darwin GATT assertions pass", "all pass" if all(item.passed for item in gatt_darwin) else "some fail"))
    rejected = check("bad synthetic advertisement is rejected", not all(item.passed for item in bad_checks),
                     Evidence("one or more identity assertions fail", "failure detected" if not all(item.passed for item in bad_checks) else "unexpectedly accepted"))
    passed = print_checks("SELF-TEST: GOOD SYNTHETIC ADVERTISEMENT", good_checks)
    passed = print_checks("SELF-TEST: TRUNCATED-NAME ADVERTISEMENT", truncated_checks) and passed
    passed = print_checks("SELF-TEST: GOOD SYNTHETIC GATT (Linux)", gatt_good) and passed
    passed = print_checks("SELF-TEST: GOOD SYNTHETIC GATT (Darwin)", gatt_darwin) and passed
    _ = print_checks("SELF-TEST: BAD SYNTHETIC ADVERTISEMENT (EXPECTED FAILURES)", bad_checks)
    passed = print_checks("SELF-TEST: PARSER AND REJECTION ORACLES", (parsing, truncation, longer_rejected, darwin_skip, darwin_gatt_ok, rejected)) and passed
    return 0 if passed else 1


async def run_mode(mode: str, config: IdentityConfig, serial: SerialIdentity, timeout: float) -> int:
    """Run a hardware mode after its config and canonical UUIDs have been parsed."""

    advertisements = await discover_advertisements(timeout)
    match mode:
        case "stock":
            return 0 if print_checks("STOCK RESTORATION SCAN", stock_checks(advertisements)) else 1
        case "scan":
            _, checks = scan_checks(advertisements, config, platform.system())
            return 0 if print_checks("AIRBRIDGE PASSIVE SCAN", checks) else 1
        case "gatt":
            target, checks = scan_checks(advertisements, config, platform.system())
            scan_passed = print_checks("AIRBRIDGE GATT PRE-FLIGHT SCAN", checks)
            if target is None:
                return 1
            print("\nPAIRING: accept the OS numeric-comparison prompt when it appears; do not cancel it.")
            snapshot = await inspect_gatt(target.address, timeout)
            return 0 if scan_passed and print_checks("AIRBRIDGE GATT ENUMERATION", gatt_checks(
                snapshot, config, serial, platform.system())) else 1
        case _:
            raise SetupError(f"unsupported mode: {mode}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("scan", "gatt", "stock"))
    parser.add_argument("--mode", dest="mode_option", choices=("scan", "gatt", "stock"))
    parser.add_argument("--name", help="expected advertised local name (default: config ble_name)")
    parser.add_argument("--mac", help="expected on-air MAC (default: config ble_mac)")
    parser.add_argument("--timeout", type=float, default=8.0, help="passive scan/connect timeout in seconds")
    parser.add_argument("--selftest", action="store_true", help="run synthetic config and assertion tests without BLE I/O")
    args = parser.parse_args()
    if args.mode and args.mode_option:
        parser.error("use either the positional mode or --mode, not both")
    if not args.selftest and not (args.mode or args.mode_option):
        parser.error("choose scan, gatt, or stock (or use --selftest)")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    return args


def main() -> int:
    """Map usage/setup failures to 2 and assertion or adapter failures to 1."""

    args = parse_args()
    if args.selftest:
        return selftest()
    try:
        config = load_config(CONFIG_PATH)
        config = replace(config, name=args.name or config.name, mac=(args.mac or config.mac).upper())
        if not MAC_PATTERN.fullmatch(config.mac):
            raise SetupError("--mac is not a six-octet colon-separated address")
        return asyncio.run(run_mode(args.mode or args.mode_option, config, load_serial_identity(IDENTITY_PATH), args.timeout))
    except SetupError as error:
        print(f"SETUP ERROR: {error}", file=sys.stderr)
        return 2
    except BleOperationError as error:
        print(f"BLE ERROR: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"LOCAL ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
