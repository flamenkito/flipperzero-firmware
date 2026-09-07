from __future__ import annotations

import csv
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FAP_SOURCE_DIR = ROOT / "applications_user/pocket_airbridge"
AIRBRIDGE_C_SOURCES = tuple(sorted(FAP_SOURCE_DIR.glob("*.c")))
AIRBRIDGE_TYPING_SOURCE = FAP_SOURCE_DIR / "airbridge_typing.c"
AIRBRIDGE_STREAM_SOURCE = FAP_SOURCE_DIR / "airbridge_stream.c"
AIRBRIDGE_UI_INTERNAL = FAP_SOURCE_DIR / "airbridge_ui_i.h"
AIRBRIDGE_SCREENS_SOURCE = FAP_SOURCE_DIR / "airbridge_screens.c"
APP_CONF = ROOT / "targets/f7/ble_glue/app_conf.h"


class T12StaticInvariantTest(unittest.TestCase):
    def test_product_symbols_are_not_part_of_the_firmware_sdk(self) -> None:
        with (ROOT / "targets/f7/api_symbols.csv").open(newline="") as source:
            rows = list(csv.DictReader(source))
        self.assertFalse(
            [row["name"] for row in rows if "airbridge" in row["name"].lower()]
        )
        version = next(row["name"] for row in rows if row["entry"] == "Version")
        self.assertEqual("87", version.split(".")[0])

    def test_ble_mblock_count_uses_computed_expression(self) -> None:
        source = APP_CONF.read_text(encoding="utf-8")

        self.assertNotIn("#define CFG_BLE_MBLOCK_COUNT 96", source)

    def test_deploy_is_usb_only(self) -> None:
        sources = "\n".join(
            path.read_text(encoding="utf-8") for path in AIRBRIDGE_C_SOURCES
        )

        self.assertNotIn("AirbridgeTypingTransportBle", sources)
        self.assertNotIn("bootstrap-ble.js", sources)
        self.assertNotIn("app-ble.html.gz", sources)
        self.assertNotIn("bt_airbridge_kb_report", sources)
        self.assertNotIn("airbridge_stream_step_ble", sources)

    def test_ui_owns_no_domain_module_pointer(self) -> None:
        ui_state = AIRBRIDGE_UI_INTERNAL.read_text(encoding="utf-8")

        for domain_type in (
            "AirbridgeApp*",
            "AirbridgeBle*",
            "AirbridgeConfig*",
            "AirbridgeRelay*",
            "AirbridgeStream*",
            "AirbridgeTyping*",
            "AirbridgeScreen*",
            "AirbridgeError*",
        ):
            self.assertNotIn(domain_type, ui_state)

    def test_screen_state_writes_are_confined_to_screen_policy(self) -> None:
        other_sources = "\n".join(
            source.read_text(encoding="utf-8")
            for source in AIRBRIDGE_C_SOURCES
            if source != AIRBRIDGE_SCREENS_SOURCE
        )
        screen_policy = AIRBRIDGE_SCREENS_SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("AirbridgeScreen*", other_sources)
        self.assertNotIn("->screen =", other_sources)
        self.assertNotIn("screens->current =", other_sources)
        self.assertIn("screens->current =", screen_policy)

    def test_deploy_assets_are_authenticated_before_use(self) -> None:
        typing = AIRBRIDGE_TYPING_SOURCE.read_text(encoding="utf-8")
        stream = AIRBRIDGE_STREAM_SOURCE.read_text(encoding="utf-8")

        self.assertIn("AIRBRIDGE_BOOTSTRAP_SHA256", typing)
        self.assertLess(
            typing.index("airbridge_digest_matches"),
            typing.index("for(size_t offset = 0; offset < typing->bootstrap_len"),
        )
        self.assertIn("airbridge_bundle_validate_header", stream)
        self.assertIn("AIRBRIDGE_APP_USB_BUNDLE_SHA256", stream)
        self.assertIn("AIRBRIDGE_BUNDLE_HEADER_SIZE", stream)

    def test_generated_digest_header_matches_dist_assets(self) -> None:
        """fbt does not track the generated digest header; catch drift here.

        After running build_bundle.py the FAP must be force-rebuilt
        (touch applications_user/pocket_airbridge/airbridge_assets.c) because
        scons does not rescan the generated include.
        """
        import hashlib
        import re

        header = (FAP_SOURCE_DIR / "airbridge_assets_digest.h").read_text(
            encoding="utf-8"
        )

        def header_digest(name: str) -> bytes:
            match = re.search(name + r"\[32\] = \{([^}]+)\}", header)
            self.assertIsNotNone(match, name)
            return bytes(int(token, 16) for token in match.group(1).split(","))

        bundle_path = ROOT / "airbridge/dist/app-usb.html.gz"
        bootstrap_path = ROOT / "airbridge/web/bootstrap.js"
        self.assertEqual(
            header_digest("AIRBRIDGE_APP_USB_BUNDLE_SHA256"),
            hashlib.sha256(bundle_path.read_bytes()).digest(),
        )
        self.assertEqual(
            header_digest("AIRBRIDGE_BOOTSTRAP_SHA256"),
            hashlib.sha256(bootstrap_path.read_bytes()).digest(),
        )


if __name__ == "__main__":
    unittest.main()
