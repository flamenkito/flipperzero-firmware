# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/todo5_qa.py
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Final

sys.dont_write_bytecode = True
ROOT: Final = Path("applications_user/flooper")
RUNNER: Final = str(ROOT / "tests/run_tests.py")
EVIDENCE: Final = Path(".omo/evidence/flooper")
ANCHOR: Final = EVIDENCE / "todo-3-package-fix-before.sha256"
BASELINE: Final = EVIDENCE / "protected-baseline.sha256"
ANCHOR_DIGEST: Final = "83221f035a7b3543b156bb3dd2b5d2e804a4a79bc4cbb89bbc157701813eeab7"
AUTHORIZED: Final = frozenset(ROOT / name for name in (
    "flooper_player.c", "flooper_player.h", "flooper.c", "flooper_ui.c", "flooper_ui.h",
    "README.md", "ARCHITECTURE.md"))
PROBE: Final = r'''
import pathlib, runpy, subprocess, sys
sys.dont_write_bytecode = True
mode, group = sys.argv[1:]
real_run = subprocess.run
def probe_run(args, **kwargs):
    compiler = args[0] == "cc"
    binary = pathlib.Path(args[0]).name.endswith("-test")
    if compiler and mode == "stale-binary":
        return real_run([sys.executable, "-c", "print('PASS stale output')"], **kwargs)
    if (compiler and mode == "compiler-interrupt") or (binary and mode == "test-interrupt"):
        raise KeyboardInterrupt()
    if (compiler and mode == "compiler-timeout") or (binary and mode == "test-timeout"):
        kwargs["timeout"] = 0.05
        return real_run([sys.executable, "-c", "import time; time.sleep(60)"], **kwargs)
    if binary and mode == "misleading-success":
        return real_run([sys.executable, "-c", f"print('{group}: PASS cases=0 failures=0')"], **kwargs)
    if binary and mode == "failure-with-pass":
        count = 32 if group == "ui" else 14
        return real_run([sys.executable, "-c", f"print('{group}: PASS cases={count} failures=0'); raise SystemExit(7)"], **kwargs)
    return real_run(args, **kwargs)
subprocess.run = probe_run
sys.argv = ["run_tests.py", "--group", group]
runpy.run_path("applications_user/flooper/tests/run_tests.py", run_name="__main__")
'''


@dataclass(frozen=True, slots=True)
class DigestEntry:
    path: Path
    digest: str


class BaselineError(RuntimeError):
    path: Path
    reason: str

    def __init__(self, path: Path, reason: str) -> None:
        self.path = path
        self.reason = reason
        super().__init__(f"integrity FAIL {path}: {reason}")


def read_manifest(path: Path) -> tuple[DigestEntry, ...]:
    entries: list[DigestEntry] = []
    seen: set[Path] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        digest, separator, name = line.partition("  ")
        relative = Path(name)
        if (not separator or not re.fullmatch(r"[0-9a-f]{64}", digest) or
                relative.is_absolute() or ".." in relative.parts or
                relative.as_posix() != name or relative in seen):
            raise BaselineError(path, "malformed or duplicate manifest entry")
        seen.add(relative)
        entries.append(DigestEntry(relative, digest))
    if not entries:
        raise BaselineError(path, "empty manifest")
    return tuple(entries)


def check_digests(checkout: Path, entries: tuple[DigestEntry, ...], source: Path) -> None:
    for entry in entries:
        actual = hashlib.sha256((checkout / entry.path).read_bytes()).hexdigest()
        if actual != entry.digest:
            raise BaselineError(entry.path, f"SHA-256 mismatch against {source}: expected={entry.digest} actual={actual}")


def verify_baselines(checkout: Path) -> tuple[int, int]:
    if hashlib.sha256((checkout / ANCHOR).read_bytes()).hexdigest() != ANCHOR_DIGEST:
        raise BaselineError(ANCHOR, "retained anchor digest mismatch")
    anchor = read_manifest(checkout / ANCHOR)
    core = tuple(entry for entry in anchor if entry.path not in AUTHORIZED)
    check_digests(checkout, core, ANCHOR)
    pinned = read_manifest(checkout / BASELINE)
    inventory = {p.relative_to(checkout) for p in (checkout / ROOT).rglob("*")
                 if p.is_file() and "tests" not in p.relative_to(checkout).parts}
    inventory.update(p.relative_to(checkout) for p in (checkout / ROOT / "tests/fixtures").rglob("*")
                     if p.is_file())
    inventory.add(Path("targets/f7/api_symbols.csv"))
    pinned_paths = {entry.path for entry in pinned}
    if pinned_paths != inventory or not {entry.path for entry in anchor}.issubset(pinned_paths):
        raise BaselineError(BASELINE, f"inventory mismatch: missing={sorted(inventory - pinned_paths)} stale={sorted(pinned_paths - inventory)}")
    check_digests(checkout, pinned, BASELINE)
    return len(core), len(pinned)


def test_baselines_reject_mutated_copies() -> None:
    # Given retained digests and isolated copies; when any protected file changes;
    # then the historical/full-tree check rejects it rather than self-baselining.
    entries = [line.split("  ", 1)[1] for line in BASELINE.read_text().splitlines()
               if line and not line.startswith("#")]
    with tempfile.TemporaryDirectory(prefix="flooper-baseline-probes-") as directory:
        checkout = Path(directory)
        for relative in [Path(name) for name in entries] + [ANCHOR, BASELINE]:
            target = checkout / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            _ = shutil.copyfile(relative, target)
        for name in entries:
            target = checkout / name
            original = target.read_bytes()
            _ = target.write_bytes(original + b"\nmutated-before-QA\n")
            try:
                _ = verify_baselines(checkout)
            except BaselineError as error:
                assert name in str(error) and "mismatch" in str(error), error
                source = BASELINE if Path(name) in AUTHORIZED else ANCHOR
                assert str(source) in error.reason, error
                print(f"baseline mutation: PASS rejected {name}: {error}")
            else:
                raise AssertionError(f"baseline accepted mutated file: {name}")
            _ = target.write_bytes(original)
        # Given each required manifest; when absent/corrupt; then fail closed.
        for relative in (ANCHOR, BASELINE):
            target = checkout / relative
            original = target.read_bytes()
            for replacement in (None, b"", original + original, b"\n".join(original.splitlines()[:-1]) + b"\n"):
                if replacement is None:
                    target.unlink()
                else:
                    _ = target.write_bytes(replacement)
                try:
                    _ = verify_baselines(checkout)
                except (BaselineError, OSError) as error:
                    assert str(relative) in str(error), error
                    print(f"baseline manifest: PASS rejected {relative}: {error}")
                else:
                    raise AssertionError(f"baseline accepted absent/corrupt manifest: {relative}")
                _ = target.write_bytes(original)
        # Given the pinned inventory; when a new production file appears; then reject.
        extra = checkout / ROOT / "unexpected.c"
        _ = extra.write_text("unapproved", encoding="utf-8")
        try:
            _ = verify_baselines(checkout)
        except BaselineError as error:
            assert "inventory mismatch" in error.reason, error
            print(f"baseline inventory: PASS rejected {extra.name}")
        else:
            raise AssertionError("baseline accepted unlisted production file")


def main() -> int:
    if sys.flags.optimize:
        print("integrity FAIL: QA assertions disabled", file=sys.stderr)
        return 2
    try:
        core_count, full_count = verify_baselines(Path.cwd())
    except (BaselineError, OSError, UnicodeError) as error:
        print(f"integrity FAIL: {error}", file=sys.stderr)
        return 1
    print(f"retained Todo-3 immutable core: PASS {core_count} files")
    print(f"pinned full production baseline: PASS {full_count} files")
    test_baselines_reject_mutated_copies()
    protected = sorted(p for p in ROOT.rglob("*") if p.is_file() and "tests" not in p.parts)
    protected += sorted((ROOT / "tests/fixtures").glob("*.json"))
    protected.append(Path("targets/f7/api_symbols.csv"))
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in protected}
    with tempfile.TemporaryDirectory(prefix="flooper-todo5-qa-") as directory:
        env = dict(os.environ, TMPDIR=directory)
        for group in ("ui", "adapters"):
            for mode in ("stale-binary", "misleading-success", "failure-with-pass", "compiler-timeout",
                         "test-timeout", "compiler-interrupt", "test-interrupt"):
                result = subprocess.run([sys.executable, "-c", PROBE, mode, group], env=env,
                                        capture_output=True, text=True, timeout=60)
                assert result.returncode != 0 and f"{group}: PASS" not in result.stdout, result
                assert not list(Path(directory).iterdir()), "interrupted run leaked artifacts"
                print(f"{group} adversarial {mode}: PASS rejected; cleanup PASS")
            outputs: list[str] = []
            for attempt in (1, 2):
                result = subprocess.run([sys.executable, RUNNER, "--group", group], env=env,
                                        capture_output=True, text=True, timeout=90, check=True)
                expected = "ui: PASS cases=32 failures=0\n" if group == "ui" else (
                    "adapters: PASS cases=14 failures=0\nadapters-path: PASS cases=4 failures=0\n")
                assert result.stdout == expected and not result.stderr, result
                outputs.append(result.stdout)
                print(f"fresh {group} run {attempt}:\n{result.stdout}", end="")
            assert outputs[0] == outputs[1]
        for args in (["-O", RUNNER, "--group", "ui"], [RUNNER, "--group", "zzz"],
                     [RUNNER, "--group", "x" * 32768], [RUNNER, "--group"],
                     [RUNNER, "--group", "adapters", "extra"]):
            result = subprocess.run([sys.executable, *args], env=env, capture_output=True,
                                    text=True, timeout=10)
            assert result.returncode == 2 and "FAIL" in result.stderr
            print("adversarial CLI: PASS rejected")
        assert not list(Path(directory).iterdir())
    assert hashes == {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in protected}
    _ = verify_baselines(Path.cwd())
    print(f"protected source/assets/fixtures/docs/API integrity: PASS {len(protected)} files unchanged")
    print("double-run: PASS identical nonzero C cases, fresh ASan/UBSan compilations")
    print("device Canvas/acoustics/RAM/stack/deployment: SKIP unauthorized")
    return 0


if __name__ == "__main__":
    sys.exit(main())
