# External FAP API Symbol Checklist

Every HAL or service function called by `pocket_airbridge.fap` must be declared
in a public header and exported from `targets/f7/api_symbols.csv`.

Required groups include:

- `usb_airbridge`
- USB profile lookup, label, identity, VID/PID, and keyboard-capability APIs
- Vendor HID connect state, callback, RX, nonblocking TX, and blocking TX APIs
- Deploy keyboard press, release, and release-all APIs
- `bt_set_raw_serial_callback`
- `bt_serial_tx`

After adding APIs, run the FAP build. The SDK generator may add entries marked
`?` and bump the API version. Review every generated change; mark only the
intentionally public entries `+`. Do not blindly accept unrelated generated
symbols or hand-copy an API version from a different firmware build or branch.

If the FAP links but reports unresolved symbols at launch, confirm all three:

1. The function is declared in an exported header.
2. Its CSV entry has status `+` and the exact generated signature.
3. The device is running the same firmware build used to compile the FAP.
