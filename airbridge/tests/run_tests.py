"""Run host regressions with build artifacts outside the repository."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "airbridge/tests"
FAP = ROOT / "applications_user/pocket_airbridge"


def run(*args: str | Path) -> None:
    _ = subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="airbridge-tests-") as output:
        for source in sorted(TESTS.glob("*_test.c")):
            executable = Path(output) / source.stem
            extra = (
                [FAP / "airbridge_assets.c"]
                if source.stem == "airbridge_assets_test"
                else []
            )
            if source.stem == "airbridge_usb_test":
                extra = [
                    FAP / "airbridge_usb.c",
                    "-DSTM32WB55xx",
                    "-Iairbridge/tests/platform",
                    "-I.",
                    "-Ifuri",
                    "-Itargets/furi_hal_include",
                    "-Ilib/libusb_stm32/inc",
                ]
            if source.stem == "usb_spoof_descriptor_test":
                extra = [
                    ROOT / "targets/f7/furi_hal/furi_hal_usb_spoof.c",
                    "-DSTM32WB55xx",
                    "-Iairbridge/tests/platform",
                    "-I.",
                    "-Ifuri",
                    "-Ilib",
                    "-Itargets/furi_hal_include",
                    "-Itargets/f7/furi_hal",
                    "-Ilib/libusb_stm32/inc",
                ]
            if source.stem == "airbridge_relay_usb_test":
                relay_extra = [
                    FAP / "airbridge_relay.c",
                    TESTS / "platform/furi_message_queue_host.c",
                    TESTS / "airbridge_relay_usb_stubs.c",
                    "-DSTM32WB55xx",
                    "-Iairbridge/tests/platform",
                    "-I.",
                    "-Ifuri",
                    "-Iapplications/services",
                    "-Iapplications_user/pocket_airbridge",
                    "-Itargets/furi_hal_include",
                    "-Itargets/f7/ble_glue",
                    "-Ilib/libusb_stm32/inc",
                ]
                for teardown_enabled in (1, 0):
                    variant = Path(output) / f"{source.stem}-{teardown_enabled}"
                    run(
                        "clang",
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pthread",
                        f"-DAIRBRIDGE_SAFE_TEARDOWN_ENABLED={teardown_enabled}",
                        source,
                        *relay_extra,
                        "-o",
                        variant,
                    )
                    run(variant)
                print(f"PASS {source.name}", flush=True)
                continue
            if source.stem == "usb_identity_e2e_test":
                extra = [
                    ROOT / "targets/f7/furi_hal/furi_hal_usb_spoof.c",
                    FAP / "airbridge_relay.c",
                    TESTS / "platform/furi_message_queue_host.c",
                    "-DAIRBRIDGE_SAFE_TEARDOWN_ENABLED=1",
                    "-DSTM32WB55xx",
                    "-Iairbridge/tests/platform",
                    "-Iairbridge/tests",
                    "-I.",
                    "-Ifuri",
                    "-Ilib",
                    "-Iapplications/services",
                    "-Iapplications_user/pocket_airbridge",
                    "-Itargets/furi_hal_include",
                    "-Itargets/f7/furi_hal",
                    "-Itargets/f7/ble_glue",
                    "-Ilib/libusb_stm32/inc",
                ]
            if source.stem == "airbridge_ble_test":
                extra = [
                    FAP / "airbridge_ble.c",
                    "-Iairbridge/tests/platform",
                    "-I.",
                    "-Ifuri",
                    "-Iapplications/services",
                    "-Itargets/furi_hal_include",
                    "-Itargets/f7/ble_glue",
                ]
            if source.stem == "airbridge_ble_dispatch_test":
                extra = [
                    "-D__PACKED_STRUCT=struct __attribute__((packed))",
                    "-Iairbridge/tests/platform",
                    "-I.",
                    "-Ifuri",
                    "-Ilib/mlib",
                    "-Ilib/stm32wb_copro/wpan",
                    "-Itargets/furi_hal_include",
                    "-Itargets/f7/ble_glue",
                ]
            if source.stem == "airbridge_typing_test":
                extra = [
                    FAP / "airbridge_typing.c",
                    FAP / "airbridge_assets.c",
                    FAP / "airbridge_screens.c",
                    "-DSTM32WB55xx",
                    "-Iairbridge/tests/platform",
                    "-I.",
                    "-Ifuri",
                    "-Iapplications/services",
                    "-Itargets/furi_hal_include",
                    "-Itargets/f7/ble_glue",
                    "-Ilib/libusb_stm32/inc",
                ]
            run(
                "clang",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                source,
                *extra,
                "-o",
                executable,
            )
            run(executable)
            print(f"PASS {source.name}", flush=True)
        executable = Path(output) / "input-wrap-test"
        run(
            "clang",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-DTEST_WRAP",
            FAP / "airbridge_ui_input.c",
            "-o",
            executable,
        )
        run(executable)
    run(sys.executable, "-m", "unittest", "airbridge.tests.test_t12_static_invariants")
    run(
        sys.executable,
        "-c",
        "from airbridge.tests.test_gen_identity_paths import "
        + "test_identity_generator_roots_match_merged_layout, test_serial_uuids_match_canonical_header_comments; "
        + "test_identity_generator_roots_match_merged_layout(); test_serial_uuids_match_canonical_header_comments()",
    )
    run("node", "--test", *sorted(TESTS.glob("*_test.mjs")))


if __name__ == "__main__":
    main()
