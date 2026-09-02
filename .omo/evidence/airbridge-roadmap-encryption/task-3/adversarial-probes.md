## Adversarial probe notes

- stale_state: live FAP path was rediscovered and saved before edits; baseline diffs/status captured before patching.
- dirty_worktree: both repos were already dirty. Firmware baseline diff contained inherited Task 2 deploy-state handling; main repo contained inherited Task 1/2 web and bundle changes.
- generated/cached artifacts: no generated `dist/` or `__pycache__` artifacts were intentionally modified by Task 3 implementation; protocol-harness run may read existing generated files only.
- hung/long commands: firmware build must run with an extended timeout and output captured.
- flaky tests: protocol harness must be run through persistent CDP with cache-busting/normal UI run; pass count is the gate, not misleading page load success.
- misleading success output: build output and harness pass text are captured as artifacts; hardware success is not claimed unless observed.
- hardware evidence conflation: if `tools/flipper_alive.py` reports ABSENT/HUNG, write explicit hardware blocker JSON and do not claim BLE picker or `TXERR` evidence.
- cancel/resume/active-session interruption: static proof checks the Bridge watchdog contains no `bt_disconnect()` and exits when `ble_connected` is true; Waiting-screen deploy squatter kick remains scoped to `AirbridgeScreenWaiting` only.
