## 2026-07-21 — REGRESSION RULE (user directive, mandatory)
After EVERY firmware/FAP/web change, BEFORE any new-feature debugging, ALWAYS re-run the regression suite of previously-working flows:
1. Bridge chat text BOTH directions (USB->BLE and BLE->USB) on hardware.
2. File transfer both directions with SHA-256 match + DROP/TXERR = 0.
3. Deploy flows that previously worked (USB deploy).
4. Protocol harness 17/17.
A change is not "done" until previously-working flows are re-verified. Rationale: the airbridge profile swap went in without chat regression; deploy symptoms were chased before the baseline was established.
