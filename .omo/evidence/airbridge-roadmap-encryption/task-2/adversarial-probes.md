## Task 2 adversarial probes

- stale_state: harness fetches `bootstrap.js` with cache-busting query params for eval/timeout tests; final Playwright URL also used a cache-busting query.
- dirty_worktree: main repo had Task 1-era dirty files before Task 2; firmware repo had only the live FAP change after this task.
- generated/cached artifacts: `python3 tools/build_bundle.py` regenerated `dist/app-usb.html` and `dist/app-ble.html`; no `__pycache__` artifact was created in the final observed status.
- hung commands: bootstrap stream client now resolves timeout as `false`, removes the `inputreport` listener, records `errorState: "timeout"`, and sets visible retry status.
- flaky tests: first Playwright run exposed stale cached bootstrap source; harness now cache-busts bootstrap fetches and final run passed 20/20.
- misleading success output: hardware Deploy is explicitly blocked and not claimed; mock timing remains marked mock/hardware false.
- hardware evidence conflation: `hardware-blocker.json` separates absent hardware from mock/browser proof.
- mid-operation interrupt/cancel: existing cancel harness still passes; timeout cleanup closes/removes HID state and re-enables the button.
- Bridge-mode deploy guard: FAP keeps `0x42` as relay data on `AirbridgeScreenBridge`; non-Bridge/non-Waiting deploy requests show `DEPLOY NOT ARMED` instead of silently waiting.
