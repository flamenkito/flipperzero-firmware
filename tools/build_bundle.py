#!/usr/bin/env python3
"""Build the standalone USB chat page without third-party dependencies."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
OUTPUT = ROOT / "dist" / "app-usb.html"
BOOTSTRAP = WEB / "bootstrap.js"


def inline_module(path: Path, bindings: list[str]) -> str:
    source = path.read_text(encoding="utf-8")
    source = re.sub(r"(?ms)^\s*import\s+.*?;\s*\n?", "", source)
    source = re.sub(r"(?m)^(\s*)export\s+", r"\1", source)
    names = ", ".join(bindings)
    return f"const {{ {names} }} = (() => {{\n{source}\nreturn {{ {names} }};\n}})();"


def replace_once(page: str, needle: str, replacement: str) -> str:
    if page.count(needle) != 1:
        raise ValueError(f"expected exactly one {needle!r} in chat-usb.html")
    return page.replace(needle, replacement)


def main() -> None:
    page = (WEB / "chat-usb.html").read_text(encoding="utf-8")
    page = replace_once(
        page,
        "import { buildMessage, encodeMeta, ItemReceiver, ItemSender, MAX_PAYLOAD, MSG, parseMessage, sha256 } from './airbridge-protocol.js';",
        inline_module(
            WEB / "airbridge-protocol.js",
            ["buildMessage", "encodeMeta", "ItemReceiver", "ItemSender", "MAX_PAYLOAD", "MSG", "parseMessage", "sha256"],
        ),
    )
    page = replace_once(
        page,
        "import { WebHIDAdapter } from './airbridge-transports.js';",
        inline_module(WEB / "airbridge-transports.js", ["WebHIDAdapter"]),
    )
    page = replace_once(page, '<script type="module">', "<script>")
    if "import " in page or 'type="module"' in page:
        raise ValueError("bundle still contains module syntax")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    _ = OUTPUT.write_text(page, encoding="utf-8")
    print(f"{OUTPUT.relative_to(ROOT)}: {len(page.encode('utf-8'))} bytes")

    bootstrap = BOOTSTRAP.read_text(encoding="ascii")
    if len(bootstrap) > 1200:
        raise ValueError(f"{BOOTSTRAP.relative_to(ROOT)} exceeds 1200 chars: {len(bootstrap)}")
    print(f"{BOOTSTRAP.relative_to(ROOT)}: {len(bootstrap)} chars")


if __name__ == "__main__":
    main()
