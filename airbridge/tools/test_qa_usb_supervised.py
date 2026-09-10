import subprocess
import unittest
from unittest import mock

from airbridge.tools import qa_usb_supervised as qa


# Minimal ioreg -p IOUSB -l -w 0 -c IOUSBHostDevice output for the case
# where the STM32 (0x0483 = 1155) device is visible — i.e. Flipper USB = ON
# or some other STM32 USB stick is attached. Each parent owns its own
# ``{ ... }`` block; ioreg only emits the requested class so the dump is
# bounded.
IOREG_WITH_STM32: str = (
    "+-o Root  <class IORegistryEntry, id 0x100000100, retain 35>\n"
    "  {\n"
    "  }\n"
    "  +-o AppleT8132USBXHCI@02100000  <class AppleT8132USBXHCI, id 0x1000004cc>\n"
    "    {\n"
    "    }\n"
    "    +-o Flipper Luwot@02100000  <class IOUSBHostDevice, id 0x1001b62c8>\n"
    "        {\n"
    '          "sessionID" = 12856298403436\n'
    '          "USBSpeed" = 1\n'
    '          "idVendor" = 1155\n'
    '          "idProduct" = 2184\n'
    '          "bDeviceClass" = 0\n'
    '          "bcdDevice" = 256\n'
    '          "bMaxPacketSize0" = 8\n'
    '          "iProduct" = 2\n'
    '          "iSerialNumber" = 3\n'
    '          "bNumConfigurations" = 1\n'
    '          "locationID" = 34603008\n'
    '          "bDeviceSubClass" = 0\n'
    '          "bcdUSB" = 512\n'
    '          "USB Product Name" = "Flipper Luwot"\n'
    '          "USB Vendor Name" = "Flipper Devices Inc."\n'
    '          "kUSBProductString" = "Flipper Luwot"\n'
    '          "kUSBVendorString" = "Flipper Devices Inc."\n'
    "        }\n"
)

# ioreg output for the same host with Flipper USB = OFF (only the Logitech
# spoofed AirBridge enumeration is present).
IOREG_WITHOUT_STM32: str = (
    "+-o Root  <class IORegistryEntry, id 0x100000100, retain 35>\n"
    "  {\n"
    "  }\n"
    "  +-o AppleT8132USBXHCI@02100000  <class AppleT8132USBXHCI, id 0x1000004cc>\n"
    "    {\n"
    "    }\n"
    "    +-o USB Keyboard@02100000  <class IOUSBHostDevice, id 0x1001b62c8>\n"
    "        {\n"
    '          "idVendor" = 1133\n'
    '          "idProduct" = 49948\n'
    '          "bDeviceClass" = 239\n'
    '          "locationID" = 34603008\n'
    "        }\n"
)


def _fake_run(stdout: str, returncode: int = 0) -> mock.Mock:
    """Return a mock that mimics ``subprocess.run`` and yields a CompletedProcess."""
    completed = subprocess.CompletedProcess(
        args=qa.IOREG_DEVICE_COMMAND, returncode=returncode,
        stdout=stdout, stderr="",
    )
    return mock.Mock(return_value=completed)


def _timeout_run(*_args: object, **_kwargs: object) -> None:
    raise subprocess.TimeoutExpired(cmd=qa.IOREG_DEVICE_COMMAND, timeout=5.0)


def _clock(values: list[float]) -> mock.Mock:
    """Return a mock that yields each value in turn then repeats the last."""
    iterator = iter(values)

    def _next() -> float:
        try:
            return next(iterator)
        except StopIteration:
            return values[-1] if values else 0.0
    return mock.Mock(side_effect=_next)


class FlipperContextsTest(unittest.TestCase):
    def test_extracts_contexts_when_stm32_visible(self) -> None:
        contexts = qa.flipper_contexts(IOREG_WITH_STM32)
        self.assertEqual(len(contexts), 1)
        self.assertIn('"idVendor" = 1155', contexts[0])

    def test_returns_empty_when_stm32_absent(self) -> None:
        self.assertEqual(qa.flipper_contexts(IOREG_WITHOUT_STM32), ())

    def test_returns_empty_for_empty_input(self) -> None:
        self.assertEqual(qa.flipper_contexts(""), ())


class Stm32VendorLineTest(unittest.TestCase):
    def test_matches_idvendor_1155_with_tree_indent(self) -> None:
        self.assertIsNotNone(qa.STM32_VENDOR_LINE.search('  |       "idVendor" = 1155'))

    def test_matches_idvendor_1155_with_no_indent(self) -> None:
        self.assertIsNotNone(qa.STM32_VENDOR_LINE.search('"idVendor" = 1155'))

    def test_does_not_match_other_vendors(self) -> None:
        self.assertIsNone(qa.STM32_VENDOR_LINE.search('  |       "idVendor" = 1133'))
        self.assertIsNone(qa.STM32_VENDOR_LINE.search('"idProduct" = 1155'))


class RunSampleTest(unittest.TestCase):
    def _patch_run(self, side_effect: object) -> mock.Mock:
        """Patch ``subprocess.run`` and return the mock for later inspection."""
        patcher = mock.patch.object(qa.subprocess, "run", side_effect=side_effect)
        self.addCleanup(patcher.stop)
        return patcher.start()

    def test_sighting_when_idvendor_1155_present(self) -> None:
        run_mock = self._patch_run(_fake_run(IOREG_WITH_STM32))
        with mock.patch.object(qa.time, "monotonic", _clock([0.0, 0.05, 0.05, 0.10, 0.55])):
            exit_code = qa.run_sample(0.5, None)
        self.assertEqual(exit_code, 2)
        self.assertTrue(run_mock.called)
        command = run_mock.call_args[0][0]
        self.assertEqual(command, qa.IOREG_DEVICE_COMMAND)

    def test_clean_when_idvendor_1155_absent(self) -> None:
        run_mock = self._patch_run(_fake_run(IOREG_WITHOUT_STM32))
        with mock.patch.object(qa.time, "monotonic", _clock([0.0, 0.05, 0.05, 0.10, 0.55])):
            exit_code = qa.run_sample(0.5, None)
        self.assertEqual(exit_code, 0)
        command = run_mock.call_args[0][0]
        self.assertEqual(command, qa.IOREG_DEVICE_COMMAND)

    def test_unknown_missed_on_timeout(self) -> None:
        run_mock = self._patch_run(_timeout_run)
        with mock.patch.object(qa.time, "monotonic", _clock([0.0, 0.05, 0.05, 5.5, 6.5, 6.5])):
            exit_code = qa.run_sample(6.0, None)
        self.assertEqual(exit_code, 0)
        command = run_mock.call_args[0][0]
        self.assertEqual(command, qa.IOREG_DEVICE_COMMAND)

    def test_sample_path_never_invokes_system_profiler(self) -> None:
        run_mock = self._patch_run(_fake_run(IOREG_WITHOUT_STM32))
        with mock.patch.object(qa.time, "monotonic", _clock([0.0, 0.05, 0.05, 0.10, 0.55])):
            _ = qa.run_sample(0.5, None)
        invoked_commands = tuple(call.args[0] for call in run_mock.call_args_list)
        self.assertTrue(invoked_commands, "expected at least one ioreg invocation")
        for command in invoked_commands:
            self.assertNotIn("system_profiler", command,
                             f"sample path must not invoke system_profiler, got {command!r}")
            self.assertEqual(command, qa.IOREG_DEVICE_COMMAND)

    def test_ioreg_device_command_is_class_filtered(self) -> None:
        self.assertEqual(
            qa.IOREG_DEVICE_COMMAND,
            ("ioreg", "-p", "IOUSB", "-l", "-w", "0", "-c", "IOUSBHostDevice"),
        )


class SampleLogOutputTest(unittest.TestCase):
    """Confirm the per-sample log format stays compatible with hil_e2e consumer."""

    def test_each_completed_sample_emits_four_record_lines(self) -> None:
        captured: dict[str, str] = {}
        run_mock = _fake_run(IOREG_WITHOUT_STM32)
        with mock.patch.object(qa.subprocess, "run", run_mock):
            with mock.patch.object(qa.time, "monotonic", _clock([0.0, 0.05, 0.05, 0.10, 0.55])):
                original_write_log = qa.write_log
                qa.write_log = lambda lines, _output: captured.setdefault("log", "\n".join(lines))
                try:
                    _ = qa.run_sample(0.5, None)
                finally:
                    qa.write_log = original_write_log
        log = captured["log"]
        self.assertIn("sample.1.start_monotonic=", log)
        self.assertIn("sample.1.end_monotonic=", log)
        self.assertIn("sample.1.status=COMPLETED", log)
        self.assertIn("sample.1.0483_verdict=CLEAN", log)
        self.assertIn("overall_0483_verdict=CLEAN", log)
        self.assertIn("notice=sampled evidence, not a formal non-enumeration proof", log)
        self.assertIn("scope=macOS IOUSB plane IOUSBHostDevice nodes via ioreg", log)


if __name__ == "__main__":
    _ = unittest.main()