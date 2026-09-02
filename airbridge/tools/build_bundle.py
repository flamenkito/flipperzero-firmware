#!/usr/bin/env python3
from pathlib import Path
import gzip
import hashlib
import re


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
BOOTSTRAP = WEB / "bootstrap.js"
BUNDLES = (
    ("chat-usb.html", "app-usb.html", "WebHIDAdapter"),
    ("chat-ble.html", "app-ble.html", "WebBluetoothAdapter"),
)
PROTOCOL_IMPORT = (
    "import { AirBridgeCryptoSession, buildMessage, CRYPTO_ROLE, CRYPTO_STATE, encodeMeta, ItemReceiver, ItemSender, MAX_PAYLOAD, MSG, "
    "parseMessage, sha256 } from './airbridge-protocol.js';"
)
PROTOCOL_BINDINGS = [
    "AirBridgeCryptoSession",
    "buildMessage",
    "CRYPTO_ROLE",
    "CRYPTO_STATE",
    "encodeMeta",
    "ItemReceiver",
    "ItemSender",
    "MAX_PAYLOAD",
    "MSG",
    "parseMessage",
    "sha256",
]
EVIDENCE_IMPORT = (
    "import { createTimingRecorder, recordCancel, recordFlipperCounters } from './airbridge-evidence.js';"
)
EVIDENCE_BINDINGS = [
    "createTimingRecorder",
    "recordCancel",
    "recordFlipperCounters",
]
UI_CSS_LINK = '<link rel="stylesheet" href="./airbridge-ui.css">'
UI_IMPORT_RE = re.compile(
    r"(?m)^import \{ (?P<bindings>[^}]+) \} from './airbridge-ui\.js';$"
)


def inline_module(path: Path, bindings: list[str] | None) -> str:
    source = path.read_text(encoding="utf-8")
    source = re.sub(r"(?ms)^\s*import\s+.*?;\s*\n?", "", source)
    source = re.sub(r"(?m)^(\s*)export\s+", r"\1", source)
    if bindings is None:
        return source
    names = ", ".join(bindings)
    return f"const {{ {names} }} = (() => {{\n{source}\nreturn {{ {names} }};\n}})();"


def replace_once(page: str, needle: str, replacement: str) -> str:
    if page.count(needle) != 1:
        raise ValueError(f"expected exactly one {needle!r} in chat-usb.html")
    return page.replace(needle, replacement)


def inline_ui_css(page: str) -> str:
    css = (WEB / "airbridge-ui.css").read_text(encoding="utf-8")
    return replace_once(page, UI_CSS_LINK, f"<style>\n{css}\n</style>")


def inline_ui_module(page: str) -> str:
    matches = list(UI_IMPORT_RE.finditer(page))
    if len(matches) != 1:
        raise ValueError("expected exactly one airbridge-ui.js import line")
    bindings = [part.strip() for part in matches[0].group("bindings").split(",") if part.strip()]
    if not bindings:
        raise ValueError("expected at least one UI binding")
    return page[: matches[0].start()] + inline_module(WEB / "airbridge-ui.js", bindings) + page[matches[0].end() :]


def gzip_deterministic(data: bytes) -> bytes:
    return gzip.compress(data, compresslevel=9, mtime=0)


def main() -> None:
    output_dir = ROOT / "dist"
    output_dir.mkdir(parents=True, exist_ok=True)
    for page_name, output_name, adapter_name in BUNDLES:
        page = (WEB / page_name).read_text(encoding="utf-8")
        page = inline_ui_css(page)
        page = inline_ui_module(page)
        page = replace_once(
            page,
            PROTOCOL_IMPORT,
            inline_module(WEB / "airbridge-protocol.js", PROTOCOL_BINDINGS),
        )
        page = replace_once(
            page,
            EVIDENCE_IMPORT,
            inline_module(WEB / "airbridge-evidence.js", EVIDENCE_BINDINGS),
        )
        page = replace_once(
            page,
            f"import {{ {adapter_name} }} from './airbridge-transports.js';",
            "\n".join(
                (
                    inline_module(WEB / "airbridge-identity.js", None),
                    inline_module(WEB / "airbridge-transports.js", [adapter_name]),
                ),
            ),
        )
        page = replace_once(page, '<script type="module">', "<script>")
        if 'href="./airbridge-ui.css"' in page:
            raise ValueError("bundle still contains UI stylesheet link")
        if "from './airbridge-ui.js'" in page:
            raise ValueError("bundle still contains UI module import")
        if "import " in page or 'type="module"' in page:
            raise ValueError("bundle still contains module syntax")

        output = output_dir / output_name
        raw = page.encode("utf-8")
        _ = output.write_bytes(raw)
        compressed = gzip_deterministic(raw)
        compressed_output = output.with_suffix(output.suffix + ".gz")
        _ = compressed_output.write_bytes(compressed)
        if len(compressed) >= len(raw):
            raise ValueError(f"{compressed_output.relative_to(ROOT)} is not smaller than {output.relative_to(ROOT)}")
        print(
            "".join(
                (
                    f"{output.relative_to(ROOT)}: {len(raw)} bytes, ",
                    f"{compressed_output.relative_to(ROOT)}: {len(compressed)} bytes, ",
                    f"sha256={hashlib.sha256(raw).hexdigest()}, ",
                    f"gzip_sha256={hashlib.sha256(compressed).hexdigest()}",
                )
            )
        )

    bootstrap = BOOTSTRAP.read_text(encoding="ascii")
    if len(bootstrap) > 1200:
        raise ValueError(f"{BOOTSTRAP.relative_to(ROOT)} exceeds 1200 chars: {len(bootstrap)}")
    print(f"{BOOTSTRAP.relative_to(ROOT)}: {len(bootstrap)} chars")


if __name__ == "__main__":
    main()
