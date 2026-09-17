#!/usr/bin/env python3
from pathlib import Path
import gzip
import hashlib
import hmac
import re
import struct
import zlib
from collections.abc import Mapping
from typing import Final, override
from html.parser import HTMLParser


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
ASSET_DIGEST_HEADER: Final = ROOT.parent / "applications_user" / "pocket_airbridge" / "airbridge_assets_digest.h"
BUNDLE_MAGIC: Final = b"ABND"
BUNDLE_FORMAT_VERSION: Final = 1
BUNDLE_HEADER: Final = struct.Struct("<4sB3xI")
BOOTSTRAPS = (
    (WEB / "bootstrap.js", 4096),
    (WEB / "bootstrap-ble.js", 16 * 1024),
)
BUNDLES = (
    ("chat-usb.html", "app-usb.html", "WebHIDAdapter"),
    ("chat-ble.html", "app-ble.html", "WebBluetoothAdapter"),
)
COMMON_SCRIPTS: Final = (
    "vendor/js-sha256-0.11.1.js", "airbridge-incremental-sha256.js",
    "airbridge-protocol.js", "airbridge-receive-accumulator.js",
    "airbridge-item-receiver.js", "airbridge-ui.js", "airbridge-file-pass.js",
    "airbridge-outbound.js", "airbridge-chat-outbound.js", "airbridge-chat-receive.js",
    "airbridge-mock-stream-peer.js", "airbridge-chat-mock.js", "airbridge-evidence.js",
    "airbridge-identity.js", "airbridge-ble-packets.js", "airbridge-transports.js",
    "airbridge-terminal.js", "airbridge-chat-page.js",
)
UI_CSS_LINK = '<link rel="stylesheet" href="./airbridge-ui.css">'
IMPORT_RE: Final = re.compile(
    r"\bimport\s*\{(?P<bindings>[^}]+)\}\s*from\s*['\"]\./(?P<path>[\w./-]+)['\"];"
)
EXPORT_RE: Final = re.compile(r"(?m)^export (?:async )?(?:const|function|class) (\w+)")
EXPORT_LIST_RE: Final = re.compile(
    r"(?m)^export \{([^}]+)\}(?: from ['\"]\./([\w./-]+)['\"])?;"
)
CHARACTERIZATION_ONLY_RE: Final = re.compile(
    r"\s*/\* characterization-only:start \*/.*?/\* characterization-only:end \*/\s*",
    re.DOTALL,
)


class BundleFormatError(ValueError):
    pass


class BundleBuildError(ValueError):
    pass


class OfflinePage(HTMLParser):
    @override
    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        for name, value in attrs:
            if name in {"src", "href", "srcset"} and (value is None or not value.startswith("data:")):
                raise BundleBuildError(f"unresolved {tag} resource: {name}={value!r}")
            if tag == "script" and name == "type" and value and value.lower() == "module":
                raise BundleBuildError("bundle still contains module script")


def binding_names(value: str) -> tuple[str, ...]:
    names = tuple(part.strip() for part in value.strip().removesuffix(",").split(","))
    if len(set(names)) != len(names) or any(not re.fullmatch(r"[A-Za-z_$][\w$]*", name) for name in names):
        raise BundleBuildError(f"unsupported or duplicate bindings: {value!r}")
    return names


def inline_scripts(page: str) -> str:
    sources = {
        name: CHARACTERIZATION_ONLY_RE.sub("\n", (WEB / name).read_text(encoding="utf-8"))
        for name in COMMON_SCRIPTS[1:]
    }
    exports = {name: set(EXPORT_RE.findall(source)) for name, source in sources.items()}
    for name, source in sources.items():
        for match in EXPORT_LIST_RE.finditer(source):
            exports[name].update(binding_names(match[1]))
    emitted: set[str] = set()

    def imports(source: str) -> str:
        seen: set[str] = set()
        def replace(match: re.Match[str]) -> str:
            dependency = match["path"]
            names = binding_names(match["bindings"])
            if dependency not in emitted or not set(names) <= exports[dependency] or seen.intersection(names):
                raise BundleBuildError(f"missing, unordered or duplicate import: {match[0]}")
            seen.update(names)
            return f"const {{ {', '.join(names)} }} = __airbridgeModules['{dependency}'];"
        return IMPORT_RE.sub(replace, source)

    modules = ["const __airbridgeModules = Object.create(null);"]
    for name, source in sources.items():
        getters: list[str] = []
        for match in EXPORT_LIST_RE.finditer(source):
            dependency = match[2]
            if dependency:
                names = binding_names(match[1])
                if dependency not in exports or not set(names) <= exports[dependency]:
                    raise BundleBuildError(f"missing re-export: {match[0]}")
                # The protocol/receiver cycle needs deferred lookup, like an ESM re-export.
                getters.extend(f"get {key}() {{ return __airbridgeModules['{dependency}'].{key}; }}" for key in names)
            else:
                getters.extend(binding_names(match[1]))
        body = imports(EXPORT_LIST_RE.sub("", source))
        body = re.sub(r"(?m)^export (?=(?:async )?(?:const|function|class) )", "", body)
        members = ", ".join([*EXPORT_RE.findall(source), *getters])
        modules.append(f"/* bundled: {name} */\n__airbridgeModules['{name}'] = (() => {{\n'use strict';\n{body}\nreturn {{ {members} }};\n}})();")
        emitted.add(name)
    vendor = COMMON_SCRIPTS[0]
    vendor_source = (WEB / vendor).read_text(encoding="utf-8")
    page = replace_once(page, f'<script src="./{vendor}"></script>', f"<script>/* bundled: {vendor} */\n{vendor_source}\n</script>")
    page = imports(page)
    return replace_once(page, '<script type="module">', "<script>\n'use strict';\n" + "\n".join(modules))


def replace_once(page: str, needle: str, replacement: str) -> str:
    if page.count(needle) != 1:
        raise BundleBuildError(f"expected exactly one {needle!r} in chat-usb.html")
    return page.replace(needle, replacement)


def inline_ui_css(page: str) -> str:
    css = (WEB / "airbridge-ui.css").read_text(encoding="utf-8")
    return replace_once(page, UI_CSS_LINK, f"<style>\n{css}\n</style>")


def gzip_deterministic(data: bytes) -> bytes:
    compressed = bytearray(gzip.compress(data, compresslevel=9, mtime=0))
    compressed[9] = 0xFF
    return bytes(compressed)


def asset_digest_matches(data: bytes, expected: bytes) -> bool:
    return hmac.compare_digest(hashlib.sha256(data).digest(), expected)


def build_bundle_container(raw: bytes) -> bytes:
    compressed = gzip_deterministic(raw)
    return BUNDLE_HEADER.pack(BUNDLE_MAGIC, BUNDLE_FORMAT_VERSION, len(raw)) + compressed


def decode_bundle_container(container: bytes) -> bytes:
    if len(container) < BUNDLE_HEADER.size + 2:
        raise BundleFormatError("bundle container is truncated")
    magic, version = container[:4], container[4]
    decompressed_size = int.from_bytes(container[8:12], "little")
    if magic != BUNDLE_MAGIC:
        raise BundleFormatError("bundle magic mismatch")
    if version != BUNDLE_FORMAT_VERSION:
        raise BundleFormatError("bundle format version mismatch")
    compressed = container[BUNDLE_HEADER.size :]
    if compressed[:2] != b"\x1f\x8b":
        raise BundleFormatError("bundle gzip magic mismatch")
    try:
        raw = gzip.decompress(compressed)
    except (gzip.BadGzipFile, EOFError, zlib.error) as error:
        raise BundleFormatError("bundle gzip payload is invalid") from error
    if len(raw) != decompressed_size:
        raise BundleFormatError("bundle decompressed size mismatch")
    return raw


def _c_bytes(data: bytes) -> str:
    return ", ".join(f"0x{value:02X}" for value in data)


def render_digest_header(bootstrap: bytes, bootstrap_ble: bytes, bundles: Mapping[str, bytes]) -> str:
    app_usb = bundles["app-usb.html"]
    app_ble = bundles["app-ble.html"]
    container_usb = build_bundle_container(app_usb)
    container_ble = build_bundle_container(app_ble)
    return "\n".join(
        (
            "/* Generated by airbridge/tools/build_bundle.py; do not edit. */",
            "#pragma once",
            "",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "",
            f"#define AIRBRIDGE_BUNDLE_FORMAT_VERSION {BUNDLE_FORMAT_VERSION}U",
            f"#define AIRBRIDGE_BUNDLE_HEADER_SIZE {BUNDLE_HEADER.size}U",
            "#define AIRBRIDGE_ASSET_SHA256_SIZE 32U",
            f"#define AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE {len(app_usb)}U",
            f"#define AIRBRIDGE_APP_BLE_DECOMPRESSED_SIZE {len(app_ble)}U",
            "",
            f"static const uint8_t AIRBRIDGE_BUNDLE_MAGIC[{len(BUNDLE_MAGIC)}] = {{{_c_bytes(BUNDLE_MAGIC)}}};",
            f"static const uint8_t AIRBRIDGE_BOOTSTRAP_SHA256[32] = {{{_c_bytes(hashlib.sha256(bootstrap).digest())}}};",
            f"static const uint8_t AIRBRIDGE_BOOTSTRAP_BLE_SHA256[32] = {{{_c_bytes(hashlib.sha256(bootstrap_ble).digest())}}};",
            f"static const uint8_t AIRBRIDGE_APP_USB_BUNDLE_SHA256[32] = {{{_c_bytes(hashlib.sha256(container_usb).digest())}}};",
            f"static const uint8_t AIRBRIDGE_APP_BLE_BUNDLE_SHA256[32] = {{{_c_bytes(hashlib.sha256(container_ble).digest())}}};",
            "",
        )
    )


def normalize_bootstrap(data: bytes, path: Path) -> bytes:
    normalized = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n").rstrip(b"\n")
    for offset, value in enumerate(normalized):
        if value < 0x20 or value > 0x7E:
            char = chr(value) if value < 0x80 else f"\\x{value:02x}"
            message = f"{path.relative_to(ROOT)} contains non-US-ASCII byte "
            message += f"0x{value:02X} ({char!r}) at offset {offset}"
            raise BundleBuildError(message)
    return normalized


def build_bundle() -> dict[str, bytes]:
    """Build all bundles in memory, returning {output_name: raw_bytes}."""
    bundles: dict[str, bytes] = {}
    for page_name, output_name, _adapter_name in BUNDLES:
        page = (WEB / page_name).read_text(encoding="utf-8")
        page = inline_scripts(inline_ui_css(page))
        unresolved = re.search(r"\bimport\s*[(\{*]|^\s*(?:import|export)\s|type\s*=\s*['\"]module", page, re.M)
        if unresolved:
            raise BundleBuildError(f"bundle still contains module syntax: {page[unresolved.start():unresolved.start() + 100]!r}")
        OfflinePage().feed(page)
        if re.search(r"@import|\burl\s*\(|\b(?:fetch|WebSocket|XMLHttpRequest)\s*\(", page, re.I):
            raise BundleBuildError("bundle still contains resource dependencies")
        bundles[output_name] = page.encode("utf-8")
    return bundles


def bundle_matches_source() -> bool:
    """True when deploy assets and generated digest metadata are fresh."""
    output_dir = ROOT / "dist"
    bundles = build_bundle()
    for name, raw in bundles.items():
        if not (output_dir / name).is_file() or not (output_dir / (name + ".gz")).is_file():
            return False
        on_disk = (output_dir / name).read_bytes()
        if on_disk != raw:
            return False
        on_disk_bundle = (output_dir / (name + ".gz")).read_bytes()
        if on_disk_bundle != build_bundle_container(raw):
            return False
    bootstrap_paths = [path for path, _ in BOOTSTRAPS]
    bootstraps = tuple(normalize_bootstrap(path.read_bytes(), path) for path in bootstrap_paths)
    return ASSET_DIGEST_HEADER.is_file() and ASSET_DIGEST_HEADER.read_text(encoding="utf-8") == render_digest_header(bootstraps[0], bootstraps[1], bundles)


def main() -> None:
    normalized_bootstraps: dict[Path, bytes] = {}
    for bootstrap_path, max_chars in BOOTSTRAPS:
        bootstrap = normalize_bootstrap(bootstrap_path.read_bytes(), bootstrap_path)
        if len(bootstrap) > max_chars:
            message = f"{bootstrap_path.relative_to(ROOT)} exceeds {max_chars} chars: "
            message += str(len(bootstrap))
            raise BundleBuildError(message)
        if bootstrap != bootstrap_path.read_bytes():
            _ = bootstrap_path.write_bytes(bootstrap)
        normalized_bootstraps[bootstrap_path] = bootstrap
        print(
            f"{bootstrap_path.relative_to(ROOT)}: {len(bootstrap)} chars, " +
            f"sha256={hashlib.sha256(bootstrap).hexdigest()}"
        )

    output_dir = ROOT / "dist"
    output_dir.mkdir(parents=True, exist_ok=True)
    bundles = build_bundle()
    for output_name, raw in bundles.items():
        output = output_dir / output_name
        _ = output.write_bytes(raw)
        compressed = build_bundle_container(raw)
        compressed_output = output.with_suffix(output.suffix + ".gz")
        _ = compressed_output.write_bytes(compressed)
        if len(compressed) >= len(raw):
            raise BundleBuildError(f"{compressed_output.relative_to(ROOT)} is not smaller than {output.relative_to(ROOT)}")
        print(
            "".join(
                (
                    f"{output.relative_to(ROOT)}: {len(raw)} bytes, ",
                    f"{compressed_output.relative_to(ROOT)}: {len(compressed)} bytes, ",
                    f"sha256={hashlib.sha256(raw).hexdigest()}, ",
                    f"bundle_sha256={hashlib.sha256(compressed).hexdigest()}",
                )
            )
        )

    header = render_digest_header(
        normalized_bootstraps[WEB / "bootstrap.js"], normalized_bootstraps[WEB / "bootstrap-ble.js"], bundles
    )
    _ = ASSET_DIGEST_HEADER.write_text(header, encoding="utf-8")
    print(f"{ASSET_DIGEST_HEADER.relative_to(ROOT.parent)}: generated")


if __name__ == "__main__":
    _ = main()
