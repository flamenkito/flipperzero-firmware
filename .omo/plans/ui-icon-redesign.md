# Plan: Bridge Screen Icon Redesign

## Goal

Replace the text-only counter header (`U->B`, `B->U`, `!DROP`, `XTXERR`) on the
FAP Bridge screen with horizontal icon composites, and make the USB/BT glyphs
double as live link-state indicators (filled = connected, outline = down).

## Approved design (from chat, user-confirmed)

```
┌──────────────────────────────────────────────────────────────┐
│ Pocket AirBridge                                             │ y=10  title (unchanged)
│ HP 725 K+M                                                   │ y=19  identity (unchanged)
│                                                              │
│ [usb]▸[bt]   [bt]▸[usb]   [trash]   [alert]                  │ y=24  ICON HEADER
│  1234         567          0          0                      │ y=42  digits, centered
│                                                              │
│ ↑ USB deploy              ↓ BLE deploy                       │ y=47  unchanged
│ BACK: exit                                                   │ y=63  unchanged
└──────────────────────────────────────────────────────────────┘
```

Decisions locked via question tool:
- USB glyph: **plug outline** (7x8), not trident (mush at 7px), not USB-C port
- Link state: **filled vs outline** inside the composites (no title-row chips, no underline bar)
- Stat icons: **trash can** (DROP) + **alert triangle** (TXERR)
- **Drop the `V%lu` vendor-OUT debug counter** from the main screen

## Implementation facts (verified)

- FAPs cannot reference built-in firmware icons (`I_*` variables are NOT in
  `api_symbols.csv`). Embed XBM arrays in the FAP source.
- `canvas_draw_xbm(canvas, x, y, w, h, bits)` is exported; format is standard
  XBM: row-major, LSB-first (bit 0 = leftmost pixel), rows padded to whole
  bytes, 0-bits transparent (`canvas_draw_u8g2_bitmap_int` in
  `applications/services/gui/canvas.c`).
- Link state is already tracked: file-static `usb_connected` (updated from
  `EVENT_TYPE_USB` bridge events) and `app->ble_connected` (updated by
  `app_ble_status_changed_callback`). Both readable from `render_bridge`.
- Stock pixel art to borrow verbatim: BT rune from
  `assets/icons/StatusBar/Bluetooth_Idle_5x8.png`, alert triangle from
  `assets/icons/StatusBar/Alert_9x8.png` (grids decoded, below).

## Icon bitmaps (starting values; pixel-tune on device)

```c
// USB plug outline 7x8
static const uint8_t icon_usb_outline[] = {
    0x3E, 0x55, 0x41, 0x3E, 0x04, 0x04, 0x04, 0x1C,
};
// USB plug filled 7x8 (connected; body solid, contact slits as holes)
static const uint8_t icon_usb_filled[] = {
    0x3E, 0x6B, 0x7F, 0x3E, 0x04, 0x04, 0x04, 0x1C,
};
// BT rune 5x8 (stock Bluetooth_Idle_5x8)
static const uint8_t icon_bt_rune[] = {
    0x04, 0x0D, 0x16, 0x0C, 0x0C, 0x16, 0x0D, 0x04,
};
// Right arrow 5x5
static const uint8_t icon_arrow_r[] = {
    0x04, 0x08, 0x1F, 0x08, 0x04,
};
// Trash can 8x8 (DROP)
static const uint8_t icon_trash[] = {
    0x7E, 0x81, 0x55, 0x55, 0x55, 0x55, 0x81, 0x7E,
};
// Alert triangle 9x8 (TXERR, stock Alert_9x8)
static const uint8_t icon_alert[] = {
    0x10, 0x00, 0x38, 0x00, 0x28, 0x00, 0x6C, 0x00,
    0x6C, 0x00, 0xFE, 0x00, 0xEE, 0x00, 0xFF, 0x01,
};
```

BT "filled" (connected): draw rune, then `canvas_draw_box(x, y+6, 5, 2)` as a
solid pedestal over the sparse bottom rows (keeps the 8px footprint; rune is a
line glyph so it cannot be body-filled like the plug).

## Layout coordinates

| Element | x | y | w×h | Notes |
|---|---|---|---|---|
| Title | 0 | 10 | — | FontPrimary, unchanged |
| Identity | 0 | 19 | — | FontSecondary, unchanged |
| U→B composite | 0..20 | 24 | 7+2+5+2+5 | USB @0, arrow @9/y26, BT @16 |
| B→U composite | 32..52 | 24 | 5+2+5+2+7 | BT @32, arrow @39/y26, USB @46 |
| Trash | 64 | 24 | 8×8 | DROP |
| Alert | 96 | 24 | 9×8 | TXERR |
| Digits | centered | 42 | — | centers: x=10, 42, 68, 100; use `canvas_string_width` |
| Deploy hints | 0/64 | 47-53 | — | unchanged |
| BACK hint | 0 | 63 | — | unchanged |

## Tasks

- [x] **T1: Icon arrays + glyph helpers** in
  `~/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c`:
  the six XBM arrays above plus `draw_usb_glyph(canvas, x, y, bool connected)`,
  `draw_bt_glyph(canvas, x, y, bool connected)`.
- [x] **T2: Rewrite `render_bridge`** — icon header row at y=24 with the two
  direction composites (glyphs filled by `usb_connected` / `app->ble_connected`),
  trash + alert columns, digits centered under each group via
  `canvas_string_width`, `V%lu` line removed. Title/identity/deploy/BACK rows
  untouched.
- [x] **T3: Build + deploy + visual check** — `./fbt build
  APPSRC=applications_user/pocket_airbridge`, exit app on device, `storage.py
  send` + `runfap.py`, eyeball icon legibility/alignment on the 128×64 screen.
  Follow the hardware-qa skill gates (attention signal, question tool,
  check-before-gating).
- [x] **T4: Pixel-tune** — iterate bitmaps on hardware until glyphs read
  cleanly (plug slits, rune pedestal, arrow balance, digit centering).
- [x] **T5b: Fix deploy Waiting pump vs pairing race** — the BLE-deploy Waiting pump calls `bt_disconnect()` every `BLE_WAITING_PUMP_MS` until `ble_waiting_ever_connected`, but `BtStatusConnected` only fires after pairing completes, so the pump kills in-progress pairings (6× `gattserverdisconnected`, dialog vanishes before user can confirm). Guard the pump with a link-active/pairing-in-progress signal. User-approved scope addition (2026-07-23); consent model (per-connection numeric code, bonding off) stays.
- [x] **T5: Regression suite** (AGENTS.md regression rule): bridge chat text
  both directions, file transfer with SHA-256, USB deploy E2E, BLE deploy E2E,
  protocol harness 17/17, exit identity restore.
- [x] **T6: Publish + docs** — copy updated FAP source to
  `docs/firmware/pocket_airbridge/pocket_airbridge.c`, update
  `docs/firmware-guide.md` status-screen description (icon header + filled =
  connected encoding), commit flipper-hid repo. Firmware repo: FAP lives in
  gitignored `applications_user/`, so only docs/firmware copy carries it.

## Non-goals

- Other screens (Deploy prompt, Typing, Waiting, Streaming, Done, Error) stay
  text-only. A keyboard glyph for the Typing screen is a possible follow-up.
- No changes to protocol, transports, identity, or deploy logic.
