# FLOOPER boundaries

Schema v2/revision 1 supersedes special Bulerias engine fields. FLOOPER identity
and the approved two-item selector are retained, not the legacy package name or
draft Left/Right switching. The legacy specification is fixture provenance,
not authority to create another app or change these controls.

## Implemented data flow

Storage -> bounded JSON tokenizer -> schema dispatcher / legacy adapter ->
generic normalized document -> validation -> immutable schedule -> worker.
UI sees copied snapshots; it never controls musical deadlines or speaker HAL.
The catalog is only two display names and basenames, in BULERIAS/TANGOS order;
there are no musical event tables in C and no behavior selected by those names.

`json_min` owns syntax and token bounds. `flooper_pattern` owns canonical schema
and references. `flooper_pattern_legacy` alone owns load-time v1/draft migration.
`flooper_schedule` owns count-local monophonic intervals. `flooper_player` owns
Storage while loading, document/schedule lifetime, commands, absolute clock,
speaker acquire/stop/release, and joined shutdown. `flooper_ui` owns rendering
and pose timing; `flooper.c` owns input routing and the application lifecycle.
Only the worker calls speaker APIs; main sends commands without inferring Toggle.

Count labels are ordered positions, not arithmetic indexes. Tangos relabeling
changes `[1,2,3,4]` to `[4,1,2,3]` without audio rotation: each corresponding
event remains at the same label-array index and microsecond offset. Canonical
timing derives only from `pulse_us`; draft cycle metadata stays in test input.
Legacy-only clap/offbeat conversion must never leak into generic playback.

## Interaction and lifetime contract

Silent 1000ms FLOOPER splash -> bounded selector. Short Up/Down select;
Short OK loads paused. Playback Short OK toggles, Long OK restarts. Long Back
alone exits in every state; other playback input is ignored. Numeric-label-3
dolphin contact lasts 80ms and is visual only. A repeated same-label snapshot does
not retrigger it. One to six labels use one row; seven to twelve use two rows.
Position inversion follows array index, while accent markers follow label values.

Input callbacks filter logical events and enqueue with zero wait. Long Back calls
the player's sticky exit API before setting the app flag, bypassing a full input
queue. Queued events carry their source screen so selector input cannot leak into
playback. The main loop polls at the 10ms target refresh cadence; the timer only
enqueues a wake event. Main copies player snapshots, rejecting stale generations,
and requests ViewPort updates. Draw copies the UI model under mutex, releases it,
then renders solely from the copy with bounded text buffers and Canvas primitives.

One idempotent close path stops/frees the timer first. Timer free flushes queued
timer work in this checkout. Disable/remove ViewPort occurs without the UI mutex;
GUI removal serializes against draw/input callbacks under the GUI lock. Input
queue, UI mutex/model and ViewPort are then unreachable and can be freed. Player
quit/join precedes destruction of its worker-visible data; GUI record closes last.
Early memory-admission failure uses the same path, with no join absent a worker.

## Test boundary

Package checks verify source/manifest layout, byte provenance, whole-document
Tangos invariance, fixture isolation, compiled catalog, and literal asset macro.
Each named group compiles its actual Furi-free or adapter C source;
host fakes live only in `tests/fakes`. The storage fake deliberately concatenates
string tokens like local `storage.h`; it must reject `APP_ASSETS_PATH(variable)`.
The loader appends catalog basenames with checked snprintf to `APP_ASSETS_PATH("")`.
FLOOPER_CATALOG_FILENAME_MAX includes NUL (package asserts strlen < MAX), so the
tight capacity is sizeof(prefix) + MAX - 1. Negative/truncated results are rejected
before open. Failed opens are still closed before File free.
Package PASS alone does not claim parsing, scheduling or UI behavior. The UI and
adapter groups provide host fake-Canvas/real-worker evidence, not device visual,
acoustic or hardware QA. Hardware remains SKIP until separately authorized.
