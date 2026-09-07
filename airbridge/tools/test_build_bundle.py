import unittest
import hashlib

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
    def test_only_usb_deploy_assets_are_built(self) -> None:
        self.assertEqual(BOOTSTRAPS, ((ROOT / "web" / "bootstrap.js", 1200),))
        self.assertEqual(BUNDLES, (("chat-usb.html", "app-usb.html", "WebHIDAdapter"),))

    def test_dist_bundle_is_not_stale(self) -> None:
        self.assertTrue(
            bundle_matches_source(),
            "airbridge/dist/*.html is stale: rebuild with python3 airbridge/tools/build_bundle.py",
        )

    def test_generated_digest_header_is_not_stale(self) -> None:
        bootstrap = normalize_bootstrap((ROOT / "web" / "bootstrap.js").read_bytes(), ROOT / "web" / "bootstrap.js")
        bundles = build_bundle()
        self.assertEqual(ASSET_DIGEST_HEADER.read_text(encoding="utf-8"), render_digest_header(bootstrap, bundles))


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


if __name__ == "__main__":
    _ = unittest.main()
