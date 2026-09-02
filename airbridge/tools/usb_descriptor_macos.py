from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass
import re
import shlex
import subprocess
from tools.usb_descriptor_fixture import CaptureData, CaptureError, Target, UNKNOWN


SYSTEM_PROFILER_COMMAND: tuple[str, ...] = ("system_profiler", "SPUSBDataType", "-detailLevel", "full")
IOREG_COMMAND: tuple[str, ...] = ("ioreg", "-p", "IOUSB", "-l", "-w", "0")
PROPERTY_LINE = re.compile(r'^\s*"(?P<key>[^"]+)" = (?P<value>.*)$')
NODE_LINE = re.compile(r"^\s*(?:\|\s*)*[+`\\]-o\s+.*?<class\s+(?P<class>[^,>]+)")


@dataclass(frozen=True)
class ProfilerSection:
    raw: str
    manufacturer: str | None
    product: str | None
    serial: str | None


@dataclass(frozen=True)
class IoregNode:
    class_name: str
    raw: str
    properties: tuple[tuple[str, str], ...]


def parse_integer(value: str) -> int | None:
    candidate = value.strip()
    match = re.match(r"^(?:0x)?([0-9A-Fa-f]+)\b", candidate)
    if match is None:
        return None
    base = 16 if candidate.lower().startswith("0x") else 10
    try:
        return int(match.group(1), base)
    except ValueError:
        return None


def run_command(command: Sequence[str]) -> str:
    completed = subprocess.run(command, capture_output=True, check=False, text=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or "no stderr output"
        raise CaptureError(f"{shlex.join(command)} failed with exit {completed.returncode}: {detail}")
    return completed.stdout


def field_value(lines: Sequence[str], name: str) -> str | None:
    pattern = re.compile(rf"^\s*{re.escape(name)}:\s*(?P<value>.*?)\s*$")
    for line in lines:
        match = pattern.match(line)
        if match is not None:
            return match.group("value")
    return None


def indentation(line: str) -> int:
    return len(line) - len(line.lstrip())


def profiler_sections(raw: str, target: Target) -> tuple[ProfilerSection, ...]:
    lines = raw.splitlines()
    sections: list[ProfilerSection] = []
    for index, line in enumerate(lines):
        product_text = field_value((line,), "Product ID")
        product_id = parse_integer(product_text) if product_text is not None else None
        if product_id != target.product_id:
            continue
        field_indent = indentation(line)
        start = index
        for cursor in range(index - 1, -1, -1):
            prior = lines[cursor]
            if prior.strip().endswith(":") and indentation(prior) < field_indent:
                start = cursor
                break
        heading_indent = indentation(lines[start])
        end = len(lines)
        for cursor in range(index + 1, len(lines)):
            candidate = lines[cursor]
            if candidate.strip() and indentation(candidate) <= heading_indent:
                end = cursor
                break
        section_lines = lines[start:end]
        vendor_text = field_value(section_lines, "Vendor ID")
        location_text = field_value(section_lines, "Location ID")
        vendor_id = parse_integer(vendor_text) if vendor_text is not None else None
        location_id = parse_integer(location_text) if location_text is not None else None
        if vendor_id == target.vendor_id and (target.location_id is None or location_id == target.location_id):
            sections.append(
                ProfilerSection(
                    raw="\n".join(section_lines),
                    manufacturer=field_value(section_lines, "Manufacturer"),
                    product=lines[start].strip().rstrip(":"),
                    serial=field_value(section_lines, "Serial Number"),
                )
            )
    return tuple(sections)


def ioreg_nodes(raw: str) -> tuple[IoregNode, ...]:
    lines = raw.splitlines()
    nodes: list[IoregNode] = []
    cursor = 0
    while cursor < len(lines):
        node_match = NODE_LINE.match(lines[cursor])
        if node_match is None:
            cursor += 1
            continue
        start = cursor
        while cursor < len(lines) and "{" not in lines[cursor]:
            cursor += 1
        if cursor == len(lines):
            break
        depth = 0
        while cursor < len(lines):
            depth += lines[cursor].count("{") - lines[cursor].count("}")
            cursor += 1
            if depth == 0:
                break
        block_lines = lines[start:cursor]
        properties: list[tuple[str, str]] = []
        for block_line in block_lines:
            property_match = PROPERTY_LINE.match(block_line)
            if property_match is not None:
                properties.append((property_match.group("key"), property_match.group("value")))
        nodes.append(IoregNode(node_match.group("class"), "\n".join(block_lines), tuple(properties)))
    return tuple(nodes)


def property_text(node: IoregNode, name: str) -> str | None:
    for key, value in node.properties:
        if key == name:
            return value.strip().strip('"')
    return None


def property_number(node: IoregNode, name: str) -> int | None:
    text = property_text(node, name)
    return parse_integer(text) if text is not None else None


def selected_ioreg_device(nodes: Iterable[IoregNode], target: Target) -> IoregNode:
    matches = tuple(
        node
        for node in nodes
        if property_number(node, "idVendor") == target.vendor_id
        and property_number(node, "idProduct") == target.product_id
        and (target.location_id is None or property_number(node, "locationID") == target.location_id)
    )
    if not matches:
        raise CaptureError(f"target {target.label} is absent from ioreg -p IOUSB -l -w 0")
    if len(matches) != 1:
        raise CaptureError(f"target {target.label} matched {len(matches)} IORegistry devices; rerun with --location-id")
    return matches[0]


def number_text(value: int | None) -> str | None:
    return str(value) if value is not None else None


def hexadecimal(value: int | None, width: int) -> str | None:
    return f"0x{value:0{width}X}" if value is not None else None


def first_known(values: Iterable[str | None]) -> str:
    for value in values:
        if value is not None and value.strip():
            return value.strip()
    return UNKNOWN


def descriptor_hex(node: IoregNode) -> str:
    for key, value in node.properties:
        if "report" in key.lower() and "descriptor" in key.lower():
            match = re.search(r"<(?P<hex>[0-9A-Fa-f\s]+)>", value)
            if match is not None:
                return re.sub(r"\s+", "", match.group("hex")).upper()
    return UNKNOWN


def add_interfaces(values: dict[str, str], nodes: Iterable[IoregNode], device: IoregNode) -> None:
    location_id = property_number(device, "locationID")
    interfaces = tuple(
        node for node in nodes if property_number(node, "bInterfaceClass") is not None and property_number(node, "locationID") == location_id
    )
    if not interfaces:
        values["interfaces.count"] = UNKNOWN
        return
    ordered = tuple(sorted(interfaces, key=lambda node: property_number(node, "bInterfaceNumber") or -1))
    values["interfaces.count"] = str(len(ordered))
    endpoints = tuple(
        node for node in nodes if property_number(node, "bEndpointAddress") is not None and property_number(node, "locationID") == location_id
    )
    for ordinal, interface in enumerate(ordered):
        prefix = f"interface.{ordinal:02d}"
        interface_number = property_number(interface, "bInterfaceNumber")
        values[f"{prefix}.number"] = first_known((number_text(interface_number),))
        for property_name, field_name in (("bInterfaceClass", "class"), ("bInterfaceSubClass", "subclass"), ("bInterfaceProtocol", "protocol")):
            values[f"{prefix}.{field_name}"] = first_known((hexadecimal(property_number(interface, property_name), 2),))
        interface_endpoints = tuple(
            node for node in endpoints if property_number(node, "bInterfaceNumber") == interface_number
        )
        values[f"{prefix}.endpoint_count"] = str(len(interface_endpoints))
        values[f"{prefix}.hid_report_descriptor"] = descriptor_hex(interface)
        for endpoint_ordinal, endpoint in enumerate(interface_endpoints):
            endpoint_prefix = f"{prefix}.endpoint.{endpoint_ordinal:02d}"
            address = property_number(endpoint, "bEndpointAddress")
            values[f"{endpoint_prefix}.address"] = first_known((hexadecimal(address, 2),))
            values[f"{endpoint_prefix}.direction"] = "IN" if address is not None and address & 0x80 else "OUT"
            values[f"{endpoint_prefix}.interval"] = first_known((number_text(property_number(endpoint, "bInterval")),))
            values[f"{endpoint_prefix}.max_packet_size"] = first_known((number_text(property_number(endpoint, "wMaxPacketSize")),))


def normalized_values(profiler: ProfilerSection, nodes: Iterable[IoregNode], device: IoregNode, target: Target) -> tuple[tuple[str, str], ...]:
    values = {
        "device.VID": f"0x{target.vendor_id:04X}", "device.PID": f"0x{target.product_id:04X}",
        "device.bcdDevice": first_known((hexadecimal(property_number(device, "bcdDevice"), 4),)),
        "device.bcdUSB": first_known((hexadecimal(property_number(device, "bcdUSB"), 4),)),
        "device.iManufacturer": first_known((profiler.manufacturer, property_text(device, "kUSBVendorString"))),
        "device.iManufacturer.index": first_known((number_text(property_number(device, "iManufacturer")),)),
        "device.iProduct": first_known((profiler.product, property_text(device, "kUSBProductString"))),
        "device.iProduct.index": first_known((number_text(property_number(device, "iProduct")),)),
        "device.iSerial": first_known((profiler.serial, property_text(device, "kUSBSerialNumberString"))),
        "device.iSerial.index": first_known((number_text(property_number(device, "iSerialNumber")),)),
        "device.class": first_known((hexadecimal(property_number(device, "bDeviceClass"), 2),)),
        "device.subclass": first_known((hexadecimal(property_number(device, "bDeviceSubClass"), 2),)),
        "device.protocol": first_known((hexadecimal(property_number(device, "bDeviceProtocol"), 2),)),
        "device.bMaxPacketSize0": first_known((number_text(property_number(device, "bMaxPacketSize0")),)),
        "device.bNumConfigurations": first_known((number_text(property_number(device, "bNumConfigurations")),)),
    }
    add_interfaces(values, nodes, device)
    return tuple(sorted(values.items()))


def capture(target: Target) -> CaptureData:
    profiler_raw = run_command(SYSTEM_PROFILER_COMMAND)
    ioreg_raw = run_command(IOREG_COMMAND)
    profiler_matches = profiler_sections(profiler_raw, target)
    if not profiler_matches:
        raise CaptureError(f"target {target.label} is absent from system_profiler SPUSBDataType")
    if len(profiler_matches) != 1:
        raise CaptureError(f"target {target.label} matched {len(profiler_matches)} system_profiler sections; rerun with --location-id")
    nodes = ioreg_nodes(ioreg_raw)
    device = selected_ioreg_device(nodes, target)
    return CaptureData(
        target, SYSTEM_PROFILER_COMMAND, profiler_raw, profiler_matches[0].raw,
        IOREG_COMMAND, ioreg_raw, device.raw, normalized_values(profiler_matches[0], nodes, device, target),
    )
