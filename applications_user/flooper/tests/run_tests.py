# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# How to run: python3 applications_user/flooper/tests/run_tests.py --group package
"""Package and production C pattern checks; future groups must not silently pass."""

import ast
from collections.abc import Callable
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Final, TypedDict

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))


class Timebase(TypedDict):
    count_labels: list[int]
    pulse_us: int
    cycle_us: int


class Event(TypedDict):
    count: int
    offset_us: int


class Pattern(TypedDict):
    events: list[Event]


class Section(TypedDict):
    name: str
    cycles: int


class Ui(TypedDict):
    accent_counts: list[int]


class Document(TypedDict):
    schema_version: int
    schema_revision: int
    timebase: Timebase
    patterns: list[Pattern]
    sections: list[Section]
    ui: Ui


# Test projection only: digest/equality assertions cover the whole JSON tree;
# this callable annotation does not claim production schema validation.
def decode_document(raw: bytes, decoder: Callable[[bytes], Document] = json.loads) -> Document:
    return decoder(raw)

ROOT: Final = Path(__file__).resolve().parents[1]
MODULES: Final = (
    "flooper_pattern", "flooper_pattern_legacy", "flooper_schedule",
    "flooper_player", "flooper_ui", "json_min",
)
ASSETS: Final = (
    "flipper_bulerias_pattern_v2_1.json", "flipper_tangos_pattern_v2_1.json",
)
FIXTURES: Final = {
    "bulerias_v1.json": "0ff050d322dacb90c632e2cc05f3283b4fd5417fd8007d0acb3cfdd760900c7f",
    "tangos_v2_draft.json": "bb43f94dc59280e431698419d80f037b826c451e1334af401f4c4bfd738f5842",
}


def test_layout_when_package_created() -> None:
    # Given the approved layout; when inspecting the package; then all exist.
    required = ["application.fam", "flooper.c", "flooper_app.h", "README.md",
                "ARCHITECTURE.md", "tests/test_main.c", "tests/fakes/storage/storage.h"]
    required += [f"{name}.{ext}" for name in MODULES for ext in ("c", "h")]
    required += [f"assets/{name}" for name in ASSETS]
    required += [f"tests/fixtures/{name}" for name in FIXTURES]
    missing = [name for name in required if not (ROOT / name).is_file()]
    assert not missing, f"missing package artifacts: {missing}"
    assert not (ROOT.parent / "bulerias_looper").exists()
    assert sorted(p.name for p in (ROOT / "assets").iterdir()) == sorted(ASSETS)
    assert sorted(p.name for p in (ROOT / "tests/fixtures").iterdir()) == sorted(FIXTURES)


def test_manifest_when_parsed() -> None:
    # Given a manifest; when parsed without exec; then identity/exclusions match.
    tree = ast.parse((ROOT / "application.fam").read_text())
    assert len(tree.body) == 1
    statement = tree.body[0]
    assert isinstance(statement, ast.Expr)
    call = statement.value
    assert isinstance(call, ast.Call)
    assert isinstance(call.func, ast.Name) and call.func.id == "App"
    values = {item.arg: item.value for item in call.keywords}
    app_type = values.pop("apptype")
    assert isinstance(app_type, ast.Attribute) and app_type.attr == "EXTERNAL"
    assert isinstance(app_type.value, ast.Name) and app_type.value.id == "FlipperAppType"
    fields = {key: ast.literal_eval(value) for key, value in values.items()}
    assert fields == {
        "appid": "flooper", "name": "FLOOPER", "entry_point": "flooper_app",
        "fap_category": "Music", "fap_version": "0.2", "fap_file_assets": "assets",
        "requires": ["gui"], "stack_size": 4096, "sources": ["*.c", "!tests"],
    }


def test_bulerias_when_packaged() -> None:
    # Given the supplied bytes; when hashing the asset; then it is verbatim.
    raw = (ROOT / "assets" / ASSETS[0]).read_bytes()
    assert hashlib.sha256(raw).hexdigest() == (
        "9cea60433bf578abbf602673456ec9d40d5ebeeb6d2e45ff88f8f565e07337e4"
    )
    data = decode_document(raw)
    assert (data["schema_version"], data["schema_revision"]) == (2, 1)
    assert data["timebase"]["count_labels"] == [6, 1, 2, 3, 4, 5]
    assert data["timebase"]["pulse_us"] == 314667
    assert [(s["name"], s["cycles"]) for s in data["sections"]] == [
        ("INTRO", 4), ("PALMAS", 4), ("PALMAS+PERC", 12),
    ]


def test_tangos_when_packaged() -> None:
    # Given corrected labels; when undoing labels IN MEMORY; then every field
    # matches the independently pinned original structural digest, including
    # metadata, ordering, optional fields and every voice/event property.
    data = decode_document((ROOT / "assets" / ASSETS[1]).read_bytes())
    assert (data["schema_version"], data["schema_revision"]) == (2, 1)
    assert data["timebase"]["count_labels"] == [4, 1, 2, 3]
    assert data["ui"]["accent_counts"] == [4]
    assert data["timebase"]["pulse_us"] * 4 == 2879276
    events = [e for p in data["patterns"] for e in p["events"]]
    assert len(events) == 16
    assert {label: [e["offset_us"] for e in events if e["count"] == label]
            for label in (4, 1, 2, 3)} == {
        4: [0, 216000, 388000, 562000], 1: [0, 203000, 390000, 572000],
        2: [0, 202000, 389000, 563000], 3: [0, 195000, 392000, 567000],
    }
    inverse = {4: 1, 1: 2, 2: 3, 3: 4}
    data["timebase"]["count_labels"] = [inverse[n] for n in data["timebase"]["count_labels"]]
    data["ui"]["accent_counts"] = [inverse[n] for n in data["ui"]["accent_counts"]]
    for event in events:
        event["count"] = inverse[event["count"]]
    normalized = json.dumps(data, sort_keys=True, separators=(",", ":")).encode()
    assert hashlib.sha256(normalized).hexdigest() == (
        "f19c2988cc81eedc4aefe1f21553227ca7251d8991bb8e10f89005372896e8dd"
    ), "a non-remapped Tangos property changed"


def test_fixtures_when_packaged() -> None:
    # Given exact FILE bodies; when hashing fixtures; then legacy stays legacy.
    for name, digest in FIXTURES.items():
        assert hashlib.sha256((ROOT / "tests/fixtures" / name).read_bytes()).hexdigest() == digest
    draft = decode_document((ROOT / "tests/fixtures/tangos_v2_draft.json").read_bytes())
    assert draft["timebase"]["count_labels"] == [1, 2, 3, 4]
    assert draft["timebase"]["cycle_us"] == 2879274
    assert "schema_revision" not in draft


def test_host_boundary_when_compiled() -> None:
    # Given actual catalog source and literal-only fake; when compiled afresh;
    # then catalog assertions run and runtime macro input fails compilation.
    with tempfile.TemporaryDirectory(prefix="flooper-package-") as directory:
        binary = str(Path(directory) / "package-test")
        flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-ffunction-sections", "-fdata-sections",
                 "-I", str(ROOT), "-I", str(ROOT / "tests/fakes")]
        link = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        _ = subprocess.run(flags + [str(ROOT / "flooper.c"), str(ROOT / "tests/test_main.c"),
                                link, "-o", binary], check=True, timeout=30)
        result = subprocess.run([binary], check=True, capture_output=True, text=True, timeout=5)
        assert result.stdout == "catalog: PASS (2 records); assets macro: PASS\n"
        invalid = '#include <storage/storage.h>\nconst char* f(const char* p) { return APP_ASSETS_PATH(p); }\n'
        rejected = subprocess.run(flags + ["-x", "c", "-fsyntax-only", "-"], input=invalid,
                                  capture_output=True, text=True, timeout=30)
        assert rejected.returncode != 0, "fake accepts runtime APP_ASSETS_PATH input"


def main() -> int:
    if sys.flags.optimize:
        print("package: FAIL (assertions disabled)", file=sys.stderr)
        return 2
    if sys.argv[1:] in (["--group", "ui"], ["--group", "adapters"]):
        from applications_user.flooper.tests.ui_tests import run_ui
        run_ui(sys.argv[2])
        return 0
    if sys.argv[1:] == ["--group", "player"]:
        from applications_user.flooper.tests.player_tests import run_player
        run_player()
        return 0
    if sys.argv[1:] == ["--group", "schedule"]:
        from applications_user.flooper.tests.schedule_tests import run_schedule
        run_schedule()
        return 0
    if sys.argv[1:] == ["--group", "pattern"]:
        from applications_user.flooper.tests.pattern_tests import run_pattern
        run_pattern()
        return 0
    if sys.argv[1:] not in ([], ["--group", "package"]):
        print("FAIL: supported groups: package, pattern, schedule, player, ui, adapters", file=sys.stderr)
        return 2
    cases = (test_layout_when_package_created, test_manifest_when_parsed,
             test_bulerias_when_packaged, test_tangos_when_packaged,
             test_fixtures_when_packaged, test_host_boundary_when_compiled)
    for case in cases:
        try:
            case()
        except (AssertionError, OSError, ValueError, KeyError, TypeError,
                subprocess.SubprocessError) as error:
            print(f"package: FAIL {case.__name__}: {error}", file=sys.stderr)
            return 1
        print(f"package: PASS {case.__name__}")
    print(f"package: PASS {len(cases)} cases; total: {len(cases)} passed, 0 failed")
    if not sys.argv[1:]:
        from applications_user.flooper.tests.pattern_tests import run_pattern
        run_pattern()
        from applications_user.flooper.tests.schedule_tests import run_schedule
        run_schedule()
        from applications_user.flooper.tests.player_tests import run_player
        run_player()
        from applications_user.flooper.tests.ui_tests import run_ui
        run_ui("ui")
        run_ui("adapters")
    return 0


if __name__ == "__main__":
    sys.exit(main())
