# Pocket AirBridge

Offline, end-to-end encrypted browser chat and attachment exchange using a **Flipper Zero** as a physical bridge between two computers.

Pocket AirBridge lives in one local custom firmware and apps repository at
`/Users/asutov/projects/flipperzero-firmware`. Its product assets are under
`airbridge/`, and its canonical FAP source is
`applications_user/pocket_airbridge/`. Edit and build those in-tree sources.
There is no separate product repository, patch bundle, or patch-application step.

**Hackathon goal**: Exchange text messages and send small files or images between PC-A and PC-B with no network, no cloud, and nothing to install on either PC.

For native SSH, REST, and WebSocket clients, the optional Rust tool
[**abt**](native/README.md) forwards TCP over the same USB HID ↔ BLE bridge.
Use the [Mac/Windows/WSL daily-start guide](native/README.md#daily-start-mac--windowswsl)
for terminal commands, then [set up passwordless SSH](native/README.md#passwordless-ssh-over-the-wsl-tunnel)
or [copy files and upgrade](native/README.md#copy-files-and-upgrade-the-windows-endpoint).
macOS hardware tests and [Mac-to-WSL SSH](native/README.md#mac-client--windows-usb--wsl-server)
through Windows USB have passed, including file integrity, duplex benchmarks,
and concurrent HTTP/WebSocket forwarding. The 0.2.0 protocol changes measured
43–50% faster one-way transfers on that pair; see the [results](native/README.md#protocol-optimization-result--2026-09-16).
SSH or TLS supplies native tunnel encryption;
the browser's encrypted chat protocol remains separate.

## Quickstart

1. **Flash the Pocket AirBridge firmware** (see [docs/firmware-guide.md](docs/firmware-guide.md)).
2. **Serve the web pages locally** (WebHID/Web Bluetooth need a secure origin):
   ```bash
   python3 -m http.server 8081 --bind 127.0.0.1 --directory /Users/asutov/projects/flipperzero-firmware/airbridge/web
   ```
3. **Connect Flipper Zero to PC-A** via USB.
4. **Pair Flipper Zero to PC-B** via Bluetooth (confirm the PIN shown on the Flipper screen).
5. **Open `http://127.0.0.1:8081/chat-usb.html`** on PC-A (Chrome/Edge), type `:c` then Enter, and select the device running Pocket AirBridge in the browser prompt.
6. **Open `http://127.0.0.1:8081/chat-ble.html`** on PC-B (Chrome/Edge), type `:c` then Enter, and select the active Bluetooth impersonation profile.
7. Compare the six-digit code printed as `verify peer: NNNNNN` on both pages (also shown as SAS in the statusline). Run `:a` on each side only if the codes match. Press `i` to write a message and Enter to send it. Use `:f` to choose a file and `:s` to send it.

For a browser-only protocol check, open the canonical harness URL:
`http://127.0.0.1:8081/protocol-harness.html`.

For deploy timeouts, pairing problems, or persistent `TXERR`, see
[troubleshooting and the BLE Deploy restoration findings](docs/troubleshooting.md).

## Terminal controls

The interface follows a Vim/Neovim terminal layout: a numbered text buffer,
end-of-buffer `~` markers, a block cursor in NORMAL, a reverse-video statusline,
and an Ex command line on the last screen row. Messages and attachments are
plain text records. Help and logs open as horizontal text splits.

Both pages start in **NORMAL** mode. Press `i` (or click the mode label) to enter
**INSERT**, where Enter sends text literally, including leading `/` and `:`.
Esc returns to NORMAL and keeps the message draft and selection. From NORMAL,
`:` opens a separate **COMMAND** input. Enter runs a command; Esc discards that
command and returns to NORMAL. Backspace on an empty command also returns to
NORMAL. Click the mode label to start writing, or the buffer name to open commands.

In NORMAL, `j`/`k` move between transcript records, `gg` selects the first,
`G` selects the last, and Ctrl-d/Ctrl-u scroll half a screen. In help and logs,
these keys scroll the focused split. Keyboard shortcuts apply only within the
focused terminal. The statusline shows the selected record and scroll position.
This is a chat interface; Vim text operators and file editing are not implemented.

| Short command | Full command | Action |
|---|---|---|
| `:c` | `:connect` | Connect the current endpoint |
| `:d` | `:disconnect` | Disconnect; received downloads expire |
| `:f` | `:file` | Choose an attachment without sending it |
| `:s` | `:sendfile` | Send the selected attachment |
| `:uf` | `:unfile` | Remove the selected attachment |
| `:x` | `:cancel` | Cancel an active transfer |
| `:a` | `:accept` | Confirm the matching six-digit code |
| `:ab` | `:abort` | End code verification |
| `:l` | `:logs` | Toggle diagnostics |
| `:cl` | `:clear` | Clear transcript and its downloads |
| `:cll` | `:clearlog` | Clear diagnostics |
| `:h` | `:help` | Show commands and keyboard help |
| `:q` | `:quit` | Close help/log splits and keep the connection |

Tab completes the highlighted command; arrows select suggestions or recall
command history when suggestions are closed. Commands require exact names or
the listed aliases. Feedback and failures appear on the bottom command line.
Switching modes does not change the connection or cancel a transfer.

Selecting a file preserves the message draft. Enter in INSERT sends only text;
`:s` sends the attachment. Save received downloads before disconnecting or
clearing the transcript: they exist only in this page's memory.

Each page has one shared ASCII progress bar for sending or receiving; attachment
records show only their transfer state. Incoming transfers also show `RX 0–100%`
in the statusline, with byte totals and rate in the transfer display. Progress counts unique received
encrypted DATA bytes across all segments; retransmissions do not increase it.
`RX verify` indicates final validation. The download appears after verification,
with the exact file size.

For browser regression QA, start the canonical server and the persistent Chrome
instance described in `AGENTS.md`, then run `node airbridge/tools/qa_terminal.cjs`
with Playwright available to Node (install it in your tooling environment or set
`NODE_PATH` to its existing `node_modules`). The runner attaches over CDP, checks
both endpoints and the protocol harness, and writes screenshots/results under
`/private/tmp/airbridge-vim-qa-*`. It leaves the browser tabs open for inspection.

## Architecture

| Component | Technology | Role |
|-----------|-----------|------|
| PC-A Chat | WebHID (Chromium) | Sends and receives messages and attachments over USB HID |
| Flipper Zero | Custom USB HID + BLE Serial | Blind relay: forwards encrypted frames verbatim between USB and BLE |
| PC-B Chat | Web Bluetooth (Chromium) | Sends and receives messages and attachments over BLE, decrypts, reassembles, verifies hash |

Read the full architecture in [`docs/architecture.md`](docs/architecture.md).

## Protocol

Protocol **v2 (AB2S)** uses 64-byte frames, 59-byte outer payloads and 51-byte encrypted DATA slices. Chat requests the **AB2W four-frame DATA window** through an explicit HELLO capability exchange. Each DATA frame still receives a contextual ACK and owns its exact retry bytes; all frames in a batch must be acknowledged before the next batch. Ordinary item ACKs are 15 bytes, window HELLO ACKs and NACKs 16 bytes, and KEY handshake ACKs three bytes. Receiver authentication, final size/SHA-256 verification and Blob creation precede `ITEM_DONE` ACK. Refresh both endpoints together; older receivers reject the new window request before payload transmission. See the [wire contract](docs/protocol.md#bounded-data-windows).

The chat protocol is always encrypted end to end in the browsers. USB and BLE exchange a P-256 ECDH handshake, derive AES-GCM keys with HKDF, and unlock only after both users compare and accept the same six-digit SAS. The Flipper never sees plaintext, session keys, decrypted metadata, or decrypted files.

Read the full specification in [`docs/protocol.md`](docs/protocol.md).

Attachments use two independent File.stream() passes: incremental hash/count,
then segmented encryption. Receive authenticates each segment before retaining
plaintext chunks, verifies the final counts/SHA-256, then offers a Blob download.
Retained authenticated plaintext is intentionally **O(file size)** browser memory;
completed cards own object URLs and revoke them on removal/replacement/disposal.
There is no whole-item sender/ciphertext buffering or disk/OPFS/File System Access
receive, save picker or direct-to-disk mode.

V2 removes the old **4 MiB/4096-chunk demo ceiling**, not finite limits: uint32
IDs/segment counts, uint64 sizes, browser heap/API limits, download/storage
capacity, BLE/USB throughput and transfer time still constrain attachments.
Old/plain or unsupported-streaming peers fail with **`Unsupported protocol version`**
before META/DATA progression. Use current v2 pages on both endpoints, reconnect
and confirm a fresh SAS session; no mixed v1/v2 fallback exists. The unchanged
handshake remains `cryptoVersion:1`. See root
[ADR 0001](../docs/adr/0001-airbridge-protocol-v2-streaming.md).

## Locked-down PC bootstrap (Deploy app)

Some corporate PCs are locked down by device-control policy: no mass storage, no network transfer, no local files. HID is the only USB class that survives. The Deploy flow turns that policy to our advantage. For a locked-down USB host, the Flipper impersonates an HP "Wireless Keyboard and Mouse" dongle (VID `0x03F0`, PID `0x5341`), a composite device with a real keyboard collection and a vendor-defined collection on usage page `0xFF00`. It types a small bootstrap into the browser as if it were a keyboard, the bootstrap opens WebHID on the vendor collection, and the Flipper streams the complete chat app to the PC over the existing vendor HID channel. For a Bluetooth-paired target, the BLE Deploy prompt instead types [`bootstrap-ble.js`](web/bootstrap-ble.js) over BLE HIDS keyboard reports, and the Flipper streams the matching chat app over the AirBridge BLE serial notify characteristic. The two prompts share identical guardrails; only the transport and asset pair differ.

### Honest framing

This is BadUSB-shaped by design. Keyboard emulation is the whole point: it is the only delivery channel a HID-only policy cannot block. BLE typing is equally explicit: physical possession of the Flipper plus on-device OK plus a paired target. The guardrails are deliberate, and identical for both Deploy paths:

- Keystrokes are emitted only from an explicit deploy prompt on the Flipper (LEFT/RIGHT cycle Bridge → USB Deploy → BLE Deploy → Bridge from the Bridge screen), and only after you place the cursor and press OK to confirm.
- The Flipper screen shows `TYPING…` for the entire emission; pressing BACK aborts instantly.
- No keyboard report is ever sent in Bridge mode or on any data path. Typing exists only inside the Deploy prompts.
- The typed payloads are fixed, reviewable, ASCII-only artifacts: [`bootstrap.js`](web/bootstrap.js) for USB Deploy, [`bootstrap-ble.js`](web/bootstrap-ble.js) for BLE Deploy. The FAP verifies each build-time-pinned SHA-256 before typing. Each fetches a gzip-compressed app bundle whose versioned SD container is also SHA-256 pinned (`app-usb.html.gz` for USB, `app-ble.html.gz` for BLE); unsupported browsers show `Transfer unsupported - retry` before transport selection.
- The whole thing requires physical possession of the Flipper plus explicit on-device actions. Task 8 adds small bounded typing jitter, but typing still exists only inside the explicit Deploy flow.

### Steps

Run these commands from the monorepo root,
`/Users/asutov/projects/flipperzero-firmware`.

1. **Build the app bundle:**
   ```bash
   python3 airbridge/tools/build_bundle.py
   ```
    This inlines the shared JS modules into the self-contained USB and BLE deploy pages, writes the authenticated `airbridge/dist/app-usb.html.gz` and `airbridge/dist/app-ble.html.gz` containers, and regenerates `applications_user/pocket_airbridge/airbridge_assets_digest.h`. Both bundles are generated deploy outputs; v2 source verification does not prove generated bundle freshness. The no-limit-transfer Todo 9 integration gate owns the rebuild after runtime changes.
2. **Deploy the bootstraps and the bundles to the Flipper SD card** (exact commands in [docs/firmware-guide.md](docs/firmware-guide.md)).
3. **Launch Pocket AirBridge** on the Flipper. The app opens on the Bridge relay screen; press **RIGHT** to reach the USB Deploy prompt, **RIGHT** again for the BLE Deploy prompt. (LEFT/RIGHT cycle Bridge → USB Deploy → BLE Deploy → Bridge; a short BACK returns to Bridge, a long BACK exits the app. The relay keeps running in the background on every screen.) Each prompt asks you to place the cursor, then press OK.
4. **On the target PC**, open a browser tab at `https://blank.org`, open DevTools (F12), and click into the console. Any `https://` page works; `about:blank` is possible but verify first — on some Chrome builds `window.isSecureContext === false` there, which blocks WebHID. Run `console.log(window.isSecureContext)` to confirm before proceeding. (For BLE Deploy, pair the target to the Flipper BLE identity first, then place the cursor in the console of the paired machine.)
5. **Press OK on the Flipper.** The matching bootstrap types itself into the console while the screen shows `TYPING…` (BACK aborts). Once executed, it paints a minimal landing page with a Connect button.
6. **Click Connect.** Your real click supplies the user activation the browser needs; pick the device in the browser prompt (WebHID for USB Deploy, Web Bluetooth for BLE Deploy). The Flipper streams the compressed app from its SD card — over the vendor HID channel for USB, over the BLE serial notify characteristic for BLE — the bootstrap inflates it with `DecompressionStream("gzip")`, and the page replaces itself with the full app. Unsupported browsers show `Transfer unsupported - retry` before a picker opens.

`data:` URLs are dead for this purpose (`window.isSecureContext === false`, so WebHID is unavailable there). `about:blank` is NOT reliably a secure context — on some Chrome builds `window.isSecureContext === false` and `navigator.hid` is undefined, while the same Chrome build passes on `https://blank.org`. The robust validated channel is ANY `https://` page plus the DevTools console. Both facts were confirmed on real Chrome on 2026-07-20 (about:blank insecure on the user's build; https://blank.org worked end-to-end on hardware).

The typed snippet carries a WebHID filter list that enumerates every profile VID/PID (Logitech, Dell, MSFT, HP). On the target machine only the Flipper should match; other devices with those IDs should be absent.

The bootstrap is unaffected by the firmware USB identity setting. It uses the FAP's configured kbd+vendor profile during Deploy.

### Keyboard layout requirement

The typed payloads are ASCII-only but include symbols like `{}[]();:=>"'`. The Flipper types them using US keyboard scancodes (USB HID for USB Deploy, BLE HIDS for BLE Deploy), so **the target PC must use a US keyboard layout**. On any other layout the symbols mistype.

### Verified on hardware

- HP composite enumeration verified on macOS, 2026-07-20: VID `0x03F0` PID `0x5341`, exact HP product/manufacturer strings, keyboard collection claimed by the OS while WebHID opens the vendor `0xFF00` collection, bidirectional chat through the HP composite.
- The Windows `usbccgp` tree-compare against the real dongle capture is **deferred**. Run the same `Get-PnpDevice` capture on the target machine when one is available.
- One USB profile is active at a time, selected by `/ext/apps_data/pocket_airbridge/config` (default `hp_kbd_vendor`). The configured profile is applied automatically a short moment after app start and is held until the app exits (always-on). There is no runtime switching: composite-to-composite reconfiguration is fatal on this USB stack (the device dies silently and needs a physical reset).
- Task 8 added a per-device default BLE DIS serial derived from a hash of the Flipper UID and formatted as `HP` plus eight hex digits. Raw UID bytes are not exposed. BLE service UUID hiding and Windows `usbccgp` tree comparison remain deferred until physical Chrome and Windows evidence exists.

## Verified on hardware (2026-07-20)

Tested on a real Flipper Zero between `chat-usb.html` and `chat-ble.html`:

- Bidirectional text messages.
- Attachments in both directions with SHA-256 verified: 38 B, plus 5 KB and 20 KB transfers.
- Sender-side cancel and receiver-side cancel, both mid-transfer.
- Flipper debug aids working: green LED heartbeat every 500 ms; on-screen counters `U->B` / `B->U` / `DROP` / `TXERR`, with `DROP 0` and `TXERR 0` during healthy transfers.
- Rough throughput: ~0.5–2 KB/s over BLE (demo-acceptable, not fast).

## Current verification scope

The 2026-09-15 restoration (`90a19db1`, API 87.15) passed firmware/FAP builds,
host tests, bundle checks, and the 275/275 browser harness. After the firmware
update, the owner reported “all works perfectly.” See the
[verification record](docs/troubleshooting.md#ble-deploy-restoration-2026-09-15)
for the captured checks and the scope of that hardware report.

Earlier roadmap work added browser-only E2E crypto, SAS unlock, deterministic gzip Deploy bundles, exact-frame retry, BLE static tuning evidence, deploy typing jitter, and per-device DIS serials. Current protocol v2 streaming is covered by Node and browser/mock tests; the historical hardware results above are not v2 proof. Generated bundle integration and physical USB/BLE verification are separate gates. BLE service UUID hiding, Windows USB tree comparison and negotiated radio/throughput claims still need physical evidence. Resume is not implemented.

## Project Structure

```
flipperzero-firmware/
├── airbridge/
│   ├── docs/               # Architecture, protocol, and operations docs
│   ├── web/
│   │   ├── chat-usb.html       # WebHID chat page (send/receive over USB)
│   │   ├── chat-ble.html       # Web Bluetooth chat page (send/receive over BLE)
│   │   ├── airbridge-protocol.js
│   │   ├── airbridge-transports.js
│   │   └── protocol-harness.html
│   └── README.md               # This file
└── applications_user/pocket_airbridge/ # Canonical FAP source
```

The superseded `sender.html` and `receiver.html` file-transfer pages were removed;
use `chat-usb.html` and `chat-ble.html` instead.

## Task Breakdown for Implementation

### Phase 1 — Validate the Web Pages (No Flipper)
- [x] Host `chat-usb.html` and `chat-ble.html` on `localhost`.
- [x] Verify WebHID permission flow and device selection.
- [x] Verify Web Bluetooth pairing flow and GATT characteristic access.
- [x] Use two browser tabs + mocked Flipper (`?mock=1` pages and `protocol-harness.html`) to test message and attachment exchange.

### Phase 2 — Flipper Zero USB HID Vendor Interface
- [x] Define a custom HID report descriptor with Usage Page `0xFF00`.
- [x] Implement USB IN/OUT report handlers in the Flipper app.
- [x] Test that the sender page can open the device and exchange 64-byte reports.

### Phase 3 — Flipper Zero BLE GATT Service
- [x] Build custom AirBridge BLE serial service with its own UUID family (service `7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify `87825ec0-7398-8cb7-3242-b083eaa34f27`, RX write `152f7eeb-e3b7-5898-ba41-7ff66121c98d`).
- [x] TX characteristic (notify) and RX characteristic (write) working via a raw-serial hook in `bt_service`.
- [x] Test that the receiver page can pair, connect, and subscribe to notifications.

### Phase 4 — Bridge Logic
- [x] Implement the forwarding loop (stateless byte relay driven by an event queue; no single-item buffer on the Flipper).
- [x] Forward frames and ACKs transparently in both directions.
- [x] Timeout, retry, and cancel logic (lives in the browser endpoints, not the bridge).
- [x] Add status screen rendering (USB/BLE state, `U->B` / `B->U` / `DROP` / `TXERR` counters, LED heartbeat).

### Phase 5 — End-to-End Demo
- [x] Exchange text messages between PC-A and PC-B (both directions, hardware-verified 2026-07-20).
- [x] Send a small text file as an attachment (38 B, 5 KB, and 20 KB, both directions).
- [x] Send a small PNG/JPG image as an attachment (binary attachments up to 20 KB verified on hardware 2026-07-20).
- [x] Verify SHA-256 hash matches on the receiver (both directions).
- [x] Verify sender-side and receiver-side cancel mid-transfer.
- [x] Measure rough throughput and chunk latency (~0.5–2 KB/s over BLE).

### Known Gaps / Hackathon Hacks

| Gap | Mitigation for Demo |
|-----|---------------------|
| Custom BLE profile built | AirBridge composite profile with Battery, DIS, HIDS, and the custom serial UUID family (`7b871228-baf0-c5b4-5f46-9c2613d627a3` / TX `87825ec0-7398-8cb7-3242-b083eaa34f27` / RX `152f7eeb-e3b7-5898-ba41-7ff66121c98d`); TX uses NOTIFY (not INDICATE). Static firmware config supports a local ATT MTU maximum of 414, enables DLE, prefers 2M PHY, and requests a 7.5 to 45 ms interval; negotiated runtime values remain peer-driven and need hardware evidence. |
| Custom HID descriptor registration | May need to patch `furi_hal_usb_hid` or use a community plugin template |
| BLE service UUID hiding | Deferred. The serial UUID is still advertised and the browser still uses the service-filtered picker until `acceptAllDevices:true` with `optionalServices` is proven on hardware. |
| 51-byte encrypted DATA slices | Small reports limit throughput; practical transfer time grows with file size. |
| Single-item half-duplex | Only one item in flight; order by `(itemId, role)`, USB wins equal-ID ties. NACK retries exact current-item frames only; no resume. |
| Transfer resume | Deferred. NACK retries exact current-item outer frames only. |

## Browser Compatibility

- **Chrome / Edge / Brave** (Chromium 89+) on Windows, macOS, or Linux.
- WebHID and Web Bluetooth require a **secure origin** (`https://` or `localhost`).
- For persisted `getDevices()` grants across Chrome restarts, enable
  `chrome://flags/#enable-web-bluetooth-new-permissions-backend`; without it,
  Bluetooth reconnects fall back to the browser picker.
- On Linux, Web Bluetooth may need `chrome://flags/#enable-web-bluetooth` or kernel BLE permissions.

## Flipper Zero Firmware Notes

The firmware's new **USB Identity** (boot identity) and **Flipper USB** (volatile
CDC for maintenance) settings entries — plus compatibility edge cases and Pocket
AirBridge ownership rules — are documented in
[USB identity and maintenance](docs/usb-identity.md). Hardware enumeration claims
for this feature remain pending hardware verification.

The FAP owns both custom profiles in `applications_user/pocket_airbridge/`:

- **USB HID**: `usb_airbridge` — a vendor-defined HID profile using usage page `0xFF00` for bidirectional 64-byte reports. USB identity is selected at app start from `/ext/apps_data/pocket_airbridge/config` (default `hp_kbd_vendor`, HP VID `0x03F0` PID `0x5341`) and held for the session lifetime. Composite-to-composite reconfiguration is not attempted.
- **BLE GATT**: `airbridge_profile.c` advertises the AirBridge serial UUID family: service `7b871228-baf0-c5b4-5f46-9c2613d627a3`, TX notify `87825ec0-7398-8cb7-3242-b083eaa34f27` (NOTIFY, not INDICATE), and RX write `152f7eeb-e3b7-5898-ba41-7ff66121c98d`. Battery, DIS, and HIDS are also included in the composite; HIDS exists for explicit BLE Deploy typing and carries no Bridge-mode keyboard traffic.

Static BLE tuning evidence records configured ATT MTU 414, DLE enabled, 2M PHY preference, requested 7.5 to 45 ms connection interval, and 244-byte serial value capacity. Runtime negotiated MTU/PHY/DLE/interval and throughput remain unclaimed unless a hardware evidence run records them.

The custom FAP lives in `applications_user/pocket_airbridge/` and is built with this repository's full firmware build system.

Rebuild firmware and the FAP together at API 87.15. Exit waits on a `Closing...`
screen until both transports are restored. An independent supervisor reports
stalled operations; it does not forcibly unload the app. See
[firmware ownership and fault limits](docs/firmware-boundary.md).

The original `flipper/bridge_app.c` pseudocode skeleton now lives at [`docs/attic/bridge_app.c`](docs/attic/bridge_app.c) and is superseded. The live FAP is `applications_user/pocket_airbridge/pocket_airbridge.c`.

## License

MIT — Hackathon prototype. Use at your own risk.
