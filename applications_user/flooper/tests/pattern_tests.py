# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/run_tests.py --group pattern
"""Compile and exercise the real bounded C parser; fixtures are never rewritten."""

from pathlib import Path
import subprocess
import tempfile
from typing import Final
from applications_user.flooper.tests.pattern_cases import boundary_cases

ROOT: Final = Path(__file__).resolve().parents[1]


def run_pattern() -> None:
    base = (ROOT / "assets/flipper_tangos_pattern_v2_1.json").read_text()
    cases: list[tuple[str, str, str]] = []
    for path, mode in (("assets/flipper_bulerias_pattern_v2_1.json", "bulerias"),
                       ("assets/flipper_tangos_pattern_v2_1.json", "tangos"),
                       ("tests/fixtures/bulerias_v1.json", "legacy"),
                       ("tests/fixtures/tangos_v2_draft.json", "draft")):
        cases.append(((ROOT / path).read_text(), "Ok", mode))
    mutations = (
        ('"schema_revision": 1', '"schema_revision": 2', "UnsupportedPattern"),
        ('"schema_version": 2', '"schema_version": 3', "UnsupportedPattern"),
        ('"schema_version": 2', '"schema_version": 1', "UnsupportedPattern"),
        ('"schema_revision": 1', '"schema_revision": null', "Type"),
        ('"priority": 3', '"priority": 256', "Range"),
        ('"priority": 3', '"priority": 1.00000000000000001', "Integer"),
        ('"priority": 3', '"priority": true', "Type"),
        ('"priority": 3,', '', "Missing"),
        ('"slices": [', '"unused": [', "Missing"),
        ('"source_intensity": 1.0', '"source_intensity": -0.1', "Range"),
        ('"source_intensity": 1.0', '"source_intensity": "1"', "Type"),
        ('"velocity": 1.0', '"velocity": 1e999', "Number"),
        ('"velocity": 1.0', '"velocity": NaN', "JsonSyntax"),
        ('"velocity": 1.0', '"velocity": 1.1', "Range"),
        ('"offset_us": 567000', '"offset_us": 719818', "Containment"),
        ('"offset_us": 567000', '"offset_us": 719819', "Range"),
        ('"voice": "low_pluck"', '"voice": "missing"', "Reference"),
        ('"layer": "guitar_proxy"', '"layer": "missing"', "Reference"),
        ('"count": 4', '"count": 99', "Reference"),
        ('[4, 1, 2, 3]', '[4, 1, 2, 4]', "Duplicate"),
        ('[4, 1, 2, 3]', '[]', "Limit"),
        ('[4, 1, 2, 3]', '[1,2,3,4,5,6,7,8,9,10,11,12,13]', "Limit"),
        ('"pulse_us": 719819', '"pulse_us": 99999', "Range"),
        ('"pulse_us": 719819', '"pulse_us": 719819, "cycle_us": 2879276', "DerivedTime"),
        ('"pulse_us": 719819', '"pulse_us": 719819, "pulse_seconds": 0.719819', "DerivedTime"),
        ('"pulse_us": 719819', '"pulse_us": 719819, "cycle_counts": 4', "DerivedTime"),
        ('"pulse_us": 719819', '"pulse_us": 719819, "pulse_bpm": 83', "DerivedTime"),
        ('"pulse_us": 719819', '"pulse_us": 719819, "cycle_seconds": 2.8', "DerivedTime"),
        ('"monophonic": true', '"monophonic": false', "RenderPolicy"),
        ('higher_priority_then_louder', 'louder', "RenderPolicy"),
        ('"repeat": true', '"repeat": 1', "Type"),
        ('"start_section": 0', '"start_section": 1', "Reference"),
        ('"cycles": 1', '"cycles": 0', "Range"),
        ('"name": "COMPAS"', '"name": ""', "String"),
        ('"name": "COMPAS"', '"name": "C\\nOMPAS"', "String"),
        ('"display_name": "TANGOS"', '"display_name": "' + 'x' * 24 + '"', "String"),
        ('"display_name": "TANGOS",', '', "Missing"),
        ('"gain": 0.9', '"gain": -0.1', "Range"),
        ('"duration_us": 10000', '"duration_us": 0', "Range"),
        ('"frequency_hz": 360', '"frequency_hz": 99', "Range"),
        ('"id": "bright_strum"', '"id": "low_pluck"', "Duplicate"),
        ('"accent_counts": [4]', '"accent_counts": [4,4]', "Duplicate"),
        ('"accent_counts": [4]', '"accent_counts": [99]', "Reference"),
        ('"schema_revision": 1', '"schema_revision": 1, "schema_revision": 1', "JsonDuplicate"),
        ('"schema_revision": 1', '"schema_revision": 1, "schema_revisio\\u006e": 1', "JsonDuplicate"),
        ('"display_name": "TANGOS"', '"display_name": "\\q"', "JsonSyntax"),
        ('"display_name": "TANGOS"', '"display_name": "\\uD800"', "JsonSyntax"),
    )
    for old, new, error in mutations:
        assert old in base, old
        cases.append((base.replace(old, new, 1), error, "mutation"))
    cases.extend(((base + "x", "JsonSyntax", "trailing"),
                  (" " * 16385, "JsonSize", "size"),
                  ("[" * 17 + "0" + "]" * 17, "JsonDepth", "depth"),
                  ("[" + ",".join("0" for _ in range(2048)) + "]", "JsonTokens", "tokens"),
                  ("{}", "Missing", "empty")))
    cases.extend(boundary_cases())
    legacy = (ROOT / "tests/fixtures/bulerias_v1.json").read_text()
    for name in ("intro", "palmas", "palmas_perc"):
        original = f'"name": "{name}"'
        assert legacy.count(original) == 1
        cases.append((legacy.replace(original, '"name": "renamed"', 1), "Ok", "legacy"))
    for replacement, error in (('"name": ""', "String"),
                               ('"name": "' + 'x' * 24 + '"', "String"),
                               ('"name": "bad\\nname"', "String"),
                               ('"name": 1', "Type"),
                               ('"unused": "intro"', "Missing")):
        cases.append((legacy.replace('"name": "intro"', replacement, 1), error, "legacy-name"))
    for original, replacement, error in (('"cycles": 4', '"cycles": 5', "Range"),
                                         ('"layers": []', '"layers": ["palmas"]', "UnsupportedPattern"),
                                         ('"layers": ["palmas"]', '"layers": ["palmas", "palmas"]', "Duplicate"),
                                         ('"layers": ["palmas"]', '"layers": ["unknown"]', "Reference")):
        cases.append((legacy.replace(original, replacement, 1), error, "legacy-structure"))
    draft = (ROOT / "tests/fixtures/tangos_v2_draft.json").read_text()
    cases.extend(((draft.replace('"guitar_proxy"', '"other"'), "UnsupportedPattern", "draft-layer"),
                  (draft.replace('"cycle_us": 2879274', '"cycle_us": "bad"'), "Type", "draft-time"),
                  (draft.replace('"priority": 3', '"priority": 256', 1), "Range", "draft-priority"),
                  (draft.replace('"source_intensity": 1.0', '"source_intensity": 2', 1), "Range", "draft-intensity")))
    with tempfile.TemporaryDirectory(prefix="flooper-pattern-") as directory:
        temporary = Path(directory)
        binary = temporary / "pattern-test"
        sources = sorted(ROOT.glob("flooper_pattern*.c")) + sorted(ROOT.glob("json_min*.c"))
        command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-fsanitize=address,undefined", "-g", "-I", str(ROOT)]
        _ = subprocess.run(command + [str(p) for p in sources] +
                           [str(ROOT / "tests/pattern_main.c"), "-lm", "-o", str(binary)],
                           check=True, timeout=30)
        for index, (raw, expected, mode) in enumerate(cases):
            fixture = temporary / "case.json"
            _ = fixture.write_text(raw)
            result = subprocess.run([str(binary), str(fixture), expected, mode],
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, (index, mode, result.stdout, result.stderr)
            assert result.stdout == f"case: PASS expected={expected}; cases=1 failures=0\n"
            if mode == "draft":
                assert result.stderr == "warning: ignored draft cycle_us=2879274; derived cycle_us=2879276\n"
                print(result.stderr.strip())
            print(f"pattern[{index + 1}]: {mode} expected={expected} PASS")
        _ = subprocess.run(command + [str(p) for p in sources] +
                           [str(ROOT / "tests/pattern_equivalence.c"), "-lm", "-o", str(binary)],
                           check=True, timeout=30)
        for asset, fixture, mode in (("flipper_bulerias_pattern_v2_1.json", "bulerias_v1.json", "legacy"),
                                     ("flipper_tangos_pattern_v2_1.json", "tangos_v2_draft.json", "draft")):
            result = subprocess.run([str(binary), str(ROOT / "assets" / asset),
                                     str(ROOT / "tests/fixtures" / fixture), mode],
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 0, (mode, "whole-model equivalence", result.stderr)
            assert result.stdout == "equivalence: PASS cases=1 failures=0\n"
            print(f"pattern: whole-model {mode} equivalence PASS")
    assert len(cases) > 0
    print(f"pattern: PASS {len(cases) + 2} cases; total: {len(cases) + 2} passed, 0 failed")
