from __future__ import annotations

import csv
import sys
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

    def test_ble_deploy_restoration_is_present(self) -> None:
        sources = "\n".join(
            path.read_text(encoding="utf-8") for path in AIRBRIDGE_C_SOURCES
        )
        types = (FAP_SOURCE_DIR / "airbridge_types.h").read_text(encoding="utf-8")
        profile = (FAP_SOURCE_DIR / "airbridge_profile.c").read_text(encoding="utf-8")

        self.assertIn("AirbridgeTypingTransport", types)
        self.assertIn("AirbridgeTypingTransportBle", sources)
        self.assertIn("bootstrap-ble.js", sources)
        self.assertIn("app-ble.html.gz", sources)
        self.assertIn("airbridge_stream_step_ble", sources)
        self.assertIn("ble_svc_hid_start", profile)
        self.assertIn("airbridge_profile_kb_report", profile)

    def test_dual_deploy_digest_pins_are_present(self) -> None:
        header = (FAP_SOURCE_DIR / "airbridge_assets_digest.h").read_text(
            encoding="utf-8"
        )

        for pin in (
            "AIRBRIDGE_BOOTSTRAP_SHA256",
            "AIRBRIDGE_BOOTSTRAP_BLE_SHA256",
            "AIRBRIDGE_APP_USB_BUNDLE_SHA256",
            "AIRBRIDGE_APP_BLE_BUNDLE_SHA256",
        ):
            self.assertIn(pin, header)
        self.assertTrue((ROOT / "airbridge/web/bootstrap-ble.js").is_file())

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

    def test_ioreg_nodes_parses_pipe_prefixed_property_lines(self) -> None:
        """ioreg indents property lines with ``| `` when the device's parent
        controller is not the last sibling. The sample below places the
        Logitech target under the first of two XHCI siblings (pipe-prefixed)
        and the Realtek device under the last (unprefixed); both must parse
        so ``selected_ioreg_device`` can resolve VID:PID 0x046D:0xC31C.
        """
        if str(ROOT / "airbridge") not in sys.path:
            sys.path.insert(0, str(ROOT / "airbridge"))
        from tools.usb_descriptor_fixture import Target
        from tools.usb_descriptor_macos import (
            PROPERTY_LINE,
            ioreg_nodes,
            selected_ioreg_device,
        )

        sample = (
            "  +-o AppleT8132USBXHCI@02000000  <class AppleT8132USBXHCI, id 0x1000004cc>\n"
            "  | {\n"
            "  |   \"IOClass\" = \"AppleT8132USBXHCI\"\n"
            "  | }\n"
            "  | \n"
            "  | +-o USB Keyboard@02100000  <class IOUSBHostDevice, id 0x1001b54f5>\n"
            "  |     {\n"
            "  |       \"idProduct\" = 49948\n"
            "  |       \"idVendor\" = 1133\n"
            "  |       \"kUSBProductString\" = \"USB Keyboard\"\n"
            "  |       \"kUSBVendorString\" = \"Logitech\"\n"
            "  |       \"locationID\" = 34603008\n"
            "  |     }\n"
            "  |     \n"
            "  +-o AppleT8132USBXHCI@01000000  <class AppleT8132USBXHCI, id 0x1000004d4>\n"
            "    | {\n"
            "    |   \"IOClass\" = \"AppleT8132USBXHCI\"\n"
            "    | }\n"
            "    | \n"
            "    +-o HID Device@01100000  <class IOUSBHostDevice, id 0x1000a0155>\n"
            "        {\n"
            "          \"idProduct\" = 4352\n"
            "          \"idVendor\" = 3034\n"
            "          \"kUSBProductString\" = \"HID Device\"\n"
            "          \"kUSBVendorString\" = \"Realtek\"\n"
            "          \"locationID\" = 1114112\n"
            "        }\n"
        )

        nodes = ioreg_nodes(sample)
        usb_hosts = [n for n in nodes if n.class_name == "IOUSBHostDevice"]
        self.assertEqual(
            len(usb_hosts), 2,
            "expected exactly two IOUSBHostDevice nodes (Logitech + Realtek)",
        )

        logitech = [
            n for n in usb_hosts
            if any(k == "kUSBVendorString" and "Logitech" in v for k, v in n.properties)
        ]
        realtek = [
            n for n in usb_hosts
            if any(k == "kUSBVendorString" and "Realtek" in v for k, v in n.properties)
        ]
        self.assertEqual(len(logitech), 1, "exactly one Logitech node expected")
        self.assertEqual(len(realtek), 1, "exactly one Realtek node expected")
        logitech_node = logitech[0]
        realtek_node = realtek[0]

        parsed_keys = {key for key, _ in logitech_node.properties}
        for required in ("idVendor", "idProduct", "kUSBProductString", "kUSBVendorString", "locationID"):
            self.assertIn(required, parsed_keys, f"missing {required} on pipe-prefixed Logitech node")
        self.assertEqual(
            {key for key, _ in realtek_node.properties},
            parsed_keys,
            "Realtek and Logitech nodes must expose the same key set",
        )

        device = selected_ioreg_device(nodes, Target(0x046D, 0xC31C, None))
        self.assertIs(
            device, logitech_node,
            "selected_ioreg_device must resolve to the pipe-prefixed Logitech node",
        )

        self.assertIsNone(PROPERTY_LINE.match("  | +-o Foo@0  <class Bar>"))
        self.assertIsNone(PROPERTY_LINE.match("  | }"))
        self.assertIsNotNone(PROPERTY_LINE.match("  |     \"foo\" = 1"))
        self.assertIsNotNone(PROPERTY_LINE.match("  \"foo\" = 1"))


if __name__ == "__main__":
    unittest.main()
