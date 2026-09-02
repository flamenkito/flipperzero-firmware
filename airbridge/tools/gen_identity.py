#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# ─── How to run ───
# python3 airbridge/tools/gen_identity.py
"""Generate the browser BLE identity module from the FAP config and firmware UUID header."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import re
from typing import Final


MONOREPO_ROOT: Final = Path(__file__).resolve().parents[2]
ROOT: Final = Path(__file__).resolve().parents[1]
CONFIG_PATH: Final = ROOT / "config" / "pocket_airbridge.conf"
UUID_HEADER_PATH: Final = (
    MONOREPO_ROOT
    / "targets"
    / "f7"
    / "ble_glue"
    / "services"
    / "airbridge_serial_uuid.h"
)
OUTPUT_PATH: Final = ROOT / "web" / "airbridge-identity.js"

UUID_DEFINITIONS: Final = (
    ("Service", "BLE_SVC_AIRBRIDGE_SERIAL_SERVICE_UUID", "SERIAL_SERVICE_UUID"),
    ("TX", "BLE_SVC_AIRBRIDGE_SERIAL_TX_CHAR_UUID", "SERIAL_TX_CHAR_UUID"),
    ("RX", "BLE_SVC_AIRBRIDGE_SERIAL_RX_CHAR_UUID", "SERIAL_RX_CHAR_UUID"),
    ("Flow control", "BLE_SVC_AIRBRIDGE_SERIAL_FLOW_CONTROL_UUID", "SERIAL_FLOW_CONTROL_UUID"),
    ("Status", "BLE_SVC_AIRBRIDGE_SERIAL_STATUS_UUID", "SERIAL_STATUS_UUID"),
)
UUID_COMMENT_PATTERN: Final = re.compile(
    r"^\s*\*\s*(Service|TX|RX|Flow control|Status):\s*"
    + r"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\s*$",
    re.IGNORECASE | re.MULTILINE,
)
UUID_MACRO_PATTERN: Final = re.compile(
    r"^#define[ \t]+(?P<name>BLE_SVC_AIRBRIDGE_SERIAL_[A-Z_]+)[ \t]*"
    + r"(?:\\[ \t]*\r?\n[ \t]*)?\{(?P<bytes>[^}]*)\}",
    re.MULTILINE,
)
BYTE_PATTERN: Final = re.compile(r"0x([0-9a-f]{2})\b", re.IGNORECASE)


class IdentityGenerationError(RuntimeError):
    """Raised when the config or UUID header violates the identity contract."""


@dataclass(frozen=True, slots=True)
class BrowserIdentity:
    """The subset of FAP identity data required by Web Bluetooth."""

    ble_name: str
    ble_mfg_company_id: int
    serial_service_uuid: str
    serial_tx_char_uuid: str
    serial_rx_char_uuid: str
    serial_flow_control_uuid: str
    serial_status_uuid: str


def parse_config(path: Path) -> tuple[str, int]:
    """Parse the required browser identity values from the canonical FAP config."""
    values: dict[str, str] = {}
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith(";"):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            if "profile" in values:
                raise IdentityGenerationError(f"{path}:{line_number}: expected key=value")
            values["profile"] = line
            continue
        normalized_key = key.strip()
        if not normalized_key:
            raise IdentityGenerationError(f"{path}:{line_number}: identity key is blank")
        if normalized_key in values:
            raise IdentityGenerationError(f"{path}:{line_number}: duplicate key {normalized_key}")
        values[normalized_key] = value.strip()

    try:
        ble_name = values["ble_name"]
        company_id = int(values["ble_mfg_company"], 0)
    except KeyError as error:
        raise IdentityGenerationError(f"{path}: missing required key {error.args[0]}") from error
    except ValueError as error:
        raise IdentityGenerationError(f"{path}: ble_mfg_company must be an integer") from error

    if not ble_name:
        raise IdentityGenerationError(f"{path}: ble_name must not be blank")
    if not 0 <= company_id <= 0xFFFF:
        raise IdentityGenerationError(f"{path}: ble_mfg_company must fit in uint16")
    return ble_name, company_id


def bytes_to_uuid(values: list[str]) -> str:
    """Render a 16-byte BLE UUID array as a canonical 8-4-4-4-12 hex string.

    The firmware header stores each UUID macro in controller byte order (MSB
    first), so the bytes can be joined left-to-right and grouped directly. The
    browser and bleak see the full 16-byte reversal — callers that need the
    on-air form must reverse the bytes with ``reverse_uuid_bytes`` before
    invoking this formatter.
    """
    if len(values) != 16:
        raise IdentityGenerationError(f"UUID macro must contain 16 bytes, found {len(values)}")
    compact = "".join(values).lower()
    return f"{compact[:8]}-{compact[8:12]}-{compact[12:16]}-{compact[16:20]}-{compact[20:]}"


def reverse_uuid_bytes(values: list[str]) -> list[str]:
    """Return a 16-byte UUID array reversed, so controller order becomes on-air order."""
    if len(values) != 16:
        raise IdentityGenerationError(f"UUID macro must contain 16 bytes, found {len(values)}")
    return list(reversed(values))


def parse_serial_uuids(path: Path) -> tuple[str, str, str, str, str]:
    """Parse UUID macro bytes and verify them against the header's canonical comment.

    The byte-array macro is in controller order while the header comment is
    the canonical on-air form. Reverse each macro before comparing and
    returning the UUID that the browser and bleak actually observe.
    """
    header = path.read_text(encoding="utf-8")
    documented: dict[str, str] = {}
    for match in UUID_COMMENT_PATTERN.finditer(header):
        documented[match.group(1).casefold()] = match.group(2).casefold()
    macros: dict[str, list[str]] = {}
    for match in UUID_MACRO_PATTERN.finditer(header):
        macros[match.group("name")] = BYTE_PATTERN.findall(match.group("bytes"))

    parsed: list[str] = []
    for label, macro_name, _ in UUID_DEFINITIONS:
        try:
            macro_bytes = macros[macro_name]
            documented_uuid = documented[label.casefold()]
        except KeyError as error:
            raise IdentityGenerationError(f"{path}: missing UUID definition {error.args[0]}") from error
        on_air_uuid = bytes_to_uuid(reverse_uuid_bytes(macro_bytes))
        if on_air_uuid != documented_uuid:
            raise IdentityGenerationError(
                f"{path}: {macro_name} reverses to {on_air_uuid}, expected documented {documented_uuid}",
            )
        parsed.append(on_air_uuid)
    return parsed[0], parsed[1], parsed[2], parsed[3], parsed[4]


def render_module(identity: BrowserIdentity) -> str:
    """Render the deterministic ES module consumed by the browser pages."""
    return "\n".join(
        (
            "// generated by tools/gen_identity.py — do not edit",
            "export const identity = Object.freeze({",
            f"  BLE_NAME: {json.dumps(identity.ble_name)},",
            f"  BLE_MFG_COMPANY_ID: 0x{identity.ble_mfg_company_id:04X},",
            f"  SERIAL_SERVICE_UUID: {json.dumps(identity.serial_service_uuid)},",
            f"  SERIAL_TX_CHAR_UUID: {json.dumps(identity.serial_tx_char_uuid)},",
            f"  SERIAL_RX_CHAR_UUID: {json.dumps(identity.serial_rx_char_uuid)},",
            f"  SERIAL_FLOW_CONTROL_UUID: {json.dumps(identity.serial_flow_control_uuid)},",
            f"  SERIAL_STATUS_UUID: {json.dumps(identity.serial_status_uuid)},",
            "});",
            "",
        ),
    )


def main() -> None:
    """Generate the browser identity module from the two canonical sources."""
    ble_name, company_id = parse_config(CONFIG_PATH)
    service_uuid, tx_uuid, rx_uuid, flow_control_uuid, status_uuid = parse_serial_uuids(UUID_HEADER_PATH)
    identity = BrowserIdentity(
        ble_name=ble_name,
        ble_mfg_company_id=company_id,
        serial_service_uuid=service_uuid,
        serial_tx_char_uuid=tx_uuid,
        serial_rx_char_uuid=rx_uuid,
        serial_flow_control_uuid=flow_control_uuid,
        serial_status_uuid=status_uuid,
    )
    _ = OUTPUT_PATH.write_text(render_module(identity), encoding="utf-8")
    print(f"Generated {OUTPUT_PATH.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
