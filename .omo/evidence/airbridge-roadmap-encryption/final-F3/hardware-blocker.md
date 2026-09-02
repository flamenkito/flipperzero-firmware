# F3 Hardware End-to-End QA Blocker

- Date: 2026-08-29
- Working directory: `/Users/asutov/projects/flipper-hid`
- Required preflight command: `python3 tools/flipper_alive.py`
- Observed output: `ABSENT: no usbmodem port`
- Verdict type: hardware absence blocker, not a product failure.

Because no Flipper USB modem port is present, the final physical QA matrix was not run. No WebHID, Web Bluetooth, SAS unlock, encrypted bidirectional text, encrypted attachment SHA-after-decrypt, cancel recovery, compressed USB Deploy from `https://blank.org`, Bridge BLE picker watchdog, or Flipper `DROP`/`TXERR` counter results are claimed.

Minimum rerun steps:
1. Connect and unlock the Flipper Zero at desktop.
2. Rerun `python3 tools/flipper_alive.py` from `/Users/asutov/projects/flipper-hid` and require `ALIVE` before deployment or browser gates.
3. Use persistent Chrome CDP at `http://localhost:9222` only.
4. Deploy only `/ext/apps/USB/pocket_airbridge.fap`, verify exactly one AirBridge FAP copy, then run the full gated hardware QA with audible alert + `question` before every physical picker/button/screen action.
