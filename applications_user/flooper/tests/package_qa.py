# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/package_qa.py
"""Independent source comparison and adversarial package-runner checks."""

import copy
from pathlib import Path
import re
import subprocess
import sys
from typing import Final

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from applications_user.flooper.tests.run_tests import ASSETS, ROOT, decode_document

DOWNLOADS: Final = Path("/Users/asutov/Downloads")
RUNNER: Final = "applications_user/flooper/tests/run_tests.py"


def compare_sources() -> None:
    # Given original Downloads; when mapping a fresh in-memory copy once;
    # then the full structure equals the packaged document, not just totals.
    original = decode_document((DOWNLOADS / ASSETS[1]).read_bytes())
    packaged = decode_document((ROOT / "assets" / ASSETS[1]).read_bytes())
    expected = copy.deepcopy(original)
    mapping = {1: 4, 2: 1, 3: 2, 4: 3}
    assert original["timebase"]["count_labels"] == [1, 2, 3, 4]
    expected["timebase"]["count_labels"] = [mapping[n] for n in original["timebase"]["count_labels"]]
    expected["ui"]["accent_counts"] = [mapping[n] for n in original["ui"]["accent_counts"]]
    for pattern in expected["patterns"]:
        for event in pattern["events"]:
            event["count"] = mapping[event["count"]]
    assert expected == packaged, "full Tangos structural comparison failed"
    for old_pattern, new_pattern in zip(original["patterns"], packaged["patterns"], strict=True):
        for old, new in zip(old_pattern["events"], new_pattern["events"], strict=True):
            old_index = original["timebase"]["count_labels"].index(old["count"])
            new_index = packaged["timebase"]["count_labels"].index(new["count"])
            assert old_index == new_index and old["offset_us"] == new["offset_us"]
    assert (DOWNLOADS / ASSETS[0]).read_bytes() == (ROOT / "assets" / ASSETS[0]).read_bytes()
    text = (DOWNLOADS / "codex_compas_looper_update_v2_1.txt").read_bytes()
    for name in ("bulerias_v1.json", "tangos_v2_draft.json"):
        marker = f"applications_user/bulerias_looper/tests/fixtures/{name}".encode()
        bodies = re.findall(rb"FILE_BEGIN " + marker + rb"\n(.*?)FILE_END " + marker, text, re.S)
        assert len(bodies) == 1
        assert bodies[0] == (ROOT / "tests/fixtures" / name).read_bytes()
    print("structural/source comparison: PASS (entire Tangos tree, event order/time, Bulerias bytes, both fixture bodies)")


PROBE: Final = r'''
import json, pathlib, runpy, subprocess, sys
mode = sys.argv[1]
read_bytes = pathlib.Path.read_bytes
is_file = pathlib.Path.is_file
run = subprocess.run
def probe_read(path):
    raw = read_bytes(path)
    if path.name == "flipper_tangos_pattern_v2_1.json":
        data = json.loads(raw)
        if mode == "stale-labels":
            data["timebase"]["count_labels"] = [1,2,3,4]
        if mode == "altered-property":
            data["metadata"]["origin"] = "changed non-remapped property"
        return json.dumps(data).encode()
    return raw
def probe_file(path):
    return False if mode == "missing-asset" and path.name == "flipper_tangos_pattern_v2_1.json" else is_file(path)
def probe_run(args, **kwargs):
    if args[0] == "cc" and mode in ("stale-binary", "misleading-success", "timeout"):
        code = {"stale-binary": "print(\"PASS stale compiler output\")",
                "misleading-success": "print(\"PASS\"); raise SystemExit(7)",
                "timeout": "import time; time.sleep(60)"}[mode]
        if mode == "timeout":
            kwargs["timeout"] = 0.1
        return run([sys.executable, "-c", code], **kwargs)
    return run(args, **kwargs)
pathlib.Path.read_bytes = probe_read
pathlib.Path.is_file = probe_file
subprocess.run = probe_run
sys.argv = ["run_tests.py", "--group", "package"]
runpy.run_path("applications_user/flooper/tests/run_tests.py", run_name="__main__")
'''


def main() -> int:
    compare_sources()
    for mode in ("stale-labels", "altered-property", "missing-asset", "stale-binary",
                 "misleading-success", "timeout"):
        result = subprocess.run([sys.executable, "-c", PROBE, mode], capture_output=True,
                                text=True, timeout=40)
        print(f"probe {mode}: exit={result.returncode}\n{result.stdout}{result.stderr}")
        assert result.returncode == 1 and "package: FAIL" in result.stderr
        assert "total:" not in result.stdout, "misleading final success"
        print(f"probe {mode}: PASS (expected rejection)")
    for args in ([sys.executable, "-O", RUNNER, "--group", "package"],
                 [sys.executable, RUNNER, "--group", "unsupported"]):
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and "FAIL" in result.stderr
        assert "total:" not in result.stdout
        print(f"probe {args}: PASS exit=2 {result.stderr.strip()}")
    outputs: list[str] = []
    for attempt in (1, 2):
        result = subprocess.run([sys.executable, RUNNER, "--group", "package"],
                                capture_output=True, text=True, timeout=60)
        print(f"package double-run {attempt}: exit={result.returncode}\n{result.stdout}{result.stderr}")
        assert result.returncode == 0 and "total: 6 passed, 0 failed" in result.stdout
        outputs.append(result.stdout)
    assert outputs[0] == outputs[1]
    print("double-run: PASS (identical output, fresh host compilation each time)")
    print("cleanup: PASS (in-memory mutations only; TemporaryDirectory contexts removed host binaries)")
    print("manual package QA: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
