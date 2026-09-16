# abt — native Pocket AirBridge tunnel

`abt` carries one full-duplex TCP connection between two computers through the
Flipper's existing USB HID ↔ BLE serial bridge. It needs no browser or firmware
change. macOS has passed hardware tests with both endpoints on one Mac. A
Windows x64 build is available; Windows USB and Mac-to-WSL hardware QA is pending.

USB remains HID: the default HP identity is `03f0:5341`, using only vendor usage
`ff00:0001`, with 64-byte reports. `abt` sends no keyboard reports.

## Build

### macOS

Install Rust 1.90 or newer and Apple's command-line developer tools, then run
from the repository root:

```sh
cargo build --release --locked --manifest-path airbridge/native/Cargo.toml
```

The executable is `airbridge/native/target/release/abt`. Copy it to a directory
on your PATH on each Mac, or use its full path in the commands below. Copying a
binary requires compatible CPU architecture and macOS; build on each Mac if
necessary. `Cargo.lock` pins dependencies. No browser assets need rebuilding.

### Windows x64 build

A Windows x64 executable is cross-built using Rust's `x86_64-pc-windows-gnu`
target and MinGW-w64. On this Mac, with MinGW-w64 installed:

```sh
rustup target add x86_64-pc-windows-gnu
CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
  cargo build --release --locked --target x86_64-pc-windows-gnu \
  --manifest-path airbridge/native/Cargo.toml
```

Output: `airbridge/native/target/x86_64-pc-windows-gnu/release/abt.exe`.
This is a Windows console application; run it in PowerShell on Windows 10/11 x64.
The 2026-09-16 build passed `--version` and `usb --help` checks under Wine 11.0.
Physical Windows USB access and the complete WSL path still need verification.

Local build artifacts (under the ignored `target/` directory):

- ZIP with executable, setup instructions, license, and checksum:
  `airbridge/native/target/dist/abt-windows-x64.zip`
- Unpacked executable: `airbridge/native/target/dist/abt-windows-x64/abt.exe`

After extracting the ZIP, compare `Get-FileHash .\abt.exe -Algorithm SHA256` in
PowerShell with its `SHA256SUMS` file. The build command above produces the raw
executable; the ZIP is a separately prepared local artifact.

## SSH

### USB client → BLE server

1. Keep Pocket AirBridge running on its **Bridge** screen. Disconnect browser
   AirBridge sessions so only the two native endpoints own the data interfaces.
2. On the BLE computer, start `abt` with the address of an existing SSH server
   reachable from that computer:

   ```sh
   abt ble --connect 127.0.0.1:22 --scan-seconds 20
   ```

   Allow Bluetooth access if macOS prompts; confirm any pairing prompt on the
   Flipper. Wait for `BLE subscribed` and `waiting for stream`.
3. On the USB computer:

   ```sh
   abt usb --listen 127.0.0.1:2222
   ```

4. Connect with the system SSH client:

   ```sh
   ssh -p 2222 -o HostKeyAlias=airbridge-peer user@127.0.0.1
   ```

   Verify the remote server's host-key fingerprint through a trusted channel on
   first connection. `HostKeyAlias` gives this server a stable identity in
   `known_hosts`; use a distinct alias for each server. Normal SSH authentication
   still applies. `abt` does not enable macOS Remote Login or install an SSH server.

Start either endpoint first. Each accepted local connection gets a new stream
identifier. Once the previous stream fully closes (including a one-second
retry grace period by default), another TCP connection can use the same running
`abt` pair. Ctrl-C stops an endpoint and closes its device connection.

### Mac client → Windows USB → WSL server

Keep the Flipper on its **Bridge** screen and disconnect competing AirBridge
clients. Run `abt.exe` on the Windows host and an SSH server inside WSL:

```text
Mac SSH → abt BLE listener → Flipper → abt.exe USB connector → WSL sshd
```

Configure WSL's SSH server to use port `2222`, then verify it from Windows
PowerShell before starting the tunnel:

```powershell
ssh -p 2222 your_wsl_user@127.0.0.1
```

Check the SSH host-key fingerprint and complete a login. WSL normally forwards
its listening ports to Windows localhost; custom networking settings may need
adjustment. See [Microsoft's WSL networking guide](https://learn.microsoft.com/en-us/windows/wsl/networking#accessing-linux-networking-apps-from-windows-localhost).

In Windows PowerShell, beside the extracted executable:

```powershell
.\abt.exe devices usb
.\abt.exe usb --connect 127.0.0.1:2222
```

On the Mac, start the BLE listener and wait for `BLE subscribed` and `listening
on 127.0.0.1:2222`:

```sh
abt ble --listen 127.0.0.1:2222 --scan-seconds 20
```

Then SSH from a second Mac terminal, verifying the same WSL SSH host key:

```sh
ssh -p 2222 -o HostKeyAlias=airbridge-wsl your_wsl_user@127.0.0.1
```

The two `2222` listeners are on different computers. USB remains HID, owned by
the Windows endpoint; this setup needs no USB forwarding into WSL. `abt` does
not install or start WSL's SSH server. End-to-end Windows/WSL hardware QA is pending.

### REST and WebSockets inside SSH

Keep **one SSH connection** open and let SSH multiplex application connections.
For an API/WebSocket server listening on port 8080 on the SSH server's computer:

```sh
ssh -N -p 2222 -o HostKeyAlias=airbridge-peer -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:18080:127.0.0.1:8080 user@127.0.0.1
curl http://127.0.0.1:18080/health
```

A native WebSocket client can connect to `ws://127.0.0.1:18080/ws` at the same
time. Host/Origin restrictions remain the API server's normal rules. For several
remote destinations, use SSH's SOCKS proxy instead:

```sh
ssh -N -p 2222 -o HostKeyAlias=airbridge-peer -o ExitOnForwardFailure=yes \
  -D 127.0.0.1:1080 user@127.0.0.1
curl --socks5-hostname 127.0.0.1:1080 https://api.example.com/health
```

For the Mac-to-WSL setup, use `HostKeyAlias=airbridge-wsl` and your WSL username
in these commands; the forwarded destination is reached from inside WSL. Run
`curl` or the WebSocket client in another terminal while `ssh -N` stays running.

The SSH server must be able to reach the destination. SOCKS-aware native
WebSocket clients can use the same proxy. SSH forwards TCP, so UDP/QUIC is outside
this tool's scope. All channels share the bridge's modest throughput.

### Direct TCP forwarding

Either transport can listen or connect. For example, `abt ble --connect
api.example.com:443` plus `abt usb --listen 127.0.0.1:8443` forwards one TLS
connection. Preserve the real hostname for certificate verification and SNI:

```sh
curl --connect-to api.example.com:443:127.0.0.1:8443 https://api.example.com/health
```

**ABT1 itself provides neither encryption nor peer authentication.** Use SSH or
TLS for confidential traffic. Direct HTTP/WS is plaintext on the bridge; CRC32
only detects accidental damage. Browser SAS/AES-GCM is a separate protocol and
is not used by `abt`. Bluetooth pairing is not end-to-end application security.
The connector permits only its operator-configured destination; the other peer
cannot request arbitrary target addresses. Listeners bind only to loopback, but
other local processes can still connect to them.

## Limits and recovery

- One active TCP connection per `abt` pair. Extra connections are accepted and
  immediately closed. Use SSH forwarding for concurrent requests/sessions.
- Two unacknowledged 40-byte DATA/FIN frames per direction; bounded queues and
  TCP backpressure. The Flipper remains an unchanged eight-event relay.
- Default retry interval 500 ms, opening deadline 30 s, target TCP connection
  deadline 10 s, heartbeat every 2 s, peer timeout 12 s, delivery-stall timeout
  60 s. CLI overrides: `--retry-ms`, `--peer-timeout`, `--stall-timeout`.
- TCP half-close works: a request can finish sending and still receive a response.
- Link failure closes the current TCP session. There is no transparent reconnect
  or resume. Restart an endpoint after physical USB/BLE disconnection, then
  establish a fresh SSH/TCP connection.
- Sequence numbers never wrap; a very long stream must reconnect before consuming
  2^32 DATA/FIN sequence numbers in either direction (at most about 160 GiB).
- ABT1 does not interoperate with the browser chat protocol. Run native endpoints
  on both sides; stop them before returning to browser chat or Deploy.

## Troubleshooting

```sh
abt devices usb
abt devices ble --scan-seconds 20
abt usb --help
abt ble --help
```

| Symptom | Check |
| --- | --- |
| No USB device | App running, cable attached, Bridge screen; match configured VID/PID. `--vid` and `--pid` accept hex. |
| Multiple USB entries | Composite keyboard entries are normal. `abt` opens only `ff00:0001`; use its exact `path=` value with `--device` if several matching Flippers exist. |
| No BLE peer | Return to Bridge, close other BLE clients, scan for 20 seconds. macOS can occupy the HID connection until the app's 15-second watchdog releases it. |
| More than one BLE peer | Use `--device` with an exact ID or name from `abt devices ble`. |
| BLE permission denied | Allow Bluetooth for the terminal/application launching `abt` in macOS Privacy & Security. The executable includes a Bluetooth usage description. A development sandbox can deny hardware access even when macOS permissions are already granted. |
| macOS USB open denied | Check Input Monitoring/USB permissions and other HID clients. Shared-device mode avoids exclusively claiming the composite keyboard. |
| Windows reports no matching vendor HID collection | Keep the app in Bridge; inspect `.\abt.exe devices usb` for `usage=ff00:0001` and close competing clients. Use the native Windows executable; no WSL USB attachment or WinUSB driver replacement is part of this setup. |
| Windows cannot SSH to WSL on `127.0.0.1:2222` | Check WSL's SSH server, configured port, and localhost forwarding first. This local login must work before `abt.exe usb --connect 127.0.0.1:2222` can reach it. |
| Peer refused stream | Read the connector's stderr: its fixed TCP target may not be listening. Check that address from the connector computer. |
| Extra TCP connection closes | One stream is active or still finishing its retry grace period. Put concurrent requests inside one SSH connection. |
| Heartbeat timeout / stalled stream | Inspect physical connection and Flipper `DROP`/`TXERR`, stop competing clients. Restore transport and start a new connection; increase stall timeout only for an intentionally slow consumer. |

Session logs go to stderr and include byte counts, retries, and maximum pending
frames when a stream closes. They do not print payloads, SSH credentials, or keys.

## Validation

```sh
cargo test --locked --manifest-path airbridge/native/Cargo.toml
cargo clippy --locked --manifest-path airbridge/native/Cargo.toml --all-targets -- -D warnings
cargo fmt --manifest-path airbridge/native/Cargo.toml -- --check
```

Tests cover full-duplex byte integrity, lost/corrupt/reordered/duplicate frames,
handshake/FIN retries, half-close, slow consumers, stale sessions, conflicting
retransmissions, impossible ACKs, peer timeout, busy rejection, target refusal,
and sequential reuse. See the [wire protocol](../docs/native-tunnel-protocol.md)
for the complete ABT1 contract.

| Configuration | Verified | Still pending |
| --- | --- | --- |
| macOS USB + macOS BLE on one Mac | 21 host tests, Clippy, release build, physical SSH/HTTP/WebSocket and SSH forwarding | Separate-computer validation |
| Windows x64 executable | Release cross-build, PE/import inspection, `--version` and `usb --help` under Wine 11.0 | Native Windows execution and USB HID access |
| Mac BLE → Windows USB → WSL SSH | CLI roles implemented and setup documented | Complete physical end-to-end test |

### Hardware result — 2026-09-16

On one Mac with both native endpoints and the current Pocket AirBridge FAP:

- Native OpenSSH command with public-key authentication and strict host-key
  verification passed.
- Direct HTTP echoed 32,771 bytes each way with matching SHA-256; 28.899 seconds,
  **2.215 KiB/s aggregate application payload** across request and response.
- Native WebSocket text, 8,193-byte binary, ping/pong, and close frames passed.
- One SSH connection carried concurrent HTTP and WebSocket requests, a SOCKS5
  health request, and a second SSH command. The 32,771-byte HTTP echo took
  29.874 seconds while those other channels shared the link.
- All successful streams reported zero retries and at most two pending frames.
  User confirmed Flipper **DROP 0, TXERR 0** afterwards.
- 21 automated tests, Clippy with warnings denied, and the release build passed.

These are local same-Mac checks, not a two-computer/platform compatibility claim
or a guaranteed link rate. Evidence and test helpers are in the local ignored
`.omo/evidence/native-tunnel/` directory. No firmware/FAP/browser runtime changes
were needed; this task did not repeat their previously completed regression suite.
