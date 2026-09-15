import unittest
import hashlib
import gzip
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from unittest.mock import patch

from airbridge.tools import build_bundle as builder

from airbridge.tools.build_bundle import (
    ASSET_DIGEST_HEADER,
    BOOTSTRAPS,
    BUNDLES,
    BUNDLE_FORMAT_VERSION,
    BUNDLE_MAGIC,
    BundleFormatError,
    asset_digest_matches,
    build_bundle,
    build_bundle_container,
    bundle_matches_source,
    decode_bundle_container,
    normalize_bootstrap,
    render_digest_header,
    ROOT,
)


class NormalizeBootstrapTest(unittest.TestCase):
    def test_normalizes_crlf_and_trailing_newlines(self) -> None:
        path = ROOT / "web" / "fixture.js"
        self.assertEqual(normalize_bootstrap(b"one\r\n", path), b"one")

    def test_reports_non_ascii_offset_and_byte(self) -> None:
        path = ROOT / "web" / "fixture.js"
        with self.assertRaisesRegex(ValueError, r"0x80 .* offset 2"):
            _ = normalize_bootstrap(b"ab\x80", path)

    def test_usb_bootstrap_sends_exact_deploy_request(self) -> None:
        bootstrap = (ROOT / "web" / "bootstrap.js").read_text()
        self.assertIn("new V([66])", bootstrap)
        self.assertNotIn("new V(64);r[0]=66", bootstrap)


class BundleMatchesSourceTest(unittest.TestCase):
    def test_usb_and_ble_deploy_assets_are_built(self) -> None:
        self.assertEqual(
            BOOTSTRAPS,
            ((ROOT / "web" / "bootstrap.js", 4096), (ROOT / "web" / "bootstrap-ble.js", 16 * 1024)),
        )
        self.assertEqual(
            BUNDLES,
            (
                ("chat-usb.html", "app-usb.html", "WebHIDAdapter"),
                ("chat-ble.html", "app-ble.html", "WebBluetoothAdapter"),
            ),
        )

    def test_dist_bundle_is_not_stale(self) -> None:
        self.assertTrue(
            bundle_matches_source(),
            "airbridge/dist/*.html is stale: rebuild with python3 airbridge/tools/build_bundle.py",
        )

    def test_generated_digest_header_is_not_stale(self) -> None:
        bootstrap = normalize_bootstrap((ROOT / "web" / "bootstrap.js").read_bytes(), ROOT / "web" / "bootstrap.js")
        bootstrap_ble = normalize_bootstrap(
            (ROOT / "web" / "bootstrap-ble.js").read_bytes(), ROOT / "web" / "bootstrap-ble.js"
        )
        bundles = build_bundle()
        self.assertEqual(
            ASSET_DIGEST_HEADER.read_text(encoding="utf-8"),
            render_digest_header(bootstrap, bootstrap_ble, bundles),
        )


class AssetAuthenticationTest(unittest.TestCase):
    def test_tampered_bootstrap_is_rejected(self) -> None:
        bootstrap = normalize_bootstrap((ROOT / "web" / "bootstrap.js").read_bytes(), ROOT / "web" / "bootstrap.js")
        expected = hashlib.sha256(bootstrap).digest()

        self.assertFalse(asset_digest_matches(bootstrap + b" ", expected))

    def test_bundle_with_wrong_magic_is_rejected(self) -> None:
        container = bytearray(build_bundle_container(b"<html></html>"))
        container[0] ^= 0xFF

        with self.assertRaises(BundleFormatError):
            _ = decode_bundle_container(bytes(container))

    def test_bundle_with_wrong_version_is_rejected(self) -> None:
        container = bytearray(build_bundle_container(b"<html></html>"))
        container[len(BUNDLE_MAGIC)] = BUNDLE_FORMAT_VERSION + 1

        with self.assertRaises(BundleFormatError):
            _ = decode_bundle_container(bytes(container))


class BundleIntegrationTest(unittest.TestCase):
    def test_shipping_bundle_excludes_legacy_whole_item_protocol(self) -> None:
        page = build_bundle()["app-usb.html"]
        container = build_bundle_container(page)
        inflated = decode_bundle_container(container)
        forbidden = (
            b"class LegacyItemReceiver",
            b"async encryptItem(",
            b"async decryptItem(",
            b"META size exceeds 4 MiB",
        )
        for marker in forbidden:
            with self.subTest(marker=marker.decode()):
                self.assertNotIn(marker, page)
                self.assertNotIn(marker, inflated)

    def test_classic_graph_when_current_runtime_is_bundled(self) -> None:
        # Given the finalized USB module graph; When bundled in memory.
        page = build_bundle()["app-usb.html"].decode()
        # Then each dependency occurs once in the canonical order, with valid JS.
        expected = (
            "vendor/js-sha256-0.11.1.js", "airbridge-incremental-sha256.js",
            "airbridge-protocol.js", "airbridge-receive-accumulator.js",
            "airbridge-item-receiver.js", "airbridge-ui.js", "airbridge-file-pass.js",
            "airbridge-outbound.js", "airbridge-chat-outbound.js", "airbridge-chat-receive.js",
            "airbridge-mock-stream-peer.js", "airbridge-chat-mock.js", "airbridge-evidence.js",
            "airbridge-identity.js", "airbridge-transports.js",
            "airbridge-terminal.js", "airbridge-chat-page.js",
        )
        markers = re.findall(r'/\* bundled: ([\w./-]+) \*/', page)
        self.assertEqual(tuple(markers), expected)
        self.assertNotRegex(page, r'(?m)<script[^>]+src=|type=[\"\']module|\bimport\s*[(\{*]|^\s*export\s')
        self.assertNotRegex(page, r'(?:src|href)=[\"\'](?!data:)|@import|\b(?:fetch|WebSocket|XMLHttpRequest)\s*\(')
        for script in re.finditer(r'<script>(.*?)</script>', page, re.S):
            result = subprocess.run(["node", "--check"], input=script[1], text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_invalid_graph_when_inputs_are_corrupted(self) -> None:
        # Given isolated real inputs with one fault each (never mutate runtime).
        faults = (
            ("chat-usb.html", '<script src="./vendor/js-sha256-0.11.1.js"></script>', ""),
            ("chat-usb.html", '<script src="./vendor/js-sha256-0.11.1.js"></script>', '<script src="./vendor/js-sha256-0.11.1.js"></script>' * 2),
            ("chat-usb.html", "</head>", '<script src="https://example.invalid/a.js"></script></head>'),
            ("chat-usb.html", "</head>", '<script src=remote.js></script></head>'),
            ("chat-usb.html", "</head>", '<link rel=stylesheet href=remote.css></head>'),
            ("chat-usb.html", "</head>", '<script type=module></script></head>'),
            ("chat-usb.html", "</head>", '<script type="module">import("./missing.js")</script></head>'),
            ("chat-usb.html", "</head>", '<link rel="stylesheet" href="remote.css"></head>'),
            ("chat-usb.html", "</head>", '<script>fetch("https://example.invalid")</script></head>'),
            ("airbridge-outbound.js", "import { FileHashPass }", "import { MissingBinding }"),
            ("airbridge-outbound.js", "./airbridge-file-pass.js", "./missing.js"),
            ("airbridge-outbound.js", "import { FileHashPass } from './airbridge-file-pass.js';", "import { FileHashPass } from './airbridge-file-pass.js';" * 2),
            ("airbridge-outbound.js", "export class OutboundTransfer", "export default class OutboundTransfer"),
        )
        for name, needle, replacement in faults:
            with self.subTest(name=name, replacement=replacement), tempfile.TemporaryDirectory() as tmp:
                web = Path(tmp) / "web"
                _ = shutil.copytree(builder.WEB, web)
                path = web / name
                _ = path.write_text(path.read_text().replace(needle, replacement))
                # When building; Then reject rather than publish unresolved code.
                with patch.object(builder, "WEB", web), self.assertRaises(builder.BundleBuildError):
                    _ = build_bundle()

    def test_container_and_digests_when_outputs_are_generated(self) -> None:
        # Given current generated bytes; When independently decoding the container.
        for output_name in ("app-usb.html", "app-ble.html"):
            with self.subTest(bundle=output_name):
                raw = (ROOT / f"dist/{output_name}").read_bytes()
                container = (ROOT / f"dist/{output_name}.gz").read_bytes()
                inflated = gzip.decompress(container[12:])
                # Then header, deterministic gzip and digest values match actual bytes.
                self.assertEqual(container[:12], b"ABND\x01\0\0\0" + len(raw).to_bytes(4, "little"))
                self.assertEqual(inflated, raw)
                self.assertEqual(container[16:20], b"\0" * 4)
                self.assertEqual(container[21], 255)
                self.assertEqual(build_bundle_container(raw), container)
        header = ASSET_DIGEST_HEADER.read_text()
        for name, data in (
            ("BOOTSTRAP", (ROOT / "web/bootstrap.js").read_bytes()),
            ("BOOTSTRAP_BLE", (ROOT / "web/bootstrap-ble.js").read_bytes()),
            ("APP_USB_BUNDLE", (ROOT / "dist/app-usb.html.gz").read_bytes()),
            ("APP_BLE_BUNDLE", (ROOT / "dist/app-ble.html.gz").read_bytes()),
        ):
            with self.subTest(pin=name):
                match = re.search(rf'AIRBRIDGE_{name}_SHA256\[32\] = \{{([^}}]+)\}}', header)
                self.assertIsNotNone(match)
                if match is None:
                    self.fail("missing digest array")
                actual = bytes(int(value.strip(), 16) for value in match[1].split(","))
                self.assertEqual(actual, hashlib.sha256(data).digest())
        for name, output_name in (("USB", "app-usb.html"), ("BLE", "app-ble.html")):
            with self.subTest(size=name):
                match = re.search(rf"AIRBRIDGE_APP_{name}_DECOMPRESSED_SIZE (\d+)U", header)
                self.assertIsNotNone(match)
                if match is None:
                    self.fail("missing decompressed size")
                self.assertEqual(int(match[1]), len((ROOT / f"dist/{output_name}").read_bytes()))
        self.assertEqual(
            {p.name for p in (ROOT / "dist").iterdir()},
            {"app-usb.html", "app-usb.html.gz", "app-ble.html", "app-ble.html.gz"},
        )
        self.assertTrue((ROOT / "web/bootstrap-ble.js").is_file())

    def test_freshness_is_false_when_an_artifact_is_missing_or_stale(self) -> None:
        # Given a fresh isolated output set, including interrupted-write fixtures.
        for name in ("app-usb.html", "app-usb.html.gz", "app-ble.html", "app-ble.html.gz", "digest.h"):
            for payload in (None, b"interrupted"):
                with self.subTest(name=name, payload=payload), tempfile.TemporaryDirectory() as tmp:
                    root = Path(tmp)
                    _ = shutil.copytree(ROOT / "dist", root / "dist")
                    header = root / "digest.h"
                    _ = shutil.copyfile(ASSET_DIGEST_HEADER, header)
                    target = header if name == "digest.h" else root / "dist" / name
                    if payload is None:
                        target.unlink()
                    else:
                        _ = target.write_bytes(payload)
                    # When checking freshness; Then return false, not success or an IO exception.
                    with patch.object(builder, "ROOT", root), patch.object(builder, "ASSET_DIGEST_HEADER", header):
                        self.assertFalse(bundle_matches_source())


if __name__ == "__main__":
    _ = unittest.main()
