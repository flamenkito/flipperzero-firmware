Post-write review

- Single responsibility: Task 2 touched the deploy bootstrap, deploy harness tests, and FAP deploy gate only. The touched harness/FAP files are inherited oversized modules; broad splitting is out of scope for this safety fix and would be riskier than the scoped change.
- Boundary purity: No new untrusted structured input path was added; bootstrap still parses the fixed deploy stream header at the HID boundary.
- Variant discrimination: FAP wrong-state deploy handling distinguishes screen state only for the `0x42` deploy sentinel. The harness model uses a `switch` over known screen names.
- Escape hatches: No new `any`, ignore comments, unchecked casts, or keyboard-report bypasses were added.
- Defensive layer: Timeout is a required boundary failure path for an absent/wrong-state FAP stream, not redundant verification.
- Helpers for one-off: No production helper was added; harness model helpers support deterministic regression tests.
- Tests: Final protocol harness is `PASS: 20/20`; reverting timeout or FAP state classification breaks the new tests.
- Parameter bloat: Existing harness helper was shaped as an options object with two fields to keep the call arity at three.
- Redundant verification: None added.
- Negative naming: No new negative-form public names.
- Logging: No new production browser logging. FAP shows a user-visible error for non-waiting deploy requests; no extra bridge-mode log path was added.
- LOC: `web/bootstrap.js` is 1 pure LOC; `docs/protocol.md` is 168. `web/protocol-harness.html` and both FAP copies remain inherited >250 LOC monoliths; the refactor skill was loaded, but splitting them is deferred because Task 2 requires a minimal safety change with hardware-sensitive behavior.
