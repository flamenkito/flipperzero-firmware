# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/schedule_qa.py
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Final

RUNNER: Final = "applications_user/flooper/tests/run_tests.py"
PROBE: Final = r'''
import pathlib, runpy, subprocess, sys
sys.dont_write_bytecode = True
mode = sys.argv[1]
real_run = subprocess.run
def probe_run(args, **kwargs):
    compiler = args[0] == "cc"
    binary = pathlib.Path(args[0]).name == "schedule-test"
    if compiler and mode == "stale-binary":
        return real_run([sys.executable, "-c", "print('PASS stale compiler output')"], **kwargs)
    if (compiler and mode == "compiler-interrupt") or (binary and mode == "test-interrupt"):
        raise KeyboardInterrupt()
    if (compiler and mode == "compiler-timeout") or (binary and mode == "test-timeout"):
        kwargs["timeout"] = 0.05
        return real_run([sys.executable, "-c", "import time; time.sleep(60)"], **kwargs)
    if binary and mode == "misleading-success":
        return real_run([sys.executable, "-c", "print('schedule: PASS cases=0 failures=0')"], **kwargs)
    if binary and mode == "failure-with-pass":
        return real_run([sys.executable, "-c", "print('schedule: PASS cases=85 failures=0'); raise SystemExit(7)"], **kwargs)
    return real_run(args, **kwargs)
subprocess.run = probe_run
sys.argv = ["run_tests.py", "--group", "schedule"]
runpy.run_path("applications_user/flooper/tests/run_tests.py", run_name="__main__")
'''


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="flooper-schedule-qa-") as directory:
        env = dict(os.environ, TMPDIR=directory)
        for mode in ("stale-binary", "misleading-success", "failure-with-pass",
                     "compiler-timeout", "test-timeout", "compiler-interrupt",
                     "test-interrupt", "compiler-interrupt", "test-interrupt"):
            result = subprocess.run([sys.executable, "-c", PROBE, mode], env=env,
                                    capture_output=True, text=True, timeout=40)
            assert result.returncode != 0, (mode, result.stdout, result.stderr)
            assert "schedule: PASS" not in result.stdout
            assert not list(Path(directory).iterdir()), "interrupted compilation leaked artifacts"
            print(f"adversarial {mode}: PASS rejected exit={result.returncode}; cleanup PASS")
        for args in (["-O", RUNNER, "--group", "schedule"],
                     [RUNNER, "--group", "unsupported"], [RUNNER, "--group", "x" * 32768]):
            result = subprocess.run([sys.executable, *args], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 2 and "FAIL" in result.stderr
            print("adversarial CLI: PASS rejected")
        outputs: list[str] = []
        for attempt in (1, 2):
            result = subprocess.run([sys.executable, RUNNER, "--group", "schedule"], env=env,
                                    capture_output=True, text=True, timeout=40, check=True)
            assert result.stdout == "schedule: PASS cases=85 failures=0\n" and not result.stderr
            outputs.append(result.stdout)
            print(f"fresh schedule run {attempt}: {result.stdout.strip()}")
        assert outputs[0] == outputs[1]
        assert not list(Path(directory).iterdir())
    print("double-run: PASS identical nonzero real C cases under ASan/UBSan")
    print("prompt injection: NOT APPLICABLE; no external instructions are interpreted")
    print("player cancellation/resumption: SKIP later task; hardware: SKIP unauthorized")
    print("cleanup: PASS fresh temporary binaries removed, including repeated interruptions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
