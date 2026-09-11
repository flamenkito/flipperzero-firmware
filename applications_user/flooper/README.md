# FLOOPER 0.2 — bounded compás looper

One external C FAP: `appid=flooper`, entry `flooper_app`, Music category.
Intended device path: `/ext/apps/Music/flooper.fap`. No hardware work is authorized.

The parser, compiler, worker player and Canvas app lifecycle are implemented.
The host harness exercises the real production sources; on-device visuals,
acoustics, memory measurements and deployment remain **SKIP** until authorized.

The silent 1000ms splash leads to a two-entry selector. Short Up/Down wrap between
BULERIAS and TANGOS; Short OK loads paused. During playback, Short OK toggles and
Long OK restarts. **Long Back alone exits**, including Loading/error/busy states.
Other playback inputs are ignored. The Canvas dolphin has an 80ms visual-only
contact pose on each newly displayed label 3; it cannot schedule sound.

## Source and test boundaries

- `flooper.c` / `flooper_app.h`: identity, two-record filename/display catalog,
  bounded ViewPort/input lifecycle and worker stack constant.
- `json_min*`: bounded JSON syntax, UTF-8/escape decoding and exact integer conversion.
- `flooper_pattern*`: normalized model, typed schema readers and load-only legacy adapters.
- `flooper_schedule`: immutable generic compilation.
- `flooper_player`: worker-owned timing/audio/resources.
- `flooper_ui`: copied-model Canvas rendering and visual pose cadence.
- `assets/`: exactly two runtime documents, catalog order `BULERIAS`, `TANGOS`.
- `tests/fixtures/`: exact legacy FILE bodies, never runtime content.
- `tests/test_main.c`, `tests/fakes/`, `tests/run_tests.py`: host-only harness.
  Manifest `sources=["*.c", "!tests"]` excludes the entire test tree from FBT's
  recursive source collection; `fap_file_assets="assets"` excludes fixtures,
  tests, fakes, runner and documentation from runtime assets. No private libs,
  external dependencies, generated event tables or second FAP exist.

## Reproducible package checks

```sh
python3 applications_user/flooper/tests/run_tests.py --group package
python3 applications_user/flooper/tests/run_tests.py --group pattern
python3 applications_user/flooper/tests/run_tests.py --group schedule
python3 applications_user/flooper/tests/run_tests.py --group player
python3 applications_user/flooper/tests/run_tests.py --group ui
python3 applications_user/flooper/tests/run_tests.py --group adapters
python3 applications_user/flooper/tests/pattern_qa.py
python3 applications_user/flooper/tests/todo5_qa.py
python3 scripts/lint.py format applications_user/flooper
python3 scripts/lint.py check applications_user/flooper
./fbt build APPSRC=applications_user/flooper
```

Runner uses Python 3.10+ standard library and a C11 `cc`; no dependency install.
It compiles the real catalog with a host test main into a fresh temporary
directory, removes it on exit, and rejects runtime arguments to the literal-only
`APP_ASSETS_PATH` fake. Compiler/run timeouts are 30s/5s. Missing files, failed
assertions, unknown groups and Python `-O` produce nonzero exits. Unfiltered
invocation runs all six groups: package, pattern, schedule, player, ui and adapters.
UI/adapters each compile twice afresh with ASan/UBSan and require identical exact
nonzero-case outputs. The app tests include the actual entry source to exercise
its private callback/teardown seams, not a replacement app implementation.
Canvas fakes record text/coordinates/fonts/colors/primitives and check mutex
discipline. Their font metrics are approximations, not device pixel evidence.

Bulerias bytes and legacy fixture bodies are pinned by SHA-256 from supplied
Downloads sources. Tangos was relabeled once from the original input by the
simultaneous map `{1:4,2:1,3:2,4:3}` on labels/event counts/UI accents only.
Tests undo labels **in memory only** and compare the entire parsed original's
structural digest; every other property and array order is protected. No test
requires Downloads to remain available. Legacy Tangos retains `[1,2,3,4]` and
its contradictory `cycle_us=2879274` unchanged.

## Document API and memory bounds

`flooper_pattern_parse(input,length,workspace,output)` requires disjoint buffers.
The caller owns the transient `FlooperPatternWorkspace` and output allocations;
do not put either on the 4096-byte worker stack. The parser itself allocates no
heap memory. Workspace contains one 16385-byte private JSON copy and 2049 compact
8-byte token entries; strings decode in-place. Output owns every string/value
and resolves event count/voice/layer and section pattern references into indexes.
Patterns reference ranges in a single 128-event pool. On **any** parse error the
entire output is zeroed; only `FlooperPatternOk` permits publication. The caller
may release input/workspace immediately after return.

Typed errors distinguish syntax/size/token/depth/duplicate-key/numeric failures,
unsupported versions, missing/wrong types, exact-integer/range/string/bound
failures, duplicate IDs/labels/masks, missing references, forbidden derived time,
cross-count containment and unsupported render policy. Canonical time derives
only from `pulse_us`. Unknown timebase keys are errors; non-audio root metadata
is syntax-checked but not retained. IDs and retained display strings are nonempty
printable ASCII and never truncated. Decimal integer exactness is checked before
floating-point conversion. Numeric overflow and underflow are rejected.

Legacy adapters alone contain musical migration rules. Draft timing mismatch
is returned as `warning_cycle_mismatch` plus `reported_cycle_us`; the
worker logs it once while Loading. The C host test demonstrates this warning
without introducing Furi or logging dependencies into the model. Source intensity
and accent masks are retained metadata, not gain multipliers. Monophonic/exact
overlap policy are validated invariants, not mutable normalized policy fields.
The parser does not compile schedules: the separate compiler enforces 1024 intervals.

Pattern tests compile actual modules under AddressSanitizer/UndefinedBehaviorSanitizer,
check complete legacy/canonical model equality (except documented draft labels,
accents and warning), then clear workspace to check output ownership. Mutation
cases verify exact error names and zeroed outputs. No asset/fixture bytes change.

Main and worker stack budgets are each 4096 bytes; measured high-water marks remain SKIP.
Enforced load limits: JSON 16384 bytes, 2048 tokens, depth 16, 1–12 counts with unique
labels 1–99, up to 8 layers/voices/patterns/sections, 8 slices/voice,
64 events/pattern and 128 total, 64 candidates/count, 1024 compiled intervals.
IDs/display/section strings: 24 bytes including NUL; document name: 64 bytes.
Large buffers belong in bounded load-time allocations, never on either stack.
Free transient JSON/tokens before playback; do not allocate/read Storage during
Play. Actual RAM, stack high-water marks, acoustic behavior and device asset
extraction are **SKIP**, not inferred from a host test or package build.

The app reserves 8KiB contiguous heap admission under the scheduler lock for UI
bootstrap allocations; the player separately admits its own bounded allocations.
The 10ms target refresh cadence and bounded input waits keep exit polling below
20ms. Drawing copies the mutex-protected model and releases the mutex before any
Canvas call. Shutdown stops/frees the refresh timer (flushing callbacks), disables
and removes the ViewPort without the UI mutex, frees UI resources, joins/frees the
player, and closes GUI last. In-flight Storage can delay worker join, not sticky
Long Back acceptance. Initial asynchronous loading is permitted during the splash
but never plays; the selector's explicit load invalidates old-generation UI data.
