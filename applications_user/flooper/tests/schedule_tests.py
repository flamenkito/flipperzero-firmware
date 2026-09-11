# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/run_tests.py --group schedule
"""Fresh sanitizer compilation of the production schedule compiler."""

from pathlib import Path
import subprocess
import tempfile
from typing import Final

ROOT: Final = Path(__file__).resolve().parents[1]


def run_schedule() -> None:
    with tempfile.TemporaryDirectory(prefix="flooper-schedule-") as directory:
        binary = str(Path(directory) / "schedule-test")
        sources = ([ROOT / "flooper_schedule.c"] + sorted(ROOT.glob("flooper_pattern*.c")) +
                   sorted(ROOT.glob("json_min*.c")) + sorted((ROOT / "tests").glob("schedule_*.c")))
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-fsanitize=address,undefined", "-g"]
        _ = subprocess.run(command + [str(p) for p in sources] + ["-lm", "-o", binary],
                           check=True, timeout=30)
        result = subprocess.run([binary, str(ROOT)], capture_output=True, text=True, check=True, timeout=5)
        assert result.stdout == "schedule: PASS cases=85 failures=0\n"
        assert result.stderr == ""
        print(result.stdout, end="")
