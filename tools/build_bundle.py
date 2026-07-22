#!/usr/bin/env python3
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
BOOTSTRAP = WEB / "bootstrap.js"
BUNDLES = (
    ("chat-usb.html", "app-usb.html", "WebHIDAdapter"),
    ("chat-ble.html", "app-ble.html", "WebBluetoothAdapter"),
)
PROTOCOL_IMPORT = (
    "import { buildMessage, encodeMeta, ItemReceiver, ItemSender, MAX_PAYLOAD, MSG, "
    "parseMessage, sha256 } from './airbridge-protocol.js';"
)
PROTOCOL_BINDINGS = [
    "buildMessage",
    "encodeMeta",
    "ItemReceiver",
    "ItemSender",
    "MAX_PAYLOAD",
    "MSG",
    "parseMessage",
    "sha256",
]


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


def main() -> None:
    output_dir = ROOT / "dist"
    output_dir.mkdir(parents=True, exist_ok=True)
    for page_name, output_name, adapter_name in BUNDLES:
        page = (WEB / page_name).read_text(encoding="utf-8")
        page = replace_once(
            page,
            PROTOCOL_IMPORT,
            inline_module(WEB / "airbridge-protocol.js", PROTOCOL_BINDINGS),
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
        if "import " in page or 'type="module"' in page:
            raise ValueError("bundle still contains module syntax")

        output = output_dir / output_name
        _ = output.write_text(page, encoding="utf-8")
        print(f"{output.relative_to(ROOT)}: {len(page.encode('utf-8'))} bytes")

    bootstrap = BOOTSTRAP.read_text(encoding="ascii")
    if len(bootstrap) > 1200:
        raise ValueError(f"{BOOTSTRAP.relative_to(ROOT)} exceeds 1200 chars: {len(bootstrap)}")
    print(f"{BOOTSTRAP.relative_to(ROOT)}: {len(bootstrap)} chars")


if __name__ == "__main__":
    main()
