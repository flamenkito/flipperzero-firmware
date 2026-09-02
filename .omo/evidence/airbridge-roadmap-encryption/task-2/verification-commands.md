## Task 2 verification commands

- `python3 tools/build_bundle.py` — PASS
  - `dist/app-usb.html: 94365 bytes`
  - `dist/app-ble.html: 96220 bytes`
  - `web/bootstrap.js: 1200 chars`
- `./fbt build APPSRC=applications_user/pocket_airbridge` from `/Users/asutov/projects/flipperzero-firmware` — PASS
  - built `build/f7-firmware-D/.extapps/pocket_airbridge.fap`
  - `API version 87.4 is up to date`
- Playwright persistent CDP `http://localhost:9222`, URL `http://localhost:8000/web/protocol-harness.html?task2=final-4` — PASS
  - result artifact: `harness-final-result.json`
  - status: `PASS: 20/20 protocol tests passed.`
- `python3 tools/flipper_alive.py` — BLOCKED hardware deploy
  - `ABSENT: no usbmodem port`
- LSP diagnostics
  - JS/HTML Biome LSP unavailable: server not installed / user previously declined installation.
  - Bundled C file diagnostics cannot resolve Flipper SDK headers under this repo; firmware build is the authoritative C gate and passed.
  - Live firmware C path is outside current LSP cwd, so direct LSP diagnostics were unavailable.
