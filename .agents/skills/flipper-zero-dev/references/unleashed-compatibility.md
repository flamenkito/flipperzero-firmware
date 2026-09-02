# Unleashed Firmware Compatibility

Source inspection of `darkflippers/unleashed-firmware` `dev` commit
`e343383472aff5fa111f33a3c8597b27419138a5` found useful building blocks but no
drop-in Pocket AirBridge transport.

## USB

Unleashed exports the U2F HAL interface with bidirectional 64-byte, report-ID-0
interrupt reports. It uses usage page `0xF1D0`. Chromium's WebHID blocklist
protects the complete FIDO usage page, so this interface cannot replace the
AirBridge `0xFF00` vendor collection. Unleashed also lacks the required
keyboard-plus-vendor composite profile.

## BLE

Unleashed exports Serial profile send and callback functions, but those APIs
need the active profile instance. The instance owned by `bt_service` has no
safe public getter, and incoming bytes still flow directly to RPC. Restarting
the profile from a FAP is destructive and disconnects clients.

## Porting decision

Using Unleashed still requires porting the AirBridge USB HAL profile, the small
`bt_service` raw RX/TX hook, API-symbol exports, and the FAP. Do not add a
parallel custom UUID family unless the browser transport is changed with it.
