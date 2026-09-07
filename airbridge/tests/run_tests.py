"""Run host regressions with build artifacts outside the repository."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "airbridge/tests"
FAP = ROOT / "applications_user/pocket_airbridge"


def run(*args: str | Path) -> None:
    subprocess.run([str(arg) for arg in args], cwd=ROOT, check=True)


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
                    "-Itargets/furi_hal_include",
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
        "test_identity_generator_roots_match_merged_layout, test_serial_uuids_match_canonical_header_comments; "
        "test_identity_generator_roots_match_merged_layout(); test_serial_uuids_match_canonical_header_comments()",
    )
    run("node", "--test", *sorted(TESTS.glob("*_test.mjs")))


if __name__ == "__main__":
    main()
