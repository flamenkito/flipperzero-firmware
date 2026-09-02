# Issues — Pocket AirBridge Monorepo Merge

Append-only ledger of issues, drift, and runtime concerns encountered
during execution. Each block is timestamped; never overwrite prior
entries.

---

## 2026-09-01T22:13:15Z — Task 1, Step 1: Inventory of both repositories

### Issue: Target FAP is only one-third reproducible from a fresh checkout

**Severity:** blocking for Task 1 acceptance.

**Detail:** `applications_user/pocket_airbridge/` contains three
canonical files (`application.fam`, `icon.png`, `pocket_airbridge.c`)
but `git ls-files applications_user/pocket_airbridge/` returns only
`pocket_airbridge.c`. `git check-ignore -v` confirms both
`application.fam` and `icon.png` are excluded by the blanket
`applications_user/.gitignore:1:*` rule.

**Consequence:** a fresh checkout of `pocket-airbridge` HEAD
(`55d841791a476e52a3306d44917bc5852f74e05f`) yields a broken FAP build
because `application.fam` and `icon.png` are missing from the working
tree until re-applied from somewhere outside Git.

**Resolution path:** Task 1 step 3 (delete `applications_user/.gitignore`)
+ user Git gate 1 (`git add -A applications_user && git commit -m
"feat(fap): track complete Pocket AirBridge application"`). Plan is
already explicit.

### Issue: Source project metadata is repository-local-only by `.git/info/exclude`

**Severity:** blocking for Task 2 acceptance.

**Detail:** `AGENTS.md`, `.agents/`, and `.omo/` exist on disk but are
excluded by `.git/info/exclude` (lines 8–10). None of these paths
appears in `git ls-files`. The plan requires all three to be tracked
for fresh-checkout reproducibility, which forces `git add -f` on the
user side.

**Consequence:** without explicit force-add, a fresh source clone
loses `AGENTS.md`, both project skills, the merge plan, and every
notepad under `.omo/notepads/`. This is not yet an in-band blocker
because the merge plan lives in `.omo/` and Task 3 has not run.

**Resolution path:** Task 2 step 6 (delete runtime `.omo` state) +
user Git gate 2 (`git add -f AGENTS.md .agents .omo/plans
.omo/notepads .omo/evidence`). Plan is explicit.

### Issue: Tracked Python bytecode in source repo

**Severity:** minor (cleanup), not blocking.

**Detail:** Two `.pyc` files remain tracked:

- `scripts/__pycache__/ble_qa_scan.cpython-314.pyc` (54339 B)
- `tools/__pycache__/build_bundle.cpython-314.pyc` (4114 B)

**Consequence:** plan Task 2 step 4 removes the directories and step 5
of Task 2 ensures they do not return. Not blocking Task 1, but it is a
direct hit during the source restructure.

**Resolution path:** Task 2 step 4 deletes the `__pycache__/` dirs and
the `git rm --cached` is implicit in the user commit.

### Issue: Runtime `.omo` state (boulder.json, run-continuation, start-work) is large

**Severity:** informational.

**Detail:** `.omo/boulder.json` is 31492 B; `.omo/run-continuation/`
holds 381 entries; `.omo/start-work/` holds 5 entries. All are excluded
by `.git/info/exclude:10:/.omo/` and therefore not tracked.

**Consequence:** Task 2 step 6 requires deleting these three
subtrees before user Git gate 2. Without deletion, a `git add -f
.omo/...` of the parent could re-introduce them into the user's intent;
but because the plan force-adds only `.omo/plans .omo/notepads
.omo/evidence`, those subtrees stay untracked regardless. Deletion is
still required by the plan for hygiene.

**Resolution path:** Task 2 step 6 explicit filesystem delete. Plan is
explicit.

### Issue: `flamenkito/flipperzero-firmware` is a public remote on target

**Severity:** informational (no current action required).

**Detail:** `git -C /Users/asutov/projects/flipperzero-firmware remote
-v` shows the public GitHub remote. Plan never pushes; if a future
push is desired, the remote must first be made private or a
full-history privacy/secret scan must run. This plan never reaches a
push step.

**Resolution path:** none required for current plan execution.

### Issue: `.codegraph` is a symlink to a user-local cache directory

**Severity:** informational (excluded from import by plan).

**Detail:** `/Users/asutov/projects/flipper-hid/.codegraph` is a symlink
to `/Users/asutov/.omo/codegraph/projects/flipper-hid-29f6718bef69022e`.
It is ignored by `.git/info/exclude:7:.codegraph`. Plan deliberately
does not import it; the new Codegraph index must be initialized at the
target repo after cutover.

**Resolution path:** Task 5 last bullet ("Initialize Codegraph for the
target repo if desired; do not reuse the source `.codegraph` symlink").

### Issue: `.idea/` directories exist in both repos

**Severity:** informational (already ignored).

**Detail:** Both repos show `!! .idea/` in `--ignored` output. Source
`.idea/` is empty (3 entries); target `.idea/` ignored as well. No
agent action needed.

**Resolution path:** none required.

### Issue: No adversarial-class failures detected

- `dirty_worktree`: clean in both repos; no blocking state.
- `stale_state`: snapshot drift = none across all eight snapshot
  claims verified.
- `misleading_success_output`: every claim of "tracked" / "ignored"
  is backed by `git ls-files` and `git check-ignore -v` outputs, not
  by file-existence alone.

No further adversarial classes apply because this task is read-only.

---

## 2026-09-01T22:21:00Z — Task 1, Step 3: Filesystem-prep completed, no issues

### Confirmation

`applications_user/.gitignore` deleted via filesystem `rm`. No issues
encountered. Six adversarial-class outcomes recorded in `decisions.md`
all clean.

### Note (informational): six_mouse also surfaces after blanket removal

**Severity:** informational, not blocking.

**Detail:** The removed blanket `applications_user/.gitignore:1:*` rule
also hid everything under `applications_user/six_mouse/`. After deletion,
`six_mouse/application.fam`, `six_mouse/icon.png`, and `six_mouse/six_mouse.c`
are visible as untracked. This is an unavoidable side-effect of removing
the blanket ignore and was anticipated by the original inventory
(`!! applications_user/six_mouse/` under `--ignored`).

**Consequence:** none for Task 1, which is scoped to pocket_airbridge
only. A future gate may decide whether to track six_mouse, leave it
untracked, or delete it.

### Issue: Target side `git add -A` would accidentally stage six_mouse

**Severity:** blocking for user Git gate 1 if it uses `-A` (capture-all).

**Detail:** When the user runs `git add -A applications_user`, the
intentional pocket_airbridge additions will be staged along with the
incidental six_mouse untracked files. If six_mouse is meant to stay
untracked, the gate command should scope to pocket_airbridge only.

**Resolution path:** user Git gate 1 should use either
`git add applications_user/.gitignore applications_user/pocket_airbridge`
or `git add -A applications_user/pocket_airbridge` to avoid pulling in
six_mouse. The blanket `git add -A applications_user` would stage
six_mouse too. This advisory will be visible to the user in the
decisions notepad.

---

## 2026-09-01T22:23:38Z — Issue: previous blanket-deletion approach rejected for scope creep

**Severity:** blocking for user Git gate 1 acceptance of Task 1.

**Detail:** The previous step (2026-09-01T22:21:00Z) deleted
`applications_user/.gitignore` entirely. That was the wrong scope:
removing the blanket `*` rule exposed `six_mouse/{application.fam,
icon.png,six_mouse.c}` as untracked, and a follow-up `git add -A
applications_user` would have staged unrelated local custom apps.

The plan's locked intent is "Pocket AirBridge is canonical and must be
reproducible from Git; other local custom apps are out of scope for
this monorepo migration." A blanket-deletion of the ignore file violates
that scope.

**Resolution applied:** `.gitignore` has been re-created with a minimal
whitelist (3 lines: `*`, `!pocket_airbridge/`, `!pocket_airbridge/**`).
This restores the blanket-ignore behavior for everything except
pocket_airbridge. Post-correction status shows only pocket_airbridge
files as untracked; six_mouse files no longer appear.

**Status:** resolved by the 2026-09-01T22:23:38Z corrective edit. No
further action required from this session.

---

## 2026-09-02T00:34:00Z — Issue: whitelist-based correction superseded by user scope expansion

**Severity:** superseded; not blocking.

**Detail:** The previous corrective edit (2026-09-01T22:23:38Z)
re-created `applications_user/.gitignore` with a 3-line whitelist
(`*`, `!pocket_airbridge/`, `!pocket_airbridge/**`) to keep six_mouse
ignored while still surfacing pocket_airbridge files. That scope was
inferred by the agent.

The user has now explicitly clarified: `six mouse is okay to keep as
well, no need for gitignore`. Both custom apps (`pocket_airbridge/`
and `six_mouse/`) are intentional and to be tracked. The whitelist
approach is rejected in favor of blanket `.gitignore` removal.

**Resolution applied (2026-09-02T00:34:00Z):**
`applications_user/.gitignore` deleted via filesystem `rm`. six_mouse
files now visible as untracked; pocket_airbridge hashes unchanged;
HEAD unchanged. No remaining ignore rule at `applications_user/`.

**Status:** resolved. Future custom apps dropped into
`applications_user/` will surface as untracked without any
gitignore edits.

### Issue (informational): Pocket AirBridge tree state vs. prior recording

**Severity:** informational, not blocking.

**Detail:** The 2026-09-01T22:21:00Z decision recorded
`applications_user/pocket_airbridge/application.fam` and
`.../icon.png` as untracked. The 2026-09-01T22:23:38Z correction
carried that forward. At task re-entry
(2026-09-02T00:34:00Z), the user had already committed the complete
Pocket AirBridge directory; `git ls-files
applications_user/pocket_airbridge/` now returns all three files
(`application.fam`, `icon.png`, `pocket_airbridge.c`).

**Resolution:** accepted as user-driven intermediate progress. The
HEAD SHA `9d266ccbcda27582cccb2a281963d4929f558fe0` is recorded as
the new pre-import baseline. No rewind required; no conflict with
the current task scope.
