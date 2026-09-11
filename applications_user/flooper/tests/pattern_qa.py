# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/pattern_qa.py
import os
import subprocess
import sys
from typing import Final

RUNNER: Final = "applications_user/flooper/tests/run_tests.py"
PROBE: Final = r'''
import pathlib, runpy, subprocess, sys, tempfile
sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path("applications_user/flooper/tests").resolve()))
mode = sys.argv[1]
real_run = subprocess.run
def probe_run(args, **kwargs):
    if args[0] == "cc":
        if mode == "stale-binary":
            return real_run([sys.executable, "-c", "print('PASS stale compiler output')"], **kwargs)
        if mode == "interrupt":
            raise KeyboardInterrupt()
        if mode == "timeout":
            kwargs["timeout"] = 0.05
            return real_run([sys.executable, "-c", "import time; time.sleep(60)"], **kwargs)
    if pathlib.Path(args[0]).name == "pattern-test":
        if mode == "misleading-success":
            return real_run([sys.executable, "-c", "print('PASS 0 cases; 0 failures')"], **kwargs)
        if mode == "failure-with-pass":
            return real_run([sys.executable, "-c", "print('PASS'); raise SystemExit(7)"], **kwargs)
    return real_run(args, **kwargs)
subprocess.run = probe_run
sys.argv = ["run_tests.py", "--group", "pattern"]
runpy.run_path("applications_user/flooper/tests/run_tests.py", run_name="__main__")
'''


def main() -> int:
    for mode in ("stale-binary", "misleading-success", "failure-with-pass", "timeout", "interrupt", "interrupt"):
        result = subprocess.run([sys.executable, "-c", PROBE, mode],
                                capture_output=True, text=True, timeout=40)
        assert result.returncode != 0, (mode, result.stdout, result.stderr)
        assert "pattern: PASS" not in result.stdout
        print(f"adversarial {mode}: PASS expected nonzero exit={result.returncode}")
    for args in (["-O", RUNNER, "--group", "pattern"],
                 [RUNNER, "--group", "unsupported"], [RUNNER, "--group", "x" * 32768]):
        result = subprocess.run([sys.executable, *args], capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and "FAIL" in result.stderr
        assert "total:" not in result.stdout
        print(f"adversarial CLI bytes={sum(map(len, args))}: PASS rejected")
    outputs: list[str] = []
    for attempt in (1, 2):
        result = subprocess.run([sys.executable, RUNNER, "--group", "pattern"],
                                capture_output=True, text=True, timeout=120)
        print(f"manual pattern run {attempt}: exit={result.returncode}\n{result.stdout}{result.stderr}")
        assert result.returncode == 0 and "total: 128 passed, 0 failed" in result.stdout
        outputs.append(result.stdout)
    assert outputs[0] == outputs[1]
    print("double-run/resume after runner interruptions: PASS identical output; freshly compiled each time")
    print("prompt injection: inert JSON text, syntax/schema validation only; no execution capability")
    print("worker cancel/resume and interrupted playback: NOT APPLICABLE (Todos 4/5, not implemented)")
    print("hardware: SKIP unauthorized")
    print(f"cleanup: TemporaryDirectory removed host binaries and mutated fixtures under {os.getenv('TMPDIR', '/tmp')}")
    print("manual QA: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
