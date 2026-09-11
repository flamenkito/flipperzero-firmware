# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/run_tests.py --group ui
from pathlib import Path
import subprocess
import tempfile
from typing import Final

ROOT: Final = Path(__file__).resolve().parents[1]
EXPECTED: Final = {"ui": "ui: PASS cases=32 failures=0\n",
                   "adapters": "adapters: PASS cases=14 failures=0\nadapters-path: PASS cases=4 failures=0\n"}


def run_ui(group: str) -> None:
    assert group in EXPECTED
    core = ([ROOT / "flooper_schedule.c"] + sorted(ROOT.glob("flooper_pattern*.c")) +
            sorted(ROOT.glob("json_min*.c")) + [ROOT / "tests/player_fake.c", ROOT / "tests/player_io.c"])
    flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
             "-fsanitize=address,undefined", "-pthread", "-I", str(ROOT / "tests/fakes")]
    outputs: list[str] = []
    for _attempt in range(2):
        with tempfile.TemporaryDirectory(prefix=f"flooper-{group}-") as directory:
            binary = str(Path(directory) / f"{group}-test")
            sources = core + [ROOT / "flooper_ui.c", ROOT / "flooper_player.c"] + sorted((ROOT / "tests").glob("ui_*.c"))
            _ = subprocess.run(flags + [str(p) for p in sources] + ["-lm", "-o", binary], check=True, timeout=30)
            result = subprocess.run([binary, str(ROOT), group], capture_output=True, text=True, timeout=15)
            assert result.returncode == 0 and not result.stderr, (result.returncode, result.stdout, result.stderr)
            output = result.stdout
            if group == "adapters":
                path_binary = str(Path(directory) / "adapters-path-test")
                _ = subprocess.run(flags + [str(p) for p in core] + [str(ROOT / "tests/adapters_path.c"), "-lm", "-o", path_binary], check=True, timeout=30)
                result = subprocess.run([path_binary], capture_output=True, text=True, timeout=10)
                assert result.returncode == 0 and not result.stderr, (result.returncode, result.stdout, result.stderr)
                output += result.stdout
            assert output == EXPECTED[group], output
            outputs.append(output)
    assert outputs[0] == outputs[1]
    print(outputs[0], end="")
