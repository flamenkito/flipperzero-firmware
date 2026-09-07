import unittest

from airbridge.tools.build_bundle import ROOT, bundle_matches_source, normalize_bootstrap


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
    def test_dist_bundle_is_not_stale(self) -> None:
        self.assertTrue(
            bundle_matches_source(),
            "airbridge/dist/*.html is stale: rebuild with python3 airbridge/tools/build_bundle.py",
        )


if __name__ == "__main__":
    _ = unittest.main()
