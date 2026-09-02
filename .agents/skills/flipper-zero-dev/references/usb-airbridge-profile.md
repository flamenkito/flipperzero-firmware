# Pocket AirBridge USB Profile Contract

Use a dedicated USB HAL profile. The stock keyboard HID profile is one-way and
the stock U2F profile uses FIDO usage page `0xF1D0`, which Chromium WebHID
blocks. Cloning U2F is useful as an implementation pattern, not as the shipped
interface.

## Required vendor collection

- Top-level usage page: `0xFF00`
- Usage: `0x01`
- Input usage: `0x20`
- Output usage: `0x21`
- Report ID: implicit `0`
- Input report: 64 bytes
- Output report: 64 bytes
- Interrupt endpoints in both directions

For the composite profile use distinct endpoint indices: keyboard IN `0x81`,
vendor IN `0x82`, vendor OUT `0x03`. An IN and OUT endpoint with the same index
can overwrite each other's STM32 endpoint configuration.

## Runtime rules

Apply the configured profile once on the first event-loop turn after GUI setup.
Hold it for the application's lifetime and restore the previous USB mode only
when the app exits. Composite-to-composite reconfiguration is forbidden.

The default profile is `hp_kbd_vendor`, VID `0x03F0`, PID `0x5341`. The FAP may
read another profile label from `/ext/apps_data/pocket_airbridge/config`, but
the menu line is display-only and must not switch profiles.

Bridge mode may use only the vendor collection. Keyboard reports are permitted
only after the user selects `Deploy app`, confirms cursor placement on-device,
and while the screen visibly shows `TYPING...`; BACK must abort immediately.
