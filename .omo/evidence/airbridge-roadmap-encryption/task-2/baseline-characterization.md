Task 2 baseline characterization (2026-08-29)

- Task 1 evidence format: `.omo/evidence/airbridge-roadmap-encryption/task-1/root-harness-result.json` stores a top-level `status`, `results[]`, and `evidence[]` records using `airbridge-timing/v1`; mock evidence is explicitly `mock:true`, `hardware:false`, `marker:"mock"`.
- Current bootstrap behavior: `web/bootstrap.js` shows `Connecting`, sends one 64-byte report with byte 0 = `0x42`, then waits for HID `inputreport` completion. Before Task 2 there was no timeout; a wrong-state or silent FAP could leave `Connecting` forever.
- Current FAP behavior: both bundled and live `pocket_airbridge.c` stream only when `app->screen == AirbridgeScreenWaiting && be->data[0] == 0x42`; otherwise traffic is relayed. There was no explicit wrong-state handling for stale/non-Waiting deploy requests.
- Live FAP path discovery is recorded in `live-fap-path.txt`.
- Dirty-worktree probe is recorded in `baseline-worktree-status.txt`; inherited Task 1 files were already modified/untracked before Task 2 edits.
- Red test proof is recorded in `red-harness-result-2.json`: the new `bootstrap stream timeout` test fails because timeout completion is absent.
