# Decisions — Pocket AirBridge Monorepo Merge

Append-only ledger of decisions, evidence SHAs, and inventory findings.
Each block is timestamped and read-only; never overwrite prior entries.

---

## 2026-09-01T22:13:15Z — Task 1, Step 1: Inventory of both repositories

### Repo identity

| Repo | Absolute path | Branch | HEAD SHA | HEAD subject | Commits |
|------|---------------|--------|----------|--------------|---------|
| Source (flipper-hid) | `/Users/asutov/projects/flipper-hid` | `master` | `9ae70e3048cd197a351cac95100c8337bc9c7c6b` | `fix(airbridge): stabilize BLE reconnect and deploy flows` | 42 |
| Target (flipperzero-firmware) | `/Users/asutov/projects/flipperzero-firmware` | `pocket-airbridge` | `55d841791a476e52a3306d44917bc5852f74e05f` | `fix(airbridge): stabilize BLE reconnect and deploy flows` | 2751 |

The source HEAD subject and target HEAD subject are identical strings
(independent repos, same release message); SHAs are unrelated.

### Remotes

| Repo | remote | URL |
|------|--------|-----|
| source `origin` | `git@github.com:flamenkito/flipper-airbridge.git` (private) |
| target `origin` | `git@github.com:flamenkito/flipperzero-firmware.git` (public) |

No `airbridge` remote exists on the target; the plan's temporary local
remote is not yet present.

### Tracked / untracked / ignored state

#### Source `flipper-hid` — `git status --short --untracked-files=all`

Empty. Tracked worktree is clean; no untracked files visible.

#### Source `flipper-hid` — `git status --short --ignored`

```
!! .agents/
!! .codegraph
!! .idea/
!! .omo/
!! .playwright-mcp/
!! AGENTS.md
```

Six ignored root entries. `.codegraph` is a symlink to
`/Users/asutov/.omo/codegraph/projects/flipper-hid-29f6718bef69022e`
(not a directory). `.idea/` is empty (3 files). `.agents/`, `.omo/`,
`AGENTS.md`, and `.playwright-mcp/` are real, populated directories.

#### Target `flipperzero-firmware` — `git status --short --untracked-files=all`

Empty. Tracked worktree is clean; no untracked files visible.

#### Target `flipperzero-firmware` — `git status --short --ignored`

```
!! .idea/
!! .sconsign.dblite
!! applications_user/pocket_airbridge/application.fam
!! applications_user/pocket_airbridge/icon.png
!! applications_user/six_mouse/
!! build/
!! dist/
!! scripts/__pycache__/
!! scripts/fbt/__pycache__/
!! scripts/fbt/sdk/__pycache__/
!! scripts/fbt_tools/__pycache__/
!! scripts/flipper/__pycache__/
!! scripts/flipper/assets/__pycache__/
!! scripts/flipper/utils/__pycache__/
!! site_scons/fbt_extra/__pycache__/
!! toolchain/
```

Two ignored files are inside the canonical FAP directory:
`applications_user/pocket_airbridge/application.fam` and `.../icon.png`.
Both are present on disk but excluded by `applications_user/.gitignore`.

### Blockers

None. Both tracked worktrees are clean — no unresolved tracked changes
in either repo. Plan execution may proceed.

### Source metadata tracking (`AGENTS.md`, `.agents`, `.omo`)

| Path | Exists on disk | Tracked (`git ls-files`) | Ignored (rule) |
|------|----------------|--------------------------|----------------|
| `AGENTS.md` (13009 B, mtime 2026-08-31) | yes | NO | `.git/info/exclude:9:/AGENTS.md` |
| `.agents/skills/flipper-zero-dev/SKILL.md` | yes | NO | `.git/info/exclude:8:/.agents/` |
| `.agents/skills/pocket-airbridge-hardware-qa/SKILL.md` | yes | NO | `.git/info/exclude:8:/.agents/` |
| `.agents/skills/flipper-zero-dev/references/{usb-airbridge-profile,ble-serial-passthrough,hid-composite-deploy,api-symbol-exports,firmware-build-and-flash,unleashed-compatibility}.md` (6 files) | yes | NO | `.git/info/exclude:8:/.agents/` |
| `.agents/skills/pocket-airbridge-hardware-qa/references/hardware-checklist.md` | yes | NO | `.git/info/exclude:8:/.agents/` |
| `.omo/plans/monorepo-merge.md` (20548 B, mtime 2026-09-01) | yes | NO | `.git/info/exclude:10:/.omo/` |
| `.omo/notepads/` (16 subdirs) | yes | NO | `.git/info/exclude:10:/.omo/` |
| `.omo/evidence/`, `.omo/plans/`, `.omo/drafts/` (16+17+6 entries) | yes | NO | `.git/info/exclude:10:/.omo/` |
| `.omo/boulder.json` (31492 B, mtime 2026-09-02) | yes | NO | `.git/info/exclude:10:/.omo/` (runtime state) |
| `.omo/run-continuation/` (381 entries) | yes | NO | `.git/info/exclude:10:/.omo/` (runtime state) |
| `.omo/start-work/` (5 entries) | yes | NO | `.git/info/exclude:10:/.omo/` (runtime state) |

`git ls-files AGENTS.md` and `git ls-files .agents .omo` exit 0 with
empty output (i.e. no tracked entries under those paths). Confirmed by
`git ls-files | grep -E "(__pycache__|\.codegraph|\.idea|\.playwright-mcp|boulder\.json|run-continuation|start-work)"` returning only the
two `__pycache__` entries (see below).

### Tracked Python bytecode (source)

Confirmed `git ls-files` includes:

- `scripts/__pycache__/ble_qa_scan.cpython-314.pyc` (54339 B)
- `tools/__pycache__/build_bundle.cpython-314.pyc` (4114 B)

Both are committed bytecode; the plan removes them in Task 2.

### Target canonical FAP directory inventory

`/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/`

| File | Mode | Size | Tracked? | Git blob SHA-1 | SHA-256 | Ignored? |
|------|------|------|----------|----------------|---------|----------|
| `application.fam` | `100644` | 261 B | **NO** | (untracked) | `36fcfc531911b053bb349f7a8bf9edeb3c8eb23afd1ba364de97fb79fac73872` | YES (`applications_user/.gitignore:1:*`) |
| `icon.png` | `100644` | 98 B | **NO** | (untracked) | `f49f7ebeab79e6e2bf7466367b5d88652929d649b836bf17c31dc954b249de2b` | YES (`applications_user/.gitignore:1:*`) |
| `pocket_airbridge.c` | `100644` | 72696 B | **YES** | `491664ff8b35c55a16bb97724f869fb7c10f5adc` | `ead16aede8ac810de5ca8eec3310c6f5cc8243ba0ac172867085d737f3263afe` | NO |

`git ls-files applications_user/pocket_airbridge/` returns only
`pocket_airbridge.c`. Confirmed `git check-ignore -v` matches both
`application.fam` and `icon.png` against `applications_user/.gitignore:1:*`; `pocket_airbridge.c` is not ignored.

### Target `applications_user/.gitignore` content

```
*
```

Single-line blanket ignore. Plan Task 1 step 3 requires deleting this
file before user Git gate 1 can stage `application.fam` and `icon.png`.

### Source root dotfile-aware inventory (Python `Path.iterdir`)

Sorted output of `os.listdir` on `/Users/asutov/projects/flipper-hid`:

```
.agents
.codegraph          -> /Users/asutov/.omo/codegraph/projects/flipper-hid-29f6718bef69022e (symlink)
.git
.gitignore
.idea
.omo
.playwright-mcp
AGENTS.md
README.md
config
dist
docs
firmware
scripts
tests
tools
web
```

Every root entry is dotfile-aware; no hidden file is missed. `.codegraph`
is a symlink, not a directory. `.idea/` and `.playwright-mcp/` are
empty/listing-populated (see ignored view above).

### Target root dotfile-aware inventory

Sorted output of `os.listdir` on `/Users/asutov/projects/flipperzero-firmware`:

```
.clang-format
.clangd
.editorconfig
.git
.gitattributes
.github
.gitignore
.gitmodules
.idea
.pvsconfig
.pvsoptions
.sconsign.dblite
.sublime-project
.vscode
CODE_OF_CONDUCT.md
CODING_STYLE.md
CONTRIBUTING.md
LICENSE
ReadMe.md
SConstruct
applications
applications_user
assets
build
dist
documentation
fbt
fbt.cmd
fbt_options.py
firmware.scons
furi
lib
scripts
site_scons
targets
toolchain
tsconfig.json
```

No `AGENTS.md`, `.agents/`, `.omo/`, or `airbridge/` at root — the
planned import is disjoint from existing tracked surface.

### Source scripts/tools tracked surface (relevant subset)

From `git ls-files scripts/ tools/`:

- `scripts/ble_qa_runbook.md`
- `scripts/ble_qa_scan.py`
- `tools/build_bundle.py`
- `tools/flipper_alive.py`
- `tools/gen_identity.py`
- `tools/usb_descriptor_capture.py`
- `tools/usb_descriptor_diff.py`
- `tools/usb_descriptor_fixture.py`
- `tools/usb_descriptor_macos.py`

Plus the two tracked bytecode files listed above.

### Drift vs. plan snapshot

| Snapshot claim | Runtime truth | Drift? |
|----------------|---------------|--------|
| Source branch `flipper-hid:master`, HEAD clean | confirmed `master`, clean | none |
| Target branch `pocket-airbridge`, HEAD clean | confirmed, clean | none |
| Source `.git/info/exclude` ignores `.codegraph`, `.agents/`, `AGENTS.md`, `.omo/`, `.playwright-mcp/` | confirmed (5 rules present) | none |
| `applications_user/.gitignore` is `*` | confirmed | none |
| Only `pocket_airbridge.c` tracked under `applications_user/pocket_airbridge/` | confirmed (single tracked file) | none |
| Source has tracked `scripts/__pycache__/ble_qa_scan.cpython-314.pyc` and `tools/__pycache__/build_bundle.cpython-314.pyc` | confirmed (both still tracked) | none |
| Target has no root `AGENTS.md`, `.agents/`, `.omo/` | confirmed (sorted root inventory shows none) | none |
| Target `.omo/boulder.json`, `.omo/run-continuation/`, `.omo/start-work/` to be excluded as runtime state | not present on target (excluded by disjoint-surface audit) | none |
| `.agents/skills/flipper-zero-dev/references/firmware-build-and-flash.md` and `scripts/ble_qa_runbook.md` present in source | both confirmed on disk and tracked | none |

No drift detected. Plan snapshot remains authoritative.

### Plan preconditions for Task 1

All required preconditions for proceeding to user Git gate 1 (commit
complete custom FAP on target) are verified:

1. Source tracked worktree is clean (no `--untracked-files=all` output).
2. Target tracked worktree is clean.
3. Source branch = `master`, target branch = `pocket-airbridge` (matches plan).
4. `applications_user/.gitignore` confirmed = `*`.
5. `applications_user/pocket_airbridge/` contains all three canonical
   FAP files on disk (two ignored, one tracked).
6. Plan-snapshot drift = none.

Task 1 agent preparation may proceed to filesystem edits
(`rm applications_user/.gitignore`) without re-prompting the user.

---

## 2026-09-01T22:21:00Z — Task 1, Step 3: Filesystem preparation — `.gitignore` deletion

### Action

Filesystem `rm` of `/Users/asutov/projects/flipperzero-firmware/applications_user/.gitignore`.
Single atomic delete. No `git rm`, no other file changes.

### Before-edit characterization (re-verified immediately before edit)

- HEAD: `55d841791a476e52a3306d44917bc5852f74e05f` (unchanged).
- `git status --short --untracked-files=all`: empty (worktree clean).
- `.gitignore` content: `*\n` (2 bytes; mode `100644`; mtime `Jun  8 18:44:48 2026`).
- `git check-ignore -v applications_user/pocket_airbridge/application.fam applications_user/pocket_airbridge/icon.png applications_user/pocket_airbridge/pocket_airbridge.c`:
  ```
  applications_user/.gitignore:1:*	applications_user/pocket_airbridge/application.fam
  applications_user/.gitignore:1:*	applications_user/pocket_airbridge/icon.png
  ```
  (`pocket_airbridge.c` returned no match — already tracked.)
- `git ls-files applications_user/pocket_airbridge/`: only `pocket_airbridge.c`.

### Pre-edit SHA-256 (matched against inventory notepad)

| File | SHA-256 | Inventory match |
|------|---------|-----------------|
| `application.fam` | `36fcfc531911b053bb349f7a8bf9edeb3c8eb23afd1ba364de97fb79fac73872` | YES |
| `icon.png` | `f49f7ebeab79e6e2bf7466367b5d88652929d649b836bf17c31dc954b249de2b` | YES |
| `pocket_airbridge.c` | `ead16aede8ac810de5ca8eec3310c6f5cc8243ba0ac172867085d737f3263afe` | YES |

All three pre-edit hashes equal inventory notepad values; no stale_state drift.

### After-edit verification

- `test -e applications_user/.gitignore` → `ABSENT`.
- `git check-ignore -v applications_user/pocket_airbridge/application.fam applications_user/pocket_airbridge/icon.png applications_user/pocket_airbridge/pocket_airbridge.c`: empty output, exit `1` (no match — fam, icon, and C all un-ignored).
- `git status --short`:
  ```
   D applications_user/.gitignore
  ?? applications_user/pocket_airbridge/application.fam
  ?? applications_user/pocket_airbridge/icon.png
  ?? applications_user/six_mouse/
  ```
- `git status --short --untracked-files=all` (the full expansion):
  ```
   D applications_user/.gitignore
  ?? applications_user/pocket_airbridge/application.fam
  ?? applications_user/pocket_airbridge/icon.png
  ?? applications_user/six_mouse/application.fam
  ?? applications_user/six_mouse/icon.png
  ?? applications_user/six_mouse/six_mouse.c
  ```
- `git diff --stat`: ` applications_user/.gitignore | 1 -  1 file changed, 1 deletion(-)`.
- `git diff --cached --stat`: empty, exit `0` (nothing staged).
- HEAD: `55d841791a476e52a3306d44917bc5852f74e05f` (unchanged).

### Post-edit SHA-256 (must equal pre-edit)

| File | SHA-256 | Equal to pre-edit? |
|------|---------|---------------------|
| `application.fam` | `36fcfc531911b053bb349f7a8bf9edeb3c8eb23afd1ba364de97fb79fac73872` | YES |
| `icon.png` | `f49f7ebeab79e6e2bf7466367b5d88652929d649b836bf17c31dc954b249de2b` | YES |
| `pocket_airbridge.c` | `ead16aede8ac810de5ca8eec3310c6f5cc8243ba0ac172867085d737f3263afe` | YES |

All three FAP files byte-identical before and after.

### Status scope interpretation

- ` D applications_user/.gitignore`: the only tracked-content change in
  the index — git knows `.gitignore` was tracked and the working tree
  is now missing it. Un-staged on purpose.
- `?? applications_user/pocket_airbridge/application.fam`: was previously
  hidden by the `applications_user/.gitignore:1:*` blanket; now visible
  as untracked. This is the intended FAP-file surfacing for user Git
  gate 1.
- `?? applications_user/pocket_airbridge/icon.png`: same reasoning.
- `?? applications_user/six_mouse/{application.fam,icon.png,six_mouse.c}`:
  incidental side-effect of removing the blanket `*` rule. six_mouse was
  also blanket-ignored by the same rule (inventory notepad recorded
  `!! applications_user/six_mouse/` under `--ignored`). Its content is
  unchanged; only its status moved from ignored to untracked. Plan scope
  is pocket_airbridge only; six_mouse visibility is an unavoidable
  consequence of removing the blanket ignore and is not a regression.
  Future gate work may decide what to do with six_mouse.

### Stage / commit / push state

- No `git add` performed.
- No commit performed.
- No remote interaction performed.
- Index dirty only by the `.gitignore` deletion (not yet staged).

### Adversarial-class outcomes

- `dirty_worktree`: status scope is exactly the four-line set above;
  nothing else changed.
- `stale_state`: HEAD + hashes re-checked immediately before edit;
  matched inventory values; HEAD unchanged after edit.
- `misleading_success_output`: post-condition verified by `check-ignore`
  (semantic, not file-existence); `.gitignore` absence verified by
  `test -e`; hashes by `shasum -a 256`.
- `mid_operation_interrupt`: single atomic `rm` (filesystem op); no
  intermediate state possible; verified complete-absent after.

### Outstanding

User Git gate 1 may now run:
`git -C /Users/asutov/projects/flipperzero-firmware add -A applications_user && git commit -m "feat(fap): track complete Pocket AirBridge application"`.
That gate is out of scope for this filesystem-prep unit.

---

## 2026-09-01T22:23:38Z — Task 1, Step 3 (correction): Whitelist `.gitignore` to scope airbridge only

### Rationale for correction

The previous filesystem `rm` of `applications_user/.gitignore` exposed
`six_mouse/{application.fam,icon.png,six_mouse.c}` as untracked because the
removed blanket `*` rule had been hiding them. The plan's gate
(`git add -A applications_user`) would have staged unrelated local
custom apps. That scope-creep was rejected: Pocket AirBridge is the only
canonical FAP for this monorepo migration; six_mouse is out of scope.

The fix is to restore `.gitignore` with a minimal whitelist that
re-includes only `pocket_airbridge/` while keeping everything else in
`applications_user/` ignored. The blanket-deletion approach is rejected
as too broad.

### New `.gitignore` content (3 lines, 42 bytes, 100644)

```
*
!pocket_airbridge/
!pocket_airbridge/**
```

SHA-256: `a5296aca26ad4c37dac33c220b5b0928b813d77f2e4bf5fd9311b3d9c55497b9`.

### Pre-correction re-baseline (re-verified immediately before edit)

- HEAD: `55d841791a476e52a3306d44917bc5852f74e05f` (still unchanged).
- `.gitignore`: ABSENT (still in the post-deletion state).
- `git status --short --untracked-files=all`:
  ```
   D applications_user/.gitignore
  ?? applications_user/pocket_airbridge/application.fam
  ?? applications_user/pocket_airbridge/icon.png
  ?? applications_user/six_mouse/application.fam
  ?? applications_user/six_mouse/icon.png
  ?? applications_user/six_mouse/six_mouse.c
  ```
- All three FAP hashes still match inventory.

### Post-correction verification

#### `git check-ignore -v` (with `--no-index` to bypass index hiding of tracked paths)

```
applications_user/.gitignore:3:!pocket_airbridge/**	applications_user/pocket_airbridge/
applications_user/.gitignore:3:!pocket_airbridge/**	applications_user/pocket_airbridge/application.fam
applications_user/.gitignore:3:!pocket_airbridge/**	applications_user/pocket_airbridge/icon.png
applications_user/.gitignore:3:!pocket_airbridge/**	applications_user/pocket_airbridge/pocket_airbridge.c
applications_user/.gitignore:1:*	applications_user/six_mouse/
applications_user/.gitignore:1:*	applications_user/six_mouse/application.fam
applications_user/.gitignore:1:*	applications_user/six_mouse/icon.png
applications_user/.gitignore:1:*	applications_user/six_mouse/six_mouse.c
```

Rule-3 matches for all four `pocket_airbridge/` paths (including the
directory itself and `pocket_airbridge.c`) — these are NEGATION matches,
so all four are NOT ignored.
Rule-1 matches for all four `six_mouse/` paths — these are POSITIVE
matches, so all four ARE ignored.

`pocket_airbridge/` directory: re-included (negation match on rule 3) — git
can now descend. `six_mouse/` directory: excluded (positive match on
rule 1) — git skips descent, so its contents are ignored by inheritance.

#### `git status --short --untracked-files=all`

```
 M applications_user/.gitignore
?? applications_user/pocket_airbridge/application.fam
?? applications_user/pocket_airbridge/icon.png
```

Exactly four outcomes: modification of the `.gitignore`, plus two newly
untracked pocket_airbridge files. NO six_mouse entries appear. NO deletion
marker (`D`) — the previous deletion has been undone by re-creating the file.

#### `git diff -- applications_user/.gitignore`

```
diff --git a/applications_user/.gitignore b/applications_user/.gitignore
index 72e8ffc0..7cb2ed76 100644
--- a/applications_user/.gitignore
+++ b/applications_user/.gitignore
@@ -1 +1,3 @@
 *
+!pocket_airbridge/
+!pocket_airbridge/**
```

Two added lines, no other change. `git diff --stat -- applications_user/.gitignore`:
` applications_user/.gitignore | 2 ++  1 file changed, 2 insertions(+)`.

#### SHA-256 (post-correction, must equal inventory baseline)

| File | SHA-256 | Equal to baseline? |
|------|---------|---------------------|
| `application.fam` | `36fcfc531911b053bb349f7a8bf9edeb3c8eb23afd1ba364de97fb79fac73872` | YES |
| `icon.png` | `f49f7ebeab79e6e2bf7466367b5d88652929d649b836bf17c31dc954b249de2b` | YES |
| `pocket_airbridge.c` | `ead16aede8ac810de5ca8eec3310c6f5cc8243ba0ac172867085d737f3263afe` | YES |

All three FAP files byte-identical.

#### Stage / commit / push state

- `git diff --cached --stat`: empty (nothing staged).
- HEAD: `55d841791a476e52a3306d44917bc5852f74e05f` (unchanged).
- No remote interaction performed.

### Adversarial-class outcomes

- `dirty_worktree`: status scope is exactly the three-line set above;
  nothing else changed.
- `stale_state`: HEAD + hashes re-checked immediately before edit;
  HEAD unchanged after edit.
- `misleading_success_output`: verified both whitelist and retained
  ignore behavior — check-ignore confirms pocket_airbridge is
  NEGATION-matched (un-ignored) and six_mouse is POSITIVE-matched
  (ignored). status confirms only pocket_airbridge appears in
  untracked view, no six_mouse.
- `mid_operation_interrupt`: write of the 42-byte file is a single
  atomic file write; post-write `read` confirms exactly the 3
  documented lines; status confirms the whitelist parses correctly.

### Cleanup receipt

No resources created (no build artifacts, no temp files, no server
processes). The only persistent state change on the target side is
the in-place edit of `applications_user/.gitignore`.

### Outstanding

User Git gate 1 may now run:
`git -C /Users/asutov/projects/flipperzero-firmware add applications_user/.gitignore applications_user/pocket_airbridge && git commit -m "feat(fap): track complete Pocket AirBridge application"`.

NOTE: the gate command is now scoped to pocket_airbridge only (NOT
`-A applications_user`); six_mouse stays untracked-and-ignored as
intended.

---

## 2026-09-02T00:34:00Z — Task 1, Step 3 (re-correction): User approved tracking `six_mouse/`, blanket `.gitignore` deleted

### User decision (verbatim)

> "six mouse is okay to keep as well, no need for gitignore"

This is a user-approved scope expansion: both `pocket_airbridge/` AND
`six_mouse/` are intentional custom apps to track. The previous
whitelist-based correction (2026-09-01T22:23:38Z) is intentionally
superseded. The blanket `applications_user/.gitignore` is now
removed entirely — `applications_user/` carries no gitignore rules at
all. Future custom apps dropped into this directory will surface as
untracked without needing per-app whitelist edits.

### Action

Filesystem `rm` of `/Users/asutov/projects/flipperzero-firmware/applications_user/.gitignore`.
Single atomic delete. No `git rm`. No other file changes anywhere
under `applications_user/`.

### Baseline (verified immediately before edit)

- HEAD: `9d266ccbcda27582cccb2a281963d4929f558fe0` (target pre-migration baseline).
- Branch: `pocket-airbridge`.
- Worktree status (`git status --short --untracked-files=all`): empty
  (clean).
- `.gitignore` content (3 lines, 42 bytes, 100644):
  ```
  *
  !pocket_airbridge/
  !pocket_airbridge/**
  ```
  SHA-256: `a5296aca26ad4c37dac33c220b5b0928b813d77f2e4bf5fd9311b3d9c55497b9`.
- `git ls-files applications_user/pocket_airbridge/` returns all three
  FAP files (tracked by the prior user commit):
  `application.fam`, `icon.png`, `pocket_airbridge.c`. Their HEAD
  blob SHAs are `c632d1e70f0414446b0e070b9837362255e2ce8a`,
  `e0631d873a8e8e9b094460c48381ca0ed497406e`, and
  `491664ff8b35c55a16bb97724f869fb7c10f5adc` respectively.
- `git ls-files applications_user/six_mouse/`: empty (six_mouse not yet
  tracked).
- `git check-ignore -v`:
  - `pocket_airbridge/{application.fam,icon.png,pocket_airbridge.c}`:
    no match (whitelist negation matched).
  - `six_mouse/{application.fam,icon.png,six_mouse.c}`:
    `applications_user/.gitignore:1:*` (blanket ignored).

### `applications_user/pocket_airbridge/` inventory (pre-edit)

| File | Mode | Size | SHA-256 | HEAD blob SHA-1 |
|------|------|------|---------|------------------|
| `application.fam` | `100644` | 261 B | `36fcfc531911b053bb349f7a8bf9edeb3c8eb23afd1ba364de97fb79fac73872` | `c632d1e70f0414446b0e070b9837362255e2ce8a` |
| `icon.png` | `100644` | 98 B | `f49f7ebeab79e6e2bf7466367b5d88652929d649b836bf17c31dc954b249de2b` | `e0631d873a8e8e9b094460c48381ca0ed497406e` |
| `pocket_airbridge.c` | `100644` | 72696 B | `ead16aede8ac810de5ca8eec3310c6f5cc8243ba0ac172867085d737f3263afe` | `491664ff8b35c55a16bb97724f869fb7c10f5adc` |

### `applications_user/six_mouse/` inventory (pre-edit)

| File | Mode | Size | SHA-256 |
|------|------|------|---------|
| `application.fam` | `100644` | 234 B | `b51396ce95e97f49a54e5879ca0fb2e0d6eb8d4298debef88d93c3f433872a92` |
| `icon.png` | `100644` | 105 B | `04cd9b0f5dbafe284439e899985f13829c9b9cae04ba98c166c015a0fa3fb5b8` |
| `six_mouse.c` | `100644` | 6527 B | `930107839f47a618bc61ba03e8666e215484657ffe271c8f71f4dbbe20c0164d` |

### Post-edit verification

- `test -e applications_user/.gitignore` → `ABSENT`.
- HEAD: `9d266ccbcda27582cccb2a281963d4929f558fe0` (unchanged).
- `git status --short`:
  ```
   D applications_user/.gitignore
  ?? applications_user/six_mouse/
  ```
- `git status --short --untracked-files=all` (full expansion):
  ```
   D applications_user/.gitignore
  ?? applications_user/six_mouse/application.fam
  ?? applications_user/six_mouse/icon.png
  ?? applications_user/six_mouse/six_mouse.c
  ```
- `git status --short --ignored`: six_mouse no longer present
  (only standard build/runtime ignores remain: `.idea/`,
  `.sconsign.dblite`, `__pycache__/` trees, `build/`, `dist/`,
  `toolchain/`).
- `git check-ignore -v` for both app trees: empty output, exit `1`
  (neither `pocket_airbridge/` nor `six_mouse/` matches any ignore
  rule).
- `git diff --stat`: ` applications_user/.gitignore | 3 ---  1 file
  changed, 3 deletions(-)`. ONLY the deleted `.gitignore`.
- `git diff --cached --stat`: empty (nothing staged).

### `applications_user/pocket_airbridge/` SHA-256 (post-edit, must equal pre-edit)

| File | SHA-256 | Equal to pre-edit? |
|------|---------|--------------------|
| `application.fam` | `36fcfc531911b053bb349f7a8bf9edeb3c8eb23afd1ba364de97fb79fac73872` | YES |
| `icon.png` | `f49f7ebeab79e6e2bf7466367b5d88652929d649b836bf17c31dc954b249de2b` | YES |
| `pocket_airbridge.c` | `ead16aede8ac810de5ca8eec3310c6f5cc8243ba0ac172867085d737f3263afe` | YES |

All three Pocket AirBridge files byte-identical pre/post.

### `applications_user/six_mouse/` SHA-256 (post-edit, must equal pre-edit)

| File | SHA-256 | Equal to pre-edit? |
|------|---------|--------------------|
| `application.fam` | `b51396ce95e97f49a54e5879ca0fb2e0d6eb8d4298debef88d93c3f433872a92` | YES |
| `icon.png` | `04cd9b0f5dbafe284439e899985f13829c9b9cae04ba98c166c015a0fa3fb5b8` | YES |
| `six_mouse.c` | `930107839f47a618bc61ba03e8666e215484657ffe271c8f71f4dbbe20c0164d` | YES |

All three six_mouse files byte-identical pre/post (only their
visibility moved from ignored to untracked).

### Stage / commit / push state

- No `git add` performed.
- No commit performed.
- No remote interaction performed.
- Index dirty only by the `.gitignore` working-tree deletion (un-staged).

### `applications_user/` full scope audit

`git ls-tree HEAD applications_user/` returns:

```
100644 blob 7cb2ed761caa20d8d2d373eb3db323e561f36635	applications_user/.gitignore
100644 blob 8bb7823c1a554cf168f9e3b7d89ea92a87b905fb	applications_user/README.md
040000 tree ff31c603ae6a252e0ee1bd136128ffc6ca0ab817	applications_user/pocket_airbridge
```

The deletion touched ONLY the `.gitignore` blob; `applications_user/README.md`
is tracked and unchanged (HEAD blob SHA-1 unchanged, on-disk SHA-256
`1656c0c8375114a0c6100efb91422001a843e88526e30475541395ab873d679f`
verified). No other applications_user content delta.

### Adversarial-class outcomes

- `dirty_worktree`: exact status scope is the four-line set above
  (one `D`, three `??`). No other path changes anywhere.
- `stale_state`: HEAD, tracked-tree, and pre/post hashes all
  re-verified immediately before and after the edit; HEAD SHA
  unchanged; Pocket AirBridge blob SHAs unchanged.
- `misleading_success_output`: verified by three independent
  mechanisms: (1) `test -e .gitignore` for absence, (2)
  `git check-ignore -v` returning exit `1` with empty output for both
  app trees, (3) `git status --short --ignored` excluding six_mouse.
- `scope_creep`: every `D` and `??` path enumerated; no others.
- `mid_operation_interrupt`: single atomic `rm`; post-edit
  re-confirmation shows `.gitignore` absent, no partial replacement,
  no temp files.

### Cleanup receipt

No resources created (no build artifacts, no temp files, no server
processes). The only persistent state change on the target side is
the in-place deletion of `applications_user/.gitignore`.

### Outstanding

User Git gate 1 (revised) may now run with full scope:
`git -C /Users/asutov/projects/flipperzero-firmware add -A applications_user && git commit -m "feat(fap): track complete Pocket AirBridge application"` —
the staged set will now include `.gitignore` deletion,
`pocket_airbridge/*` (already tracked), AND `six_mouse/*` (now
untracked). The user's blanket-approval intent makes this safe.

---

## 2026-09-01T22:52:43Z — Task 2 filesystem-only source restructure

### Scope and preconditions

- Source path: `/Users/asutov/projects/flipper-hid`.
- Branch: `master`.
- HEAD: `9ae70e3048cd197a351cac95100c8337bc9c7c6b` (exact required baseline).
- Baseline `git status --porcelain=v1 --untracked-files=all`: empty.
- Baseline tracked count: 42 files.
- Baseline `git ls-files -s` manifest SHA-256:
  `76a94d08263773f6bee0777b6d32dd78476183414b03133c9a9bf0edf4d73096`.
- Baseline product manifest: 42 files, SHA-256
  `d555ac207cc2d6bca9f56f8887714a6afd6b58398548898be803c8a2931eb434`.
- All ten required source entries existed and `airbridge/` did not exist before
  the operation: `web`, `docs`, `scripts`, `tools`, `tests`, `config`, `dist`,
  `firmware`, `README.md`, `.gitignore`.

### Filesystem actions

1. Created `airbridge/` using plain `mkdir`.
2. Moved exactly `web`, `docs`, `scripts`, `tools`, `tests`, `config`, `dist`,
   `firmware`, and `README.md` into `airbridge/` using plain `mv`.
3. Moved root `.gitignore` to `airbridge/.gitignore` using plain `mv`.
4. Appended only `__pycache__/` to that moved file. Its previous three lines
   remain byte-for-byte unchanged. Pre-edit SHA-256 was
   `3a1896900ca25aabba6f8c5a9d975ce3aa80ecb09d4f90f2392d0dc389efdc73`;
   post-edit SHA-256 is
   `623805cfbc321b27095013ae762c52ff41c26cb506b6444172723ba80ee82845`.
5. Deleted only the two known tracked bytecode directories with plain `rm`:
   - `airbridge/scripts/__pycache__/ble_qa_scan.cpython-314.pyc`
     (`3bae0b8c08dc0fd8c0d5f8046ee599e51962787ed103028d8d85d50ae29d9b9e`)
   - `airbridge/tools/__pycache__/build_bundle.cpython-314.pyc`
     (`1964f5626763685e161d4fa6db9f9ba8b4d6bd90b77f6b126a7d526a5552629d`)

No Git mutation, staging, commit, build, server, browser, or hardware command ran.

### Sorted `Path.iterdir()` inventories after the operation

Source root (exact allowlist):

```text
.agents/
.codegraph -> /Users/asutov/.omo/codegraph/projects/flipper-hid-29f6718bef69022e
.git/
.idea/
.omo/
.playwright-mcp/
AGENTS.md
airbridge/
```

`airbridge/` (exact allowlist):

```text
.gitignore
README.md
config/
dist/
docs/
firmware/
scripts/
tests/
tools/
web/
```

### Byte-identity and preservation proof

- Post-move product count: 40 files (42 baseline files minus the two intentional
  `.pyc` deletions).
- 39 unaffected product files (all post-move files except `.gitignore`) match their
  pre-move size and SHA-256 exactly.
- Normalized pre/post manifest SHA-256 for those 39 files:
  `6271a6b2f19674d5618aae2544931a7fc41129965c00292cfd24b61e00127ee0`.
- `airbridge/.gitignore` is exactly:
  `.idea/`, `.DS_Store`, `.playwright-mcp/`, `__pycache__/` (one line each).
- Preservation manifest covered `AGENTS.md`, both complete root project skill trees,
  `.omo/boulder.json`, `.omo/run-continuation/`, and `.omo/start-work/`: 466 files,
  pre/post SHA-256
  `6246b8bff64393b8e2a0e98f40ab87fdf559a60f4ce6868ea5d4dbcb128eb729`.
- `.omo/plans/`, `.omo/notepads/`, `.omo/evidence/`, and `.omo/drafts/` remain present.
  This append-only evidence block is the sole intended `.omo/notepads/` write.

### Read-only Git proof

- Post-edit index is unchanged: 42 tracked entries and the same index-manifest
  SHA-256 `76a94d08263773f6bee0777b6d32dd78476183414b03133c9a9bf0edf4d73096`.
- Cached/staged diff is empty.
- Exact porcelain-set comparison found every expected entry and no missing entry:
  42 unstaged deletions at former tracked paths and 40 untracked files at their new
  `airbridge/` paths. The only additional visible paths are the five retained root
  IDE runtime files (`.idea/.gitignore`, `flipper-hid.iml`, `misc.xml`, `modules.xml`,
  `vcs.xml`), which necessarily surface because the root `.gitignore` was moved and
  `.idea/` was explicitly required to remain at root. No product or metadata drift
  exists outside this expected surface.
- Read-only `git diff --summary` lists deletion of exactly the 42 former tracked paths;
  no staged changes exist.
- `git check-ignore -v --no-index` proves all four moved rules match representative
  paths under `airbridge/`, including `__pycache__/`.

### Adversarial outcomes and cleanup

- `dirty_worktree`: exact expected move/delete/untracked set proved; retained `.idea`
  surfacing is identified explicitly rather than mistaken for product drift.
- `stale_state`: branch, exact HEAD, clean tracked state, all source entries, and
  destination absence were checked immediately before filesystem operations.
- `generated/cached artifacts`: only the two known tracked `.pyc` files were removed;
  active `.omo` runtime hashes are unchanged.
- `misleading_success_output`: normalized per-file SHA-256 comparison, manifest
  digests, exact inventories, index digest, and ignore-rule parsing back every claim.
- `mid-operation interrupt`: post-operation source/destination allowlists and complete
  path-set comparison prove no duplicates, omissions, or partial moves.
- Temporary pre-move manifest was stored only outside the repository and removed after
  evidence capture. No process, build, browser, server, or other QA asset was created.
