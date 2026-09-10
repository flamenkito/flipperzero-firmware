import unittest
from unittest import mock

from airbridge.tools import usb_descriptor_macos as _macos
from airbridge.tools.usb_descriptor_macos import (
    IOREG_COMMAND,
    IOREG_IOSERVICE_INTERFACES_COMMAND,
    add_interfaces,
    add_interfaces_with_fallback,
    ioreg_nodes,
    property_number,
)

# Bind through the production module object (not via
# ``airbridge.tools.usb_descriptor_fixture``) so ``except CaptureError``
# matches the same class instance that ``run_command`` raises. The
# fixture source is loaded twice under the script-style and package-style
# import paths and yields two distinct class objects otherwise.
UNKNOWN = _macos.UNKNOWN
CaptureError = _macos.CaptureError


# Minimal IOUSB-plane dump with a Logitech 046D:C31C device but ZERO
# IOUSBHostInterface nodes. Each parent owns its own "{ ... }" block
# (required by the parser's brace depth tracking).
IOUSB_PLANE_NO_INTERFACES: str = (
    "+-o Root  <class IORegistryEntry, id 0x100000100, retain 35>\n"
    "  {\n"
    "  }\n"
    "  +-o AppleT8132USBXHCI@02100000  <class AppleT8132USBXHCI, id 0x1000004cc>\n"
    "    {\n"
    "    }\n"
    "    +-o USB Keyboard@02100000  <class IOUSBHostDevice, id 0x1001b54f5>\n"
    "        {\n"
    '          "sessionID" = 16430677606234\n'
    '          "USBSpeed" = 1\n'
    '          "idProduct" = 49948\n'
    '          "iManufacturer" = 1\n'
    '          "bDeviceClass" = 239\n'
    '          "bcdDevice" = 256\n'
    '          "bMaxPacketSize0" = 8\n'
    '          "iProduct" = 2\n'
    '          "iSerialNumber" = 0\n'
    '          "bNumConfigurations" = 1\n'
    '          "locationID" = 34603008\n'
    '          "bDeviceSubClass" = 2\n'
    '          "bcdUSB" = 512\n'
    '          "USB Address" = 1\n'
    '          "kUSBCurrentConfiguration" = 1\n'
    '          "bDeviceProtocol" = 1\n'
    '          "USBPortType" = 0\n'
    '          "USB Vendor Name" = "Logitech"\n'
    '          "Device Speed" = 1\n'
    '          "idVendor" = 1133\n'
    '          "kUSBProductString" = "USB Keyboard"\n'
    '          "kUSBVendorString" = "Logitech"\n'
    '          "kUSBAddress" = 1\n'
    "        }\n"
)


# IOService-plane fallback: two pipe-prefixed IOUSBHostInterface nodes that
# share the IOUSBHostDevice's locationID (34603008).
IOSERVICE_PLANE_TWO_INTERFACES: str = (
    "+-o Root  <class IORegistryEntry, id 0x100000100, retain 35>\n"
    "  {\n"
    "  }\n"
    "    +-o IOUSBHostInterface@0  <class IOUSBHostInterface, id 0x1001b54fd>\n"
    "    | {\n"
    '    |   "USBSpeed" = 1\n'
    '    |   "iInterface" = 0\n'
    '    |   "bInterfaceProtocol" = 1\n'
    '    |   "bAlternateSetting" = 0\n'
    '    |   "idProduct" = 49948\n'
    '    |   "bcdDevice" = 256\n'
    '    |   "USB Product Name" = "USB Keyboard"\n'
    '    |   "locationID" = 34603008\n'
    '    |   "bInterfaceClass" = 3\n'
    '    |   "bInterfaceSubClass" = 1\n'
    '    |   "bConfigurationValue" = 1\n'
    '    |   "bInterfaceNumber" = 0\n'
    '    |   "USB Vendor Name" = "Logitech"\n'
    '    |   "idVendor" = 1133\n'
    '    |   "bNumEndpoints" = 1\n'
    "    | }\n"
    "    |\n"
    "    +-o IOUSBHostInterface@1  <class IOUSBHostInterface, id 0x1001b54fe>\n"
    "      {\n"
    '        "USBSpeed" = 1\n'
    '        "iInterface" = 0\n'
    '        "bInterfaceProtocol" = 0\n'
    '        "bAlternateSetting" = 0\n'
    '        "idProduct" = 49948\n'
    '        "bcdDevice" = 256\n'
    '        "USB Product Name" = "USB Keyboard"\n'
    '        "locationID" = 34603008\n'
    '        "bInterfaceClass" = 3\n'
    '        "bInterfaceSubClass" = 0\n'
    '        "bConfigurationValue" = 1\n'
    '        "bInterfaceNumber" = 1\n'
    '        "USB Vendor Name" = "Logitech"\n'
    '        "idVendor" = 1133\n'
    '        "bNumEndpoints" = 2\n'
    "      }\n"
)


class FixtureParsingTest(unittest.TestCase):
    def test_iousb_plane_parses_device_without_interfaces(self) -> None:
        nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES)
        classes = tuple(node.class_name for node in nodes)
        self.assertIn("IOUSBHostDevice", classes)
        self.assertNotIn("IOUSBHostInterface", classes)

    def test_ioservice_plane_parses_two_interface_nodes(self) -> None:
        nodes = ioreg_nodes(IOSERVICE_PLANE_TWO_INTERFACES)
        interface_nodes = tuple(node for node in nodes if node.class_name == "IOUSBHostInterface")
        self.assertEqual(len(interface_nodes), 2)
        numbers = tuple(property_number(node, "bInterfaceNumber") for node in interface_nodes)
        non_null = tuple(number for number in numbers if number is not None)
        self.assertEqual(sorted(non_null), [0, 1])
        for node, expected_class in zip(interface_nodes, (3, 3)):
            self.assertEqual(property_number(node, "bInterfaceClass"), expected_class)
        self.assertEqual(property_number(interface_nodes[0], "bInterfaceSubClass"), 1)
        self.assertEqual(property_number(interface_nodes[1], "bInterfaceSubClass"), 0)
        self.assertEqual(property_number(interface_nodes[0], "locationID"), 34603008)


class PrimaryInterfacePathTest(unittest.TestCase):
    def test_add_interfaces_uses_primary_nodes_when_present(self) -> None:
        nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES + IOSERVICE_PLANE_TWO_INTERFACES)
        device = next(node for node in nodes if node.class_name == "IOUSBHostDevice")
        values: dict[str, str] = {}
        add_interfaces_with_fallback(values, nodes, device)
        self.assertEqual(values["interfaces.count"], "2")
        self.assertEqual(values["interface.00.class"], "0x03")
        self.assertEqual(values["interface.01.class"], "0x03")
        self.assertEqual(values["interface.00.subclass"], "0x01")
        self.assertEqual(values["interface.01.subclass"], "0x00")
        self.assertEqual(values["interface.00.protocol"], "0x01")
        self.assertEqual(values["interface.01.protocol"], "0x00")
        self.assertEqual(values["interface.00.number"], "0")
        self.assertEqual(values["interface.01.number"], "1")
        self.assertEqual(values["interface.00.endpoint_count"], "0")
        self.assertEqual(values["interface.00.hid_report_descriptor"], UNKNOWN)


class FallbackActivationTest(unittest.TestCase):
    def test_fallback_populates_interfaces_when_iousb_plane_is_empty(self) -> None:
        primary_nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES)
        device = next(node for node in primary_nodes if node.class_name == "IOUSBHostDevice")

        with mock.patch(
            "airbridge.tools.usb_descriptor_macos.run_command",
            return_value=IOSERVICE_PLANE_TWO_INTERFACES,
        ) as patched:
            values: dict[str, str] = {}
            add_interfaces_with_fallback(values, primary_nodes, device)

        self.assertEqual(values["interfaces.count"], "2")
        self.assertEqual(values["interface.00.class"], "0x03")
        self.assertEqual(values["interface.00.subclass"], "0x01")
        self.assertEqual(values["interface.00.protocol"], "0x01")
        self.assertEqual(values["interface.00.number"], "0")
        self.assertEqual(values["interface.01.class"], "0x03")
        self.assertEqual(values["interface.01.subclass"], "0x00")
        self.assertEqual(values["interface.01.protocol"], "0x00")
        self.assertEqual(values["interface.01.number"], "1")
        self.assertEqual(values["interface.00.endpoint_count"], "0")
        self.assertEqual(values["interface.00.hid_report_descriptor"], UNKNOWN)
        patched.assert_called_once_with(IOREG_IOSERVICE_INTERFACES_COMMAND)

    def test_fallback_does_not_run_when_primary_already_has_interfaces(self) -> None:
        nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES + IOSERVICE_PLANE_TWO_INTERFACES)
        device = next(node for node in nodes if node.class_name == "IOUSBHostDevice")
        with mock.patch(
            "airbridge.tools.usb_descriptor_macos.run_command",
        ) as patched:
            values: dict[str, str] = {}
            add_interfaces_with_fallback(values, nodes, device)
        self.assertEqual(values["interfaces.count"], "2")
        patched.assert_not_called()

    def test_fallback_command_failure_does_not_raise(self) -> None:
        primary_nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES)
        device = next(node for node in primary_nodes if node.class_name == "IOUSBHostDevice")

        def boom(command: tuple[str, ...]) -> str:
            raise CaptureError(f"{command} failed")

        with mock.patch(
            "airbridge.tools.usb_descriptor_macos.run_command",
            side_effect=boom,
        ):
            values = {"seed": "kept"}
            add_interfaces_with_fallback(values, primary_nodes, device)
        self.assertEqual(values["interfaces.count"], UNKNOWN)
        self.assertEqual(values["seed"], "kept")

    def test_fallback_returns_no_interfaces_when_ioservice_has_no_matching_nodes(self) -> None:
        primary_nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES)
        device = next(node for node in primary_nodes if node.class_name == "IOUSBHostDevice")
        ioservice_no_match = (
            "+-o Root  <class IORegistryEntry, id 0x100000100, retain 35>\n"
            "  |\n"
            "    +-o IOUSBHostInterface@9  <class IOUSBHostInterface, id 0x1001b54ff>\n"
            "    | {\n"
            '    |   "idVendor" = 1133\n'
            '    |   "idProduct" = 49948\n'
            '    |   "locationID" = 0\n'
            '    |   "bInterfaceClass" = 3\n'
            '    |   "bInterfaceSubClass" = 1\n'
            '    |   "bInterfaceProtocol" = 2\n'
            '    |   "bInterfaceNumber" = 9\n'
            "    | }\n"
        )
        with mock.patch(
            "airbridge.tools.usb_descriptor_macos.run_command",
            return_value=ioservice_no_match,
        ):
            values: dict[str, str] = {}
            add_interfaces_with_fallback(values, primary_nodes, device)
        self.assertEqual(values["interfaces.count"], UNKNOWN)


class PureAddInterfacesRegressionTest(unittest.TestCase):
    """Confirm the pre-fallback ``add_interfaces`` semantics are untouched."""

    def test_add_interfaces_marks_unknown_when_no_matching_interfaces(self) -> None:
        nodes = ioreg_nodes(IOUSB_PLANE_NO_INTERFACES)
        device = next(node for node in nodes if node.class_name == "IOUSBHostDevice")
        values: dict[str, str] = {}
        add_interfaces(values, nodes, device)
        self.assertEqual(values["interfaces.count"], UNKNOWN)

    def test_ioreg_command_tuple_is_unchanged(self) -> None:
        self.assertEqual(IOREG_COMMAND, ("ioreg", "-p", "IOUSB", "-l", "-w", "0"))

    def test_ioservice_fallback_command_includes_class_filter(self) -> None:
        self.assertEqual(
            IOREG_IOSERVICE_INTERFACES_COMMAND,
            ("ioreg", "-p", "IOService", "-l", "-w", "0", "-c", "IOUSBHostInterface"),
        )


if __name__ == "__main__":
    _ = unittest.main()