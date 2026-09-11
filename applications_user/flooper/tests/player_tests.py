# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/run_tests.py --group player
"""Fresh real-worker compilation with deterministic pthread-backed Furi fakes."""

from pathlib import Path
import subprocess
import tempfile
import sys
from typing import Final

ROOT: Final = Path(__file__).resolve().parents[1]
EXPECTED: Final = "player: PASS hz={hz} cases=33 failures=0\n"


def run_player() -> None:
    with tempfile.TemporaryDirectory(prefix="flooper-player-") as directory:
        binary = str(Path(directory) / "player-test")
        sources = ([ROOT / "flooper.c", ROOT / "flooper_player.c", ROOT / "flooper_schedule.c"] +
                   sorted(ROOT.glob("flooper_pattern*.c")) + sorted(ROOT.glob("json_min*.c")) +
                   sorted((ROOT / "tests").glob("player_*.c")))
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-ffunction-sections", "-fdata-sections",
                   "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
                   "-fsanitize=address,undefined", "-g", "-pthread", "-I", str(ROOT / "tests/fakes")]
        _ = subprocess.run(command + [str(path) for path in sources] + ["-lm", "-o", binary],
                           check=True, timeout=30)
        outputs: list[str] = []
        for hz in (1000, 1024):
            result = subprocess.run([binary, str(ROOT), str(hz)], check=False,
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert result.stdout == EXPECTED.format(hz=hz) and not result.stderr, (result.stdout, result.stderr)
            outputs.append(result.stdout)
        print("".join(outputs), end="")
