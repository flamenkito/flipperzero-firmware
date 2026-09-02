# Sisyphus Plan — Pocket AirBridge Custom Firmware Monorepo

## Objective

Merge `/Users/asutov/projects/flipper-hid` into
`/Users/asutov/projects/flipperzero-firmware` as one **local custom firmware/apps
profile**. The target branch is `pocket-airbridge`; product assets live under
`airbridge/`, while the FAP remains in the firmware-native
`applications_user/pocket_airbridge/` location.

After completion, one local repository contains firmware patches, custom USB/BLE
profiles, the FAP, web application, docs, tools, tests, agent skills, and durable
`.omo` project memory. The old clone becomes read-only archival material.

## User Decisions Already Locked

- This is a custom firmware/apps distribution, not a contribution intended for
  official upstream.
- Development is local-only. **No push is part of this plan.**
- Custom applications should be tracked inside the firmware repository.
- Root `.agents/skills/` must be imported and become the canonical project skills.
- Every Git mutation is executed manually by the user. Agents may only use Git
  read-only (`status`, `diff`, `log`, `ls-files`, `ls-tree`, `check-ignore`).

## Review Status

Initial plan verdicts were **REJECT** from Momus and Metis. This revision addresses
their verified blockers:

1. `AGENTS.md`, `.agents/`, and `.omo/` are ignored by
   `flipper-hid/.git/info/exclude` and require explicit user `git add -f`.
2. `applications_user/.gitignore` initially contained `*`; only
   `pocket_airbridge.c` was tracked. The complete `pocket_airbridge/` and
   `six_mouse/` custom apps must become tracked for fresh-checkout reproducibility.
3. `.omo/boulder.json`, `.omo/run-continuation/`, and `.omo/start-work/` are runtime
   state and must not become durable tracked project memory.
4. Plain `ls` cannot verify hidden files; all root-surface checks use a sorted
   dotfile-aware inventory.
5. The path sweep includes previously omitted skill reference
   `firmware-build-and-flash.md` and `scripts/ble_qa_runbook.md`.
6. Firmware build and physical deployment are serialized; F2 deploys the exact F1
   artifact by recorded size and SHA-256.

## Verified Starting Facts

- Source branch: `flipper-hid:master`.
- Target branch: `flipperzero-firmware:pocket-airbridge`.
- Both tracked worktrees were clean at review time; execution must re-check current
  state rather than trust this snapshot.
- Source `.git/info/exclude` ignores `.codegraph`, `.agents/`, `AGENTS.md`, `.omo/`,
  and `.playwright-mcp/`; none of `AGENTS.md`, `.agents/`, or `.omo/` is currently
  tracked.
- Source has tracked Python bytecode:
  `scripts/__pycache__/ble_qa_scan.cpython-314.pyc` and
  `tools/__pycache__/build_bundle.cpython-314.pyc`; these are removed during the
  restructure.
- Target `applications_user/.gitignore` is `*`. Currently tracked FAP content is
  only `pocket_airbridge.c`; `application.fam` and `icon.png` are local-only files.
- Target has no root `AGENTS.md`, `.agents/`, or `.omo/`, so the intended imported
  root surface is disjoint.
- The temporary local Git remote will point to `/Users/asutov/projects/flipper-hid`
  and will be removed after provenance verification.
- `flamenkito/flipperzero-firmware` is public while `flipper-airbridge` is private.
  This plan never pushes. If pushing is ever desired later, first make the target
  remote private or perform a full-history privacy/secret scan.

## Target Layout

```text
flipperzero-firmware/
├── applications_user/
│   ├── pocket_airbridge/
│   │   ├── application.fam
│   │   ├── icon.png
│   │   └── pocket_airbridge.c
│   └── six_mouse/
│       ├── application.fam
│       ├── icon.png
│       └── six_mouse.c
├── applications/ targets/ lib/ furi/ ...
├── airbridge/
│   ├── web/ docs/ scripts/ tools/ tests/ config/ dist/
│   ├── README.md
│   └── .gitignore
├── AGENTS.md
├── .agents/skills/
├── .omo/
│   ├── plans/
│   ├── notepads/
│   └── evidence/
└── .gitignore
```

Not imported: source `.git/`, `.idea/`, `.playwright-mcp/`, `.codegraph` symlink,
`.omo/boulder.json`, `.omo/run-continuation/`, `.omo/start-work/`, and `.omo/drafts/`
unless the user explicitly promotes a draft into `plans/` before Phase 1.

## Hard Constraints

- **No agent-run Git mutations.** This includes `add`, `commit`, `checkout`,
  `switch`, `stash`, `merge`, `fetch`, `remote`, `rm`, `mv`, `reset`, `rebase`,
  `push`, worktree creation, PR creation, or automatic merge.
- This plan is an explicit exception to `/start-work` defaults involving worktrees,
  PRs, auto-commits, or merges.
- User Git command blocks must be absolute-path anchored, fail-fast with `&&`, and
  followed by independent read-only verification.
- No stashing: plan-critical state is committed by the user or execution stops.
- From the Phase 2 cutover onward, all edits, checkboxes, notepads, and evidence go
  to the target monorepo copy only. The source clone becomes read-only.
- The root `.agents/skills/` import is mandatory and verified with `git ls-files` and
  post-merge file reads.
- No push, no GitHub PR, and no official-upstream operation occurs in this plan.
- Physical/browser actions follow root `AGENTS.md`: persistent Chrome via CDP 9222,
  alert before every physical gate, `question` tool for every user action, exact
  browser window naming, and evidence outside tracked product paths.

## TODOs

- [x] 1. **Preflight and make custom apps fully reproducible:** inventory tracked,
  untracked, and ignored state in both repos; record branches and HEADs; remove the
  blanket `applications_user/.gitignore`; user manually commits complete
  `pocket_airbridge/` and `six_mouse/` app directories and verifies them with
  `git ls-tree`.
- [ ] 2. **Restructure the source repo and explicitly track project metadata:** move
  product content under `airbridge/` using filesystem operations only; remove tracked
  bytecode; retain root `AGENTS.md`, `.agents/`, and curated `.omo`; user manually
  stages ignored metadata with `git add -f`, excludes runtime `.omo` state, and
  commits the restructure.
- [ ] 3. **Merge histories manually and cut execution over to the monorepo:** user
  manually adds/verifies the temporary local remote, fetches, and merges unrelated
  histories on `pocket-airbridge`; orchestrator verifies both histories, required
  skills, full FAP assets, and no firmware-tree changes caused by the import; all
  subsequent work uses the target copy.
- [ ] 4. **Update paths, docs, tools, skills, and runtime ignores:** parallel agents
  update disjoint code/tool and documentation/skill surfaces, remove the superseded
  patch bundle, establish one canonical web serve command, and ignore runtime `.omo`
  and Playwright state at the monorepo root.
- [ ] 5. **Commit local monorepo state and complete cutover:** user manually commits
  Phase 4 changes and removes the temporary local remote; no push; initialize a new
  Codegraph/OpenCode/IDE context at the monorepo and mark the old clone read-only.

## Execution Map

| Task | Dependency | Executor | Git mutation |
|---|---|---|---|
| 1 | none | agent filesystem prep + user gate | user commit only |
| 2 | Task 1 | quick agent + user gate | user add/force-add/commit |
| 3 | Task 2 | user gate + orchestrator | user remote/fetch/merge |
| 4 | Task 3 cutover | quick ∥ writing | none by agents |
| 5 | Task 4 verified | user gates + orchestrator | user commit/remove remote |
| F1 | Task 5 | firmware reviewer | read-only + build |
| F2 | F1 artifact | hardware QA reviewer | no Git mutation |
| F3 | Task 5 | truthfulness reviewer | read-only |
| F4 | F1–F3 | topology reviewer | read-only |

## Manual Git Ledger

These are the only Git mutations in the plan, all typed by the user after a
`question` gate:

1. Target repo: commit complete Pocket AirBridge assets, then commit removal of the
   blanket ignore plus the complete `six_mouse/` custom app.
2. Source repo: explicitly stage `AGENTS.md`, `.agents/`, and curated `.omo` with
   force-add; commit the restructure.
3. Target repo: add/verify temporary local remote, fetch it, and merge
   `airbridge/master` with unrelated histories allowed.
4. Target repo: commit path/docs/tool/ignore updates.
5. Target repo: remove the temporary local remote.

No push is permitted by this plan.

---

## Task 1 — Preflight and Reproducible Custom FAP

### Agent preparation

1. Record runtime inventories for both repos:
   - `git status --short --untracked-files=all`
   - `git status --short --ignored`
   - branch name and HEAD SHA
   - required ignored/untracked asset list
2. Abort if either repo has unresolved tracked changes. Do not offer stash.
3. In the target repo, delete `applications_user/.gitignore`. This custom firmware
   profile intentionally tracks both current custom app directories.
4. Verify the canonical FAP directory contains at least:
   `application.fam`, `icon.png`, and `pocket_airbridge.c`.
5. Append the two pre-migration HEAD SHAs and FAP inventory to
   `.omo/notepads/monorepo-merge/decisions.md` in the source repo.

### User Git gate 1

Pocket AirBridge assets were committed first as
`9d266ccbcda27582cccb2a281963d4929f558fe0`. After the user expanded scope to
include `six_mouse/`, the remaining manual gate is:

After the attention signal and `question` prompt, the user runs in an absolute-path
anchored shell:

```bash
cd /Users/asutov/projects/flipperzero-firmware && \
test "$(pwd -P)" = "/Users/asutov/projects/flipperzero-firmware" && \
test "$(git branch --show-current)" = "pocket-airbridge" && \
git add -A applications_user && \
git commit -m "feat(apps): track six_mouse custom app"
```

If nothing needs committing because these files were already fixed manually, the user
selects that option and the orchestrator verifies state rather than requiring an empty
commit.

### Acceptance

- Read-only `git status --short --untracked-files=all` is clean.
- `git ls-tree -r HEAD applications_user/pocket_airbridge` includes all three FAP
  files and `git ls-tree -r HEAD applications_user/six_mouse` includes all three
  six_mouse files.
- `git check-ignore` reports that none of either app's files is ignored.
- Recorded target HEAD becomes the immutable pre-import firmware baseline.

## Task 2 — Restructure Source and Track Skills/Memory

### Agent filesystem work

In `/Users/asutov/projects/flipper-hid`, without Git mutations:

1. Create `airbridge/`.
2. Move `web`, `docs`, `scripts`, `tools`, `tests`, `config`, `dist`, `firmware`, and
   `README.md` into `airbridge/`.
3. Move the source root `.gitignore` to `airbridge/.gitignore`; ensure it ignores
   `.idea/`, `.DS_Store`, `.playwright-mcp/`, and `__pycache__/`.
4. Delete tracked `__pycache__` directories using plain filesystem removal.
5. Leave root `AGENTS.md`, `.agents/`, and `.omo/` in place.
6. Preserve active source runtime state (`.omo/boulder.json`,
   `.omo/run-continuation/`, and `.omo/start-work/`) until the Phase 2 execution
   cutover, but do NOT force-add or import it. Import only `plans/`, `notepads/`, and
   `evidence/`; preserve `drafts/` only if the user explicitly opts in.
7. Verify the source root with a sorted dotfile-aware Python `Path.iterdir()` inventory,
   not plain `ls`.

### User Git gate 2

The user runs:

```bash
cd /Users/asutov/projects/flipper-hid && \
test "$(pwd -P)" = "/Users/asutov/projects/flipper-hid" && \
test "$(git branch --show-current)" = "master" && \
git add -A -- .gitignore README.md config dist docs firmware scripts tests tools web airbridge && \
git add -f AGENTS.md .agents .omo/plans .omo/notepads .omo/evidence && \
git commit -m "refactor: prepare Pocket AirBridge for firmware monorepo"
```

### Acceptance

- `git ls-files --error-unmatch AGENTS.md` passes.
- `git ls-files --error-unmatch .agents/skills/flipper-zero-dev/SKILL.md` passes.
- `git ls-files --error-unmatch .agents/skills/pocket-airbridge-hardware-qa/SKILL.md`
  passes.
- `git ls-files --error-unmatch .omo/plans/monorepo-merge.md` passes.
- `git ls-files` contains no `boulder.json`, `run-continuation`, `start-work`,
  `__pycache__`, `.playwright-mcp`, `.idea`, or `.codegraph` entries.
- Read-only source `git status --short --untracked-files=all` is clean except for
  intentionally untracked root `.idea/` local tooling files, which MUST remain absent
  from the commit.

## Task 3 — Manual Merge and Execution Cutover

### Preconditions

- Record expected source restructure HEAD and target post-FAP HEAD.
- Target branch is `pocket-airbridge` and tracked state is clean.
- If remote `airbridge` already exists, orchestrator first reads its URL. A mismatch
  is resolved manually by the user before continuing.

### User Git gate 3

The user runs the fail-fast block:

```bash
cd /Users/asutov/projects/flipperzero-firmware && \
test "$(pwd -P)" = "/Users/asutov/projects/flipperzero-firmware" && \
test "$(git branch --show-current)" = "pocket-airbridge" && \
test -z "$(git status --porcelain)" && \
(git remote get-url airbridge >/dev/null 2>&1 || \
  git remote add airbridge /Users/asutov/projects/flipper-hid) && \
test "$(git remote get-url airbridge)" = "/Users/asutov/projects/flipper-hid" && \
git fetch airbridge && \
git merge --allow-unrelated-histories --no-edit airbridge/master
```

Recovery is explicit:

- Conflict: user runs `git merge --abort`; execution returns to the precondition audit.
- Wrong remote: user manually removes/re-adds it after the orchestrator reports the
  observed URL.
- Already merged: orchestrator verifies ancestry and skips the duplicate merge.

### Acceptance

- `git merge-base --is-ancestor <source-restructure-sha> HEAD` passes.
- Root contains `AGENTS.md`, `.agents/`, `.omo/`, and `airbridge/`.
- `git ls-tree` proves both custom skills and the monorepo plan are present.
- Diff from target post-FAP baseline to merge HEAD under
  `applications`, `applications_user`, `targets`, `lib`, and `furi` is empty. The
  import contributes only the new product/project-memory surface.
- The complete FAP remains tracked.

### Mandatory execution-state cutover

After acceptance:

1. Stop writing to `/Users/asutov/projects/flipper-hid/.omo`.
2. All subsequent checkbox updates and notepads use
   `/Users/asutov/projects/flipperzero-firmware/.omo/...`.
3. All subsequent agents use the target monorepo as `projectPath`/workdir.
4. The old clone is read-only until Task 5 and must not run workers or local servers.
5. If `/start-work` cannot safely continue across the cwd change, create a handoff and
   resume in a fresh OpenCode session rooted at the monorepo before Task 4.

## Task 4 — Paths, Docs, Tools, Skills, and Ignores

Dispatch two disjoint agents in parallel after reading the target notepads.

### Task 4a — Tools and bundle (`quick` + `flipper-zero-dev`)

- Fix `airbridge/tools/gen_identity.py` so the monorepo root is derived from
  `Path(__file__).resolve().parents[2]`; retain the `airbridge/` product root as
  `parents[1]`.
- Sweep `airbridge/tools/` and `airbridge/scripts/` for hardcoded old absolute paths
  and obsolete two-repo assumptions.
- Remove tracked Python bytecode/caches.
- Delete `airbridge/firmware/` entirely: its patch files and duplicated FAP are
  superseded by canonical in-tree firmware/app sources.
- Add root `.gitignore` entries for `.omo/boulder.json`, `.omo/run-continuation/`,
  `.omo/start-work/`, `.playwright-mcp/`, and Python caches without disturbing existing
  firmware build ignores.
- Run Python syntax/LSP checks for edited scripts.

### Task 4b — Docs and project skills (`writing` + `flipper-zero-dev`)

Update all live documentation and skills, including:

- root `AGENTS.md`
- `airbridge/README.md`
- `airbridge/docs/firmware-guide.md`
- `airbridge/docs/demo-script.md`
- `airbridge/scripts/ble_qa_runbook.md`
- `.agents/skills/pocket-airbridge-hardware-qa/SKILL.md`
- `.agents/skills/flipper-zero-dev/SKILL.md`
- `.agents/skills/flipper-zero-dev/references/hid-composite-deploy.md`
- `.agents/skills/flipper-zero-dev/references/firmware-build-and-flash.md`

Required semantics:

- Describe this as one custom firmware/apps repo, not a separate product repo plus
  upstream-clean firmware fork.
- FAP canonical path remains `applications_user/pocket_airbridge/`.
- Canonical web command is:

  ```bash
  python3 -m http.server 8081 --bind 127.0.0.1 \
    --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
  ```

- Canonical harness URL is
  `http://127.0.0.1:8081/protocol-harness.html`; chat URLs have no extra `/web/`
  component when served this way.
- Remove all patch-application and duplicated-bundle instructions.
- Preserve every hardware safety, consent, persistent-browser, and artifact-path rule.

### Task 4 acceptance

- Read every changed file.
- Zero stale live references matching old absolute source paths,
  `cd web`, `--directory web`, `/web/chat-`, old `web/bootstrap` relative assumptions,
  or patch-bundle application instructions. Historical `.omo/evidence` and old plans
  are records and excluded from the live-doc sweep.
- Root `.agents/skills/` contains both project skills and their referenced files.
- `airbridge/firmware/` is absent.
- LSP diagnostics on edited source files are clean.
- Read-only Git diff includes only intended Task 4 files.

## Task 5 — Commit Local State and Complete Cutover

### User Git gate 4 — content commit

```bash
cd /Users/asutov/projects/flipperzero-firmware && \
test "$(pwd -P)" = "/Users/asutov/projects/flipperzero-firmware" && \
test "$(git branch --show-current)" = "pocket-airbridge" && \
git add -A && \
git commit -m "refactor: complete Pocket AirBridge custom firmware monorepo"
```

### User Git gate 5 — remove temporary remote

After the orchestrator verifies both histories remain reachable, the user runs:

```bash
cd /Users/asutov/projects/flipperzero-firmware && \
test "$(pwd -P)" = "/Users/asutov/projects/flipperzero-firmware" && \
git remote remove airbridge
```

### Local-only cutover

- Do not push.
- Initialize Codegraph for the target repo if desired; do not reuse the source
  `.codegraph` symlink.
- Open the monorepo as the sole active IDE/OpenCode project.
- Stop old servers and sessions rooted at `flipper-hid`.
- After all final verification, rename the old clone to an unmistakable archival name
  (for example `flipper-hid.ARCHIVED`) or keep it read-only. Deletion is outside scope.
- Optional safety: because the configured GitHub origin is public, the user may make
  it private manually. No command in this plan pushes or changes GitHub state.

### Acceptance

- Target tracked state is clean before Final Wave; ignored build/runtime outputs are
  separately inventoried.
- Temporary `airbridge` remote is absent.
- Root skills resolve from the target project.
- No process or active worker writes to the old source clone.

## Final Verification Wave

F1 must complete before F2. F3 may run alongside F1. F4 waits for F1–F3.

- [ ] F1. **Fresh-state firmware/FAP build — APPROVE/REJECT:** with
  `flipper-zero-dev`, verify `git ls-tree` contains `application.fam`, `icon.png`, and
  `pocket_airbridge.c`; build
  `./fbt build APPSRC=applications_user/pocket_airbridge`; record exact generated FAP
  path, byte size, and SHA-256 in `.omo/evidence/monorepo-merge/`. Compare tracked and
  ignored state before/after so generated outputs are not mistaken for source changes.
- [ ] F2. **Web + physical regression using F1 artifact — APPROVE/REJECT:** with
  `pocket-airbridge-hardware-qa` and `playwright`, serve the exact canonical
  `airbridge/web` path and obtain protocol harness **17/17**. Deploy the exact F1 FAP
  artifact after matching its recorded size/SHA-256; run bidirectional text and one
  SHA-256-verified attachment flow. Every hardware/browser gate uses the alert and
  `question` tool. If hardware is unavailable, record an explicit user-approved defer;
  browser harness still must pass.
- [ ] F3. **Live-reference and skills truthfulness — APPROVE/REJECT:** audit root
  `AGENTS.md`, `airbridge/`, and `.agents/`; verify zero stale old-repo/two-repo/patch
  assumptions, canonical serve command and URLs, root `.agents/skills/` completeness,
  runtime `.omo` ignores, no tracked caches, and absence of `airbridge/firmware/`.
- [ ] F4. **Topology, provenance, and local-only gate — APPROVE/REJECT:** read-only
  verification that both pre-migration HEADs are ancestors/referenced as recorded, the
  complete FAP and both skills are tracked, branch is `pocket-airbridge`, temporary
  remote is gone, no push occurred, tracked target state is clean, and all active work
  has cut over to the monorepo.

## Rollback

- Before Task 3, both repos are independent and recoverable by user Git operations.
- Task 3 creates one merge commit. The user may manually abort during conflict or
  manually revert/reset later; agents never perform rollback Git operations.
- No history is deleted, no force-push occurs, and no remote receives the private
  source history.
- The old clone remains available until Final Wave approval.
