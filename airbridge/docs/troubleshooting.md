# Pocket AirBridge troubleshooting

## Native `abt` tunnel: 2026-09-16

See the [native usage and troubleshooting guide](../native/README.md) for build
artifacts, commands, and the Mac BLE → Windows USB → WSL SSH setup. Keep the
Flipper on Bridge, close competing browser/native clients, and run `abt.exe`
on the Windows host. Confirm Windows can log into WSL's SSH server through
localhost before starting the bridge connection.

The Windows x64 release builds and passes `--version` and `usb --help` under
Wine 11.0; physical Windows HID access and the complete WSL path remain untested.
The same-Mac native pair passed SSH, HTTP, WebSockets, and concurrent SSH
forwarding with zero retries and owner-confirmed DROP 0 / TXERR 0. ABT1 does not
encrypt traffic itself; use SSH/TLS.

During local QA, a sandboxed temporary `sshd` failed its own macOS sandbox
initialization (`ssh_sandbox_child: sandbox_init: Operation not permitted`).
The bridge reported a TCP write failure/reset because that server closed the
socket. Running the temporary server outside the development sandbox resolved
it; no system Remote Login change or transport fix was needed. Empty HID scans
or Bluetooth permission errors can also come from the development sandbox.

## Transfer throughput and HID report ownership: 2026-09-16

Hardware baseline with 32 KiB attachments measured **1.43 KiB/s USB→BLE** and
**1.49 KiB/s BLE→USB**. Every 51-byte DATA slice waited roughly 30 ms for its
end-to-end ACK. BLE write-without-response reduced the browser write-call time,
but throughput stayed at 1.42/1.55 KiB/s; this alone did not remove the round-trip
bottleneck. These measurements include the file hash pass and protocol setup.

After the USB transmit fix below, the bounded four-frame window measured:

| Same 32 KiB file | One frame | Four frames | Gain |
| --- | ---: | ---: | ---: |
| USB→BLE | 1.43 KiB/s | 2.54 KiB/s | 77% |
| BLE→USB | 1.49 KiB/s | 2.21 KiB/s | 49% |

A 70,000-byte file crossing the 64 KiB encryption-segment boundary completed at
**2.57 KiB/s USB→BLE** and **2.25 KiB/s BLE→USB**. Both file sizes passed byte
comparison and SHA-256 in both directions, with zero DATA retries or browser
write errors. The owner then read the Flipper counters as **5380 / 5380 / 0 / 0**
(`U→B / B→U / DROP / TXERR`). Two-frame results were 1.98/1.63 KiB/s; four frames
is the selected bound. These are measurements from this Mac/Flipper connection,
not guaranteed rates on other hosts or radios. They measure encrypted Bridge
file transfers; the separate Deploy stream was not made faster by this change.

The AB2W DATA window exposed a second issue in the FAP's USB transmitter. The
old nonblocking `airbridge_usb_vendor_send_response` released its semaphore
immediately after `usbd_ep_write`, before the USB transmit-complete callback.
Back-to-back reports could therefore replace a report the host had not consumed.
During the initial two-frame test the BLE sender attempted 199 DATA writes,
while USB received 169; repeated five-second retries made the reverse transfer
unusable. The test was cancelled rather than counted as a completed benchmark.

The helper now leaves the slot occupied until the interrupt-IN completion.
Bridge uses the existing completion-based sender with a **10 ms maximum wait**
for a slot, preserving the exit polling budget. USB Deploy already used this
sender. The new native regression submits two reports before completion: the
old helper wrongly accepts the second write, while the fixed helper rejects it
or waits for the first slot to complete. A successful enqueue is not permission
to reuse the endpoint before its completion event.

USB remains HID-only with the selected impersonation profile, 64-byte reports,
and the existing endpoint polling interval. The Flipper queue remains eight
one-frame events. The window/ACK protocol and retry/reorder buffers live in the
browsers; see the [window contract](protocol.md#bounded-data-windows). Refresh
both browser pages together, regenerate both deploy bundles and digest pins,
then rebuild/upload the FAP to install the USB send fix. A full firmware flash
is unnecessary for these app-owned changes; API remains **87.15**.

Benchmark each direction separately with the same file and verify the received
SHA-256. Record elapsed time, ACK latency, retries, and on-device DROP/TXERR;
do not infer throughput from the duration of a GATT write promise. Local captures
are under `.omo/evidence/hid-throughput/` (gitignored).

Validation for this change:

- Native/static host checks and **237 JavaScript tests** pass; bundle builder
  **14/14**, persistent-browser harness **275/275**, FAP SDK/APPCHK pass.
- Real text works both ways. Sender and receiver cancel clear active items and
  buffers without exposing a partial download; fresh 1 KiB files then pass
  SHA-256 both ways. Receiver cancel currently displays `Failed: Peer stream
  cancel` at the sender; it still settles and permits another transfer.
- With automatic BLE reconnect disabled for the fault test, a GATT disconnect
  during DATA produces a visible sender failure after **25.1 seconds** (the
  existing five attempts at five seconds), with no stuck send or retained item.
  DROP/TXERR readings above were taken before this intentional disconnect.
- USB Deploy: owner confirmed keyboard deployment; captured bootstrap logs show
  all **67,544 gzip bytes**, checksum success and page boot. Stream time from the
  trigger was **11.489 seconds**. The loaded scripts exactly match the generated
  USB bundle. The SD container is 67,556 bytes including its 12-byte header.
- BLE Deploy: owner confirmed keyboard deployment; captured logs show TX
  subscription, the `0x42` trigger, all **67,591 gzip bytes**, checksum success
  and page boot. The loaded scripts exactly match the generated BLE bundle.
  The SD container is 67,603 bytes including its 12-byte header. Both Deploy
  regressions are complete.

## BLE Deploy restoration: 2026-09-15

Fix commit: `90a19db1` (`fix(airbridge): restore BLE deploy, correct HID event
routing, and prevent bootstrap timeouts`). After the firmware update, the owner
reported “all works perfectly.” Automated results captured for this revision:

- Firmware and FAP builds, SDK/APPCHK: pass, API **87.15**.
- Host runner: 16 C test modules, static checks, and 213 JavaScript tests pass.
- Bundle builder: 14 tests pass.
- Persistent-browser protocol harness: **275/275** pass, including mock text
  and byte-verified attachments in both directions, up to 4,194,369 bytes.
- BLE dispatcher regression passes with AddressSanitizer and
  UndefinedBehaviorSanitizer; the original code fails its subscription assertion.
- `./fbt flash_usb` completed package upload and the self-update command.

The final hardware outcome is owner-reported. Separate final G4/G5/G6/G7
measurements, counter readings, and radio negotiation values were not captured
by the agent. The large-file results above are browser/mock evidence, not a
measured hardware throughput claim. Local logs and individual harness outcomes
are under `.omo/evidence/ble-deploy-restoration/` (gitignored).

## Start with the symptom

| Symptom | First check |
| --- | --- |
| USB bootstrap repeatedly stops near the same frame, such as frame 720 | Check the bootstrap inactivity watchdog and deployed asset freshness; a repeatable cutoff is a clue to a deterministic deadline, not proof of an SD or queue limit. |
| BLE Connect times out immediately after a slow picker/pairing step | Start the stream promise and watchdog after GATT discovery and subscription, immediately before the trigger write. |
| BLE is connected, USB key exchange retries, and `TXERR` grows while both forwarding counters stay zero | Check TX subscription and HID event ownership; connection/discovery success does not prove serial event delivery. |
| BLE bootstrap logs `trigger 0x42 sent` but receives no header | Check serial RX event delivery and that the Flipper is on the matching BLE Deploy Waiting screen. |
| BLE pairing asks for keyboard-style PIN entry | Verify the effective GAP appearance is `GAP_APPEARANCE_UNKNOWN` (`0x0000`), even with HIDS advertised. |
| BLE typing reports failure despite a working HID link | Check the GATT/HID return-value convention: `true` means error at that layer. |
| Bluetooth picker cannot find an already-bonded device | The host HID daemon may hold the link; inspect subscription state and the pairing-aware 15-second watchdog. |
| `runfap.py` raises `IndexError` | Use paired `-s <local>` and `-t <device path>` arguments, not a bare positional FAP path. |
| Serial access fails with `Operation not permitted` | Check the agent sandbox before diagnosing a missing or hung Flipper. |

## HID handler swallowed serial events

The failing hardware session showed `U->B=0`, `B->U=0`, `DROP=0`, `TXERR=5`.
The USB page reported `Key exchange failed: Retry exhausted for 10:0`; the BLE
page waited for the USB peer. USB frames had reached the relay, but BLE TX
failed. The BLE deploy trigger was also lost.

The complete cause was in the shared firmware
[`hid_service.c`](../../lib/ble_profile/extra_services/hid_service.c):

1. AirBridge creates serial before HID.
2. The [dispatcher](../../targets/f7/ble_glue/furi_ble/event_dispatcher.c)
   inserts each new handler at the head, so HID runs first.
3. HID acknowledged every `ACI_GATT_ATTRIBUTE_MODIFIED` event, regardless of
   its attribute handle. Dispatch stops at the first acknowledgment.
4. The FAP [serial handler](../../applications_user/pocket_airbridge/airbridge_serial_service.c)
   therefore never received its TX client-configuration descriptor (CCCD)
   subscription writes or RX value writes. `client_subscribed` stayed false,
   TX was rejected, and `0x42` never reached the relay.

The fix restricts HID acknowledgments to the HID service's reserved attribute
range, using the same count as service allocation. HID also leaves indication
confirmations to other services: its reports use notifications. Reordering
service startup would only hide the ownership bug. Removing the serial
subscription guard would not restore RX delivery.

The stock serial handler was investigated but its normal stop path unregisters
it; stale stock handles were not the cause of this incident. The regression
[`airbridge_ble_dispatch_test.c`](../tests/airbridge_ble_dispatch_test.c)
executes the real dispatcher and both services in both registration orders,
including subscription, trigger/frame delivery, disconnect, HID writes, and
service-boundary checks.

**Flash the full firmware for this fix.** HID is a shared firmware service;
uploading a FAP alone cannot replace it. The running FAP owns USB and removes
CDC, so on-screen counters are the available live diagnostics; do not expect
serial logging while Bridge runs. `TXERR` counts failed sends, not a unique
cause. A single cancel-race error differs from persistent failures on an
otherwise connected link.

## Bootstrap deadlines and logs

USB and BLE deploy streams use an **8-second inactivity watchdog**, refreshed
by each incoming report/notification (`self.ABT` can override it for testing).
A healthy stream may take longer than eight seconds in total. Do not turn this
back into an absolute transfer deadline.

The BLE stream promise and watchdog must be created after transport discovery
and TX subscription, before writing `0x42`. Creating them at the Connect click
lets a human picker/pairing delay reject the promise before streaming begins.
Discovery and stream waiting are separate phases. Successful BLE completion
disconnects the bootstrap intentionally; its disconnect listener must not paint
a failure after `phase='done'`.

- USB: preserve the visible failure log or inspect `ABLOG` / call `ABDUMP()` in
  the bootstrap page. Note picker/open results, report IDs and sizes, first
  header, frame/byte progress, and checksum result.
- BLE: preserve `[ble-deploy]` messages for connect, discovery, `subscribed TX`,
  trigger, `HDR`, completion, checksum, and the failing phase. A successful
  GATT write alone does not prove that the FAP received its event.
- Trigger routing is transport-specific and armed only in Deploy Waiting. A
  `0x42` in Bridge is ordinary relay data; other Deploy states reject it as
  `DEPLOY NOT ARMED`.
- Bootstrap logging is retained in this revision. Editing either snippet,
  including removing logs, changes its digest: regenerate bundles/pins, rebuild
  the FAP, and deploy matching assets before testing again.

See the [bootstrap wire contract](protocol.md#dual-transport-bootstrap-separate-unchanged-protocol)
for the header, checksum, SD container, and transport differences.

## BLE pairing, advertising, and typing

The working policy advertises **AirBridge serial + HIDS** throughout the app's
lifetime. `airbridge_ble_ensure_serial_adv` reasserts the idempotent HIDS policy
and restarts advertising only from GAP idle. Earlier HIDS advertising-window
plans are superseded. Advertising a keyboard service does not authorize typing;
keyboard reports remain gated by the on-device Deploy confirmation.

The effective GAP appearance is deliberately **unknown (`0x0000`)**. The profile
overrides the configured keyboard appearance: exposing `0x03C1` made macOS ask
for keyboard-style PIN entry. For first-time BLE Deploy pairing on macOS, pair
through **System Settings → Bluetooth** so the OS binds the HID keyboard before
using Web Bluetooth. Confirm numeric-comparison codes through the physical
gates. A browser serial connection alone does not prove keyboard binding.

The [BLE QA runbook](../scripts/ble_qa_runbook.md) records a known scanner
mismatch: its Windows/Linux GATT check still expects the configured appearance,
not the effective override. Keep the measured result and classify that row as
a tooling mismatch; do not revert the firmware policy to make it pass.

The bonded host HID daemon can hold the peripheral link without subscribing to
serial TX. The watchdog permits 15 seconds for subscription and restarts that
window while `bt_pairing_in_progress` is true. A four-second variant was tried
and rejected on hardware because it disconnected clients during discovery.
Do not shorten this window to compensate for broken subscription dispatch.
There is no squatter kick during Typing, Streaming, or the Deploy prompt.
Deploy Waiting separately permits 90 seconds without an accepted trigger; Done
has a four-second grace for the old bootstrap connection.

The return convention also matters: `ble_gatt_characteristic_update` and
`ble_svc_hid_update_input_report` return **true on error**.
`airbridge_profile_kb_report` must invert that result to provide the app's
**true on successful send** convention. Preserve this inversion.

## Deployment and QA checks

Use the [firmware guide](firmware-guide.md#deploy) for the exact upload commands.
Before launch, verify one FAP copy at `/ext/apps/Tools/pocket_airbridge.fap` and
matching `bootstrap.js`, `bootstrap-ble.js`, `app-usb.html.gz`, and
`app-ble.html.gz` under `/ext/apps_data/pocket_airbridge/`. Compare remote sizes
with the actual local artifacts; FAP and bundle sizes change between builds.
Never deploy to `/ext/apps/` root. Build before upload; `runfap.py` needs
`-s <local> -t <canonical device path>`.

If Codex can reach Chrome but cannot open the serial device, its network and
device permissions are separate. In `~/.codex/config.toml`, the top-level
`approval_policy = "on-request"` allows device-access requests;
`[sandbox_workspace_write]` with `network_access = true` enables CDP networking.
Restart the session after changing these settings. A sandbox-denied probe is
not evidence of a hung Flipper. If already-installed submodules cannot be
synchronized because `.git` is protected, `FBT_NO_SYNC=1 ./fbt` builds without
updating them; it does not install missing dependencies.

Use the persistent Chrome session on `http://localhost:9222`. Before every
browser gate, verify the actual process/window and activate the exact URL;
do not assume a requested tab is frontmost. A background harness caused two
focus failures during this session. The 4 MiB paired-page fixture crosses over
80,000 ACKed DATA frames and exceeded its original 15-second deadline; that fixture
now gets 60 seconds. This is a test budget, not the bootstrap watchdog.

After a firmware/FAP/web change, record these regressions before declaring
hardware acceptance:

| Gate | Required evidence |
| --- | --- |
| G4 | USB→BLE and BLE→USB text, matching SAS accepted on both pages; forwarding counters advance, healthy `DROP=0`, `TXERR=0`. |
| G5 | Attachment completion and receiver SHA-256, with size/direction recorded. |
| G6 | USB Deploy types the pinned bootstrap, streams the entire bundle, and boots the USB page. |
| G7 | BLE Deploy types through the OS-bound keyboard, subscribes to serial, receives the trigger/header/complete bundle, and boots the BLE page. |
| Protocol harness | All current tests pass; the 2026-09-15 baseline is 275/275, not the historical 17/17. |

Use the root [AGENTS.md](../../AGENTS.md) physical gates: check before asking,
one backgrounded attention sound, one action with confirm/cancel, and no polling
for a human action. Keep screenshots and captures in the permitted evidence or
temporary directories. Record observed results separately from expected values.
