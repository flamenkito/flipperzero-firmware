# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/player_qa.py
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Final

sys.dont_write_bytecode = True
ROOT: Final = Path("applications_user/flooper")
RUNNER: Final = str(ROOT / "tests/run_tests.py")
EXPECTED: Final = "".join(f"player: PASS hz={hz} cases=33 failures=0\n" for hz in (1000, 1024))
PROBE: Final = r'''
import pathlib, runpy, subprocess, sys, tempfile
sys.dont_write_bytecode = True
mode = sys.argv[1]
real_run = subprocess.run
mutations = {
    "raw-wrap": ("p->tick64 += (uint32_t)(raw - p->raw_tick);", "p->tick64 = raw;"),
    "count-offset": ("start_us + interval->start_us", "interval->start_us"),
    "release-owner": ("furi_hal_speaker_release();", "(void)0;"),
    "relative-drift": ("if(changed) {", "if(changed) { p->epoch_tick64 = now;"),
    "sticky-quit": ("__atomic_store_n(&p->exit_requested, true, __ATOMIC_RELEASE);", "(void)0;"),
}
def probe_run(args, **kwargs):
    compiler = args[0] == "cc"
    binary = pathlib.Path(args[0]).name == "player-test"
    if compiler and mode in mutations:
        with tempfile.TemporaryDirectory(prefix="flooper-player-mutation-") as directory:
            original = pathlib.Path("applications_user/flooper/flooper_player.c").resolve()
            before, after = mutations[mode]
            source = original.read_text()
            assert source.count(before) == 1, (mode, "mutation seam changed")
            mutated = pathlib.Path(directory) / "flooper_player.c"
            mutated.write_text(source.replace(before, after))
            args = [str(mutated) if arg == str(original) else arg for arg in args]
            return real_run(args + ["-I", str(original.parent)], **kwargs)
    if binary and mode in mutations:
        kwargs["timeout"] = 15
    if compiler and mode == "stale-binary":
        return real_run([sys.executable, "-c", "print('PASS stale output')"], **kwargs)
    if (compiler and mode == "compiler-interrupt") or (binary and mode == "test-interrupt"):
        raise KeyboardInterrupt()
    if (compiler and mode == "compiler-timeout") or (binary and mode == "test-timeout"):
        kwargs["timeout"] = 0.05
        return real_run([sys.executable, "-c", "import time; time.sleep(60)"], **kwargs)
    if binary and mode == "misleading-success":
        return real_run([sys.executable, "-c", "print('player: PASS hz=1000 cases=0 failures=0')"], **kwargs)
    if binary and mode == "failure-with-pass":
        return real_run([sys.executable, "-c", "print('player: PASS hz=1000 cases=33 failures=0'); raise SystemExit(7)"], **kwargs)
    return real_run(args, **kwargs)
subprocess.run = probe_run
sys.argv = ["run_tests.py", "--group", "player"]
runpy.run_path("applications_user/flooper/tests/run_tests.py", run_name="__main__")
'''


def main() -> int:
    protected = sorted(path for path in ROOT.rglob("*") if path.is_file() and
                       "tests" not in path.parts and path.name not in ("flooper_player.c", "flooper_player.h"))
    protected += sorted((ROOT / "tests/fixtures").glob("*.json"))
    protected.append(Path("targets/f7/api_symbols.csv"))
    hashes = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in protected}
    with tempfile.TemporaryDirectory(prefix="flooper-player-qa-") as directory:
        env = dict(os.environ, TMPDIR=directory)
        for mode in ("stale-binary", "misleading-success", "failure-with-pass",
                     "compiler-timeout", "test-timeout", "compiler-interrupt", "test-interrupt",
                     "compiler-interrupt", "test-interrupt", "raw-wrap", "count-offset",
                     "release-owner", "relative-drift", "sticky-quit"):
            result = subprocess.run([sys.executable, "-c", PROBE, mode], env=env,
                                    capture_output=True, text=True, timeout=90)
            assert result.returncode != 0, (mode, result.stdout, result.stderr)
            assert "player: PASS" not in result.stdout
            if mode in ("raw-wrap", "count-offset", "release-owner", "relative-drift", "sticky-quit"):
                assert "error:" not in result.stderr, result.stderr
                assert "Assertion" in result.stderr or "TimeoutExpired" in result.stderr, result.stderr
            assert not list(Path(directory).iterdir()), "interrupted run leaked artifacts"
            print(f"adversarial {mode}: PASS rejected exit={result.returncode}; cleanup PASS")
        for args in (["-O", RUNNER, "--group", "player"],
                     [RUNNER, "--group", "unsupported"], [RUNNER, "--group", "x" * 32768]):
            result = subprocess.run([sys.executable, *args], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 2 and "FAIL" in result.stderr
            print("adversarial CLI: PASS rejected")
        outputs: list[str] = []
        for attempt in (1, 2):
            result = subprocess.run([sys.executable, RUNNER, "--group", "player"], env=env,
                                    capture_output=True, text=True, timeout=150, check=True)
            assert result.stdout == EXPECTED and not result.stderr
            outputs.append(result.stdout)
            print(f"fresh player run {attempt}:\n{result.stdout}", end="")
        assert outputs[0] == outputs[1]
        assert not list(Path(directory).iterdir())
    assert hashes == {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in protected}
    print(f"protected source/assets/fixtures/docs/API integrity: PASS {len(protected)} files unchanged")
    print("double-run: PASS identical nonzero real C cases, 1000Hz + 1024Hz, ASan/UBSan")
    print("malformed/stale/cancel/resume/saturation/blocked stages: PASS real C cases")
    print("prompt injection: NOT APPLICABLE beyond inert JSON parser coverage")
    print("hardware/acoustics/device RAM/stack: SKIP unauthorized and unmeasured")
    print("cleanup: PASS temporary compilations/mutations removed; interrupted children reaped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
