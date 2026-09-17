# abt — native Pocket AirBridge tunnel

`abt` carries one full-duplex TCP connection between two computers through the
AirBridge device’s existing USB HID ↔ BLE serial bridge. It needs no browser or firmware
change. macOS has passed hardware tests with both endpoints on one Mac. The
Mac BLE → Windows USB → WSL2 path has also passed an interactive SSH test,
confirmed by the user on two computers.

USB remains HID: the default HP identity is `03f0:5341`, using only vendor usage
`ff00:0001`, with 64-byte reports. `abt` sends no keyboard reports.

## Daily start: Mac → Windows/WSL

After the [one-time SSH setup](#mac-client--windows-usb--wsl-server), keep the
AirBridge device on **Bridge** with USB plugged into Windows. Use these four terminals.
The examples put the Windows package in WSL's `~/projects/abt-0.3.0`; adjust
that path if you extracted it elsewhere. `WSL_USER` is the result of `whoami`
inside WSL, not necessarily the Windows account name.

**WSL terminal 1 — SSH server.** If `ss -ltn 'sport = :2222'` already shows the
intended SSH listener, keep that instance. Otherwise:

```sh
sudo mkdir -p /run/sshd
sudo /usr/sbin/sshd -D -e -p 2222 -o ListenAddress=127.0.0.1
```

**WSL terminal 2 — Windows USB endpoint.** The user runs this locally in WSL;
Windows executable launches and restarts are not performed by the agent or
through an agent's SSH command:

```sh
cd ~/projects/abt-0.3.0
./abt.exe usb --connect 127.0.0.1:2222
```

**Mac terminal 1 — BLE endpoint.** First check `lsof -nP -iTCP:2222`. Reuse an
existing `abt` listener for this tunnel, including one started by the agent.
When no endpoint is running, start it from the repository root:

```sh
cd /path/to/airbridge-repository
airbridge/native/target/release/abt ble --listen 127.0.0.1:2222 --scan-seconds 20
```

Wait for `BLE subscribed` and `listening on 127.0.0.1:2222`.

**Mac terminal 2 — SSH shell:**

```sh
ssh -p 2222 -o HostKeyAlias=airbridge-wsl WSL_USER@127.0.0.1
```

Verify the server host key on first use. [Install your public key](#passwordless-ssh-over-the-wsl-tunnel)
once for passwordless login; optionally add `-C` for [SSH compression](#ssh-compression).
Leave the two `abt` endpoints and foreground `sshd` running. On the first SSH
connection, both updated endpoints should log `connected; window=4`. The faster
defaults need no extra flags. DROP and TXERR should stay zero.

Run `exit` in the SSH shell before starting a separate SCP/SSH connection, then
wait for the endpoint's `stream ... closed` log, which follows a closing grace
period of about one second by default. To stop the tunnel, finish the SSH session and press
Ctrl-C in each endpoint terminal. Stop the foreground `sshd` separately when
finished with it. Restarting `abt` alone does not start or stop the SSH server.

Further tasks: [copy files or upgrade](#copy-files-and-upgrade-the-windows-endpoint),
[forward REST/WebSockets](#rest-and-websockets-inside-ssh),
[measure throughput](#reproducible-throughput-check),
and [troubleshoot](#troubleshooting).

## Build

### macOS

Install Rust 1.90 or newer and Apple's command-line developer tools, then run
from the repository root:

```sh
python3 airbridge/native/tools/build_release.py macos
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
python3 airbridge/native/tools/build_release.py windows
```

Output: `airbridge/native/target/x86_64-pc-windows-gnu/release/abt.exe`.
This is a Windows console application; run it in PowerShell on Windows 10/11 x64.
The initial 0.1.0 build passed `--version` and `usb --help` checks under Wine 11.0,
then a physical Mac-to-WSL interactive SSH test using the Windows USB endpoint.
The 0.2.0 build subsequently passed file-integrity, throughput, and concurrent
HTTP/WebSocket forwarding checks on that pair; see the results below.

Local build artifacts (under the ignored `target/` directory):

- ZIP with executable, setup instructions, license, and checksum:
  `airbridge/native/target/dist/abt-windows-x64.zip`
- Unpacked executable: `airbridge/native/target/dist/abt-windows-x64/abt.exe`

After extracting the ZIP, compare `Get-FileHash .\abt.exe -Algorithm SHA256` in
PowerShell with its `SHA256SUMS` file. The build command above produces the executable and ZIP together, remaps local
repository paths in executable diagnostics, and regenerates package checksums.

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
   AirBridge device. Wait for `BLE subscribed` and `waiting for stream`.
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

Keep the AirBridge device on its **Bridge** screen and disconnect competing AirBridge
clients. Run `abt.exe` on the Windows host and an SSH server inside WSL:

```text
Mac SSH → abt BLE listener → AirBridge device → abt.exe USB connector → WSL sshd
```

#### 1. Start SSH inside WSL

Configure WSL's SSH server to use port `2222`. For Ubuntu/Debian, if no SSH server
is listening there, install it and start a foreground instance in a dedicated
WSL terminal:

```sh
sudo apt update
sudo apt install openssh-server
sudo mkdir -p /run/sshd
sudo /usr/sbin/sshd -D -e -p 2222 -o ListenAddress=127.0.0.1
```

Leave that terminal open; expect `Server listening on 127.0.0.1 port 2222`.
This instance stops with Ctrl-C. See the [Ubuntu installation guide](https://ubuntu.com/server/docs/how-to/security/openssh-server/)
and [OpenSSH command options](https://man.openbsd.org/sshd.8). If startup fails,
resolve the displayed error before starting the tunnel. In another WSL terminal,
`ss -ltn 'sport = :2222'` must show a listening socket.

#### 2. Start abt from another WSL terminal

Open another WSL terminal and find the Linux account name:

```sh
whoami
```

Use that name wherever `WSL_USER` appears below. First test with the Windows
SSH client so it exercises the same network path as `abt.exe`:

```sh
ssh.exe -p 2222 WSL_USER@127.0.0.1
```

Check the SSH host-key fingerprint and complete a login, then run `exit` to
return to the original WSL terminal. A plain Linux `ssh` command only checks
the WSL side. WSL normally forwards
its listening ports to Windows localhost; custom networking settings may need
adjustment. See [Microsoft's WSL networking guide](https://learn.microsoft.com/en-us/windows/wsl/networking#accessing-linux-networking-apps-from-windows-localhost).

For the package extracted into WSL's `~/projects`, use:

```sh
cd ~/projects/abt-0.3.0
./abt.exe devices usb
./abt.exe usb --connect 127.0.0.1:2222
```

Alternatively, copy `abt.exe` into the Windows Downloads folder. Replace
`WINDOWS_USER` with the Windows account's directory name (it can differ from
`WSL_USER`) and run the same commands from that directory:

```sh
cd "/mnt/c/Users/WINDOWS_USER/Downloads"
./abt.exe devices usb
./abt.exe usb --connect 127.0.0.1:2222
```

WSL launches it as a Windows process, so USB access still belongs to Windows.
See [Microsoft's interoperability documentation](https://learn.microsoft.com/en-us/windows/wsl/filesystems#run-windows-tools-from-linux).
Leave `abt.exe` running in this terminal.
All Windows `abt.exe` commands, including device and version checks, are run by
the user in a local WSL or PowerShell terminal.

Alternatively, launch it from Windows PowerShell beside the extracted executable:

```powershell
.\abt.exe devices usb
.\abt.exe usb --connect 127.0.0.1:2222
```

#### 3. Connect from the Mac

On the Mac, start the BLE listener and wait for `BLE subscribed` and `listening
on 127.0.0.1:2222`:

```sh
abt ble --listen 127.0.0.1:2222 --scan-seconds 20
```

Leave the BLE endpoint running. Then SSH from a second Mac terminal, verifying
the same WSL SSH host key and authenticating with your WSL account:

```sh
ssh -p 2222 -o HostKeyAlias=airbridge-wsl WSL_USER@127.0.0.1
```

The two `2222` listeners are on different computers. USB remains HID, owned by
the Windows endpoint; this setup needs no USB forwarding into WSL. `abt` does
not install or start WSL's SSH server. This interactive SSH path passed on
2026-09-16; see the [hardware result](#mac-to-wsl-hardware-result--2026-09-16).

### Passwordless SSH over the WSL tunnel

Check for an existing key with `ls ~/.ssh/id_ed25519.pub`. If that file is
absent, generate a key on the Mac with `ssh-keygen -t ed25519` and follow its
prompts, preserving any existing key files.

With both endpoints and WSL's SSH server running, install an existing Mac public
key using one SSH connection. This example uses `~/.ssh/id_ed25519.pub`; keep its
private key on the Mac. Run on the Mac, replacing `WSL_USER`:

```sh
cat ~/.ssh/id_ed25519.pub | ssh \
  -p 2222 -o HostKeyAlias=airbridge-wsl WSL_USER@127.0.0.1 \
  'umask 077; mkdir -p ~/.ssh && chmod 700 ~/.ssh && { printf "\n"; cat; } >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys'
```

Enter the WSL password once. After that connection closes, allow about two
seconds for the tunnel's closing grace period, then verify:

```sh
ssh -T -p 2222 -o HostKeyAlias=airbridge-wsl -o BatchMode=yes \
  -o IdentitiesOnly=yes -i ~/.ssh/id_ed25519 WSL_USER@127.0.0.1 \
  'printf "KEY_LOGIN_OK\n"'
```

If the private key has a passphrase, unlock it in the Mac's SSH agent first:

```sh
ssh-add ~/.ssh/id_ed25519
```

This changes SSH authentication only; host-key verification still applies.
The macOS `ssh-copy-id` helper opens successive SSH connections for version and
key checks. Its next connection can be rejected while `abt` is still closing the
previous stream. The single-connection command above avoids that race.

### SSH compression

For terminal output, logs, source code, and uncompressed JSON, try SSH's `-C`
option to reduce the bytes carried by the bridge:

```sh
ssh -C -p 2222 -o HostKeyAlias=airbridge-wsl WSL_USER@127.0.0.1
```

Replace `WSL_USER` with your WSL account name (`whoami` inside WSL). If already
connected, exit the Mac SSH shell first, keeping both `abt` endpoints and WSL's
`sshd` running, then reconnect with this command.

`-C` requests compression before SSH encryption, including forwarded TCP traffic.
It keeps SSH encryption and host-key verification enabled. See the
[OpenSSH compression option](https://man.openbsd.org/ssh.1#C). You can also add
`-C` to the `ssh -L` and `ssh -D` examples below.

Already-compressed files (such as ZIP, gzip, JPEG, and video) and traffic already
encrypted inside SSH (such as HTTPS/WSS) usually gain little and can incur extra
overhead. Compression does not increase the physical HID/BLE link rate; results
depend on the data. Compare with `-o Compression=no` for an explicit disabled
baseline. Compression gains on the Windows/WSL pair have not yet been measured.

### Copy files and upgrade the Windows endpoint

Keep both endpoints and WSL's SSH server running. Exit any interactive SSH
shell before starting SCP, and allow the previous tunnel stream to finish
closing. These commands run on the **Mac**, from the repository root; the
destination `projects/` is relative to the WSL user's home directory:

```sh
scp -P 2222 -o HostKeyAlias=airbridge-wsl \
  -X nrequests=2 -X buffer=4096 \
  airbridge/native/target/dist/abt-windows-x64.zip \
  WSL_USER@127.0.0.1:projects/abt-0.3.0.zip
```

For any other file, replace the source and destination filenames. Create
`~/projects` in WSL first if needed. SCP uses uppercase `-P` for the SSH port.
The `-X` options work with the tested Mac OpenSSH 9.9 client; they limit SFTP
request size and concurrency so progress updates arrive sooner on this link.
Allow the copy to finish. Avoid opening another SSH session during it.

After the copy completes, use a **local WSL terminal** to extract and verify.
Choose a fresh destination directory if an endpoint is already running from
`~/projects/abt-0.3.0`; retain the old executable until the new pair works:

```sh
python3 -m zipfile -e ~/projects/abt-0.3.0.zip ~/projects/abt-0.3.0
cd ~/projects/abt-0.3.0
sha256sum -c SHA256SUMS
chmod +x abt.exe
```

Require all checksum lines to say `OK`. In the old Windows endpoint's terminal press Ctrl-C,
then the user starts the new executable:

```sh
cd ~/projects/abt-0.3.0
./abt.exe --version
./abt.exe usb --connect 127.0.0.1:2222
```

Expect version `0.3.0`. Restart the Mac endpoint using the matching updated
macOS build and the [daily-start command](#daily-start-mac--windowswsl).
The Windows ZIP contains only the Windows executable; build/update the Mac
binary separately. Reconnect SSH and confirm `window=4` in the stream log.
Keep WSL's `sshd` running throughout the upgrade. Rebuilding or copying a file
does not replace an already-running process, or update other copies in Downloads.

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
- Up to four unacknowledged 40-byte DATA/FIN frames per direction in 0.2.0;
  negotiation falls back to two with 0.1.0 peers. Queues and TCP backpressure
  remain bounded. The AirBridge device remains an unchanged eight-event relay.
- Default retry interval 500 ms, opening deadline 30 s, target TCP connection
  deadline 10 s, heartbeat after 2 s of peer silence, peer timeout 12 s, delivery-stall timeout
  60 s. CLI overrides: `--retry-ms`, `--peer-timeout`, `--stall-timeout`.
- TCP half-close works: a request can finish sending and still receive a response.
- Link failure closes the current TCP session. There is no transparent reconnect
  or resume. Restart an endpoint after physical USB/BLE disconnection, then
  establish a fresh SSH/TCP connection.
- Sequence numbers never wrap; a very long stream must reconnect before consuming
  2^32 DATA/FIN sequence numbers in either direction (at most about 160 GiB).
- ABT1 does not interoperate with the browser chat protocol. Run native endpoints
  on both sides; stop them before returning to browser chat or Deploy.

## Protocol speed controls (0.2.0)

The default maximum window is now four frames, allowing 160 payload bytes in
flight per direction. Cumulative ACKs cover two completed frames at a time,
with a 4 ms deadline for a single frame. This increases the amount of data that
can cross each round trip and reduces reverse ACK traffic. USB remains HID;
the 64-byte report layout, firmware, and browser protocol are unchanged.
Active DATA/ACK traffic suppresses redundant heartbeats to keep relay slots
available for the larger window.

Upgrade both endpoints for the larger window. A mixed 0.1.0/0.2.0 pair uses two
frames; a new listener takes about one extra second to open an old connector
with default timers. The new endpoint's `connected; window=N` log shows the
negotiated result. `--window 2 --ack-delay-ms 0` on **both** new endpoints
reproduces the original window and ACK behavior for comparison. Normal commands
without these flags select the new defaults. The speed gain requires hardware
measurement; doubling the window does not promise double throughput.

### BLE packet aggregation (0.3.0)

Current BLE builds also negotiate packing up to three 64-byte reports into one
BLE operation with the updated FAP. The connection log reports `packet_frames=3`,
`2`, or legacy `1`. Run commands stay the same. USB remains HID and ABT1 framing
is unchanged, so an existing Windows USB endpoint can communicate with the new
Mac BLE endpoint. Refresh the FAP and rebuild the Mac executable to test this
mode. It falls back to single reports with older FAPs; a failed commit requires
reconnecting. This does not increase the four-frame send window. Hardware speed
results and remaining checks are recorded in [ADR 0007](../../docs/adr/0007-ble-packets-and-sliding-windows.md).

Version 0.3.0 passed a same-Mac 32 KiB SHA-256 benchmark with three-report BLE
packets: 3.44 KiB/s BLE→USB, 2.96 KiB/s USB→BLE, and 3.48 KiB/s combined duplex.
Both endpoint logs reported zero retries and peak pending four. This is not a
Windows/WSL speed comparison. The Windows 0.3.0 executable is cross-built and
packaged; its physical Windows regression is still to be run by the user.
Release packages include setup instructions, benchmark helper, license and
SHA-256 checksums. CLI text and packaged files use AirBridge device naming;
release builds remap repository paths out of executable diagnostics.

### Reproducible throughput check

`tools/bench.py` uses Python 3's standard library. It measures upload, download,
and simultaneous duplex transfers with deterministic, incompressible bytes and
SHA-256 checks. The client reports JSON lines. For duplex, `combined_kib_s` sums
both directions; it is not the speed of each direction. Timings exclude tunnel
setup and include completion feedback. Ping measurements exclude the first
request, which opens the tunnel. Each mode uses synthetic data; default size is
32,768 bytes per active direction. For two rounds, one TCP session carries all
six transfers. The helper exits with an error on a mismatch or a broken stream.

#### Through SSH — the method used for the recorded results

Keep the normal SSH arrangement: Windows `abt.exe usb --connect 127.0.0.1:2222`
and Mac `abt ble --listen 127.0.0.1:2222`. Close other SSH sessions first. From
the Mac repository root, copy the helper if it is not already at this WSL path:

```sh
scp -P 2222 -o HostKeyAlias=airbridge-wsl \
  -X nrequests=2 -X buffer=4096 \
  airbridge/native/tools/bench.py WSL_USER@127.0.0.1:projects/abt-bench.py
```

After SCP's stream closes, run this on the Mac, with public-key login already
working. It starts the Python server inside WSL and opens a local SSH forward:

```sh
ssh -T -p 2222 -o HostKeyAlias=airbridge-wsl \
  -o StrictHostKeyChecking=yes -o BatchMode=yes \
  -o IdentitiesOnly=yes -i ~/.ssh/id_ed25519 \
  -o Compression=no -o ExitOnForwardFailure=yes \
  -L 127.0.0.1:28081:127.0.0.1:28080 WSL_USER@127.0.0.1 \
  'python3 ~/projects/abt-bench.py serve --listen 127.0.0.1:28080 --once'
```

Wait for `Benchmark server listening on 127.0.0.1:28080`. In another Mac terminal,
from the repository root:

```sh
python3 airbridge/native/tools/bench.py client \
  --connect 127.0.0.1:28081 --label optimized-window4-ssh --rounds 2
```

The helper and SSH forward finish when this client completes. The two `abt`
endpoints and WSL `sshd` stay running. Compare JSON `combined_kib_s` by direction,
the endpoint close logs, and AirBridge device DROP/TXERR. To compare the original window
and ACK behavior, restart both endpoints with `--window 2 --ack-delay-ms 0`,
repeat with a `baseline` label, then restore both endpoints to their default
commands. The user performs each Windows executable restart. Keep compression
disabled for these measurements; measure its effect separately with real data.

#### Direct TCP — optional, without SSH overhead

For Mac BLE → Windows USB → WSL, copy `bench.py` beside the Windows executable.
Stop the USB endpoint before changing its target. In a dedicated WSL terminal,
from the package directory:

```sh
python3 bench.py serve
```

Leave the server running. Use a second WSL terminal to start the USB endpoint:

```sh
./abt.exe usb --connect 127.0.0.1:18080 --window 2 --ack-delay-ms 0
```

On the Mac, restart its endpoint with the matching baseline settings:

```sh
airbridge/native/target/release/abt ble --listen 127.0.0.1:2222 --scan-seconds 20 --window 2 --ack-delay-ms 0
```

Then run the client from another Mac terminal at the repository root:

```sh
python3 airbridge/native/tools/bench.py client --label baseline --rounds 2
```

After the client finishes, restart both endpoints without `--window` and
`--ack-delay-ms`, keeping their same TCP addresses. Run the client again with
`--label optimized --rounds 2`. Compare speeds, the `retries`/`peak_pending`/
`acks_sent` close logs, and AirBridge device DROP/TXERR. Return the USB endpoint's
`--connect` to `127.0.0.1:2222` afterwards to use the existing WSL SSH server.

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
| Multiple USB entries | Composite keyboard entries are normal. `abt` opens only `ff00:0001`; use its exact `path=` value with `--device` if several matching AirBridge devices exist. |
| No BLE peer | Return to Bridge, close other BLE clients, scan for 20 seconds. macOS can occupy the HID connection until the app's 15-second watchdog releases it. |
| More than one BLE peer | Use `--device` with an exact ID or name from `abt devices ble`. |
| BLE permission denied | Allow Bluetooth for the terminal/application launching `abt` in macOS Privacy & Security. The executable includes a Bluetooth usage description. A development sandbox can deny hardware access even when macOS permissions are already granted. |
| macOS USB open denied | Check Input Monitoring/USB permissions and other HID clients. Shared-device mode avoids exclusively claiming the composite keyboard. |
| Windows reports no matching vendor HID collection | Keep the app in Bridge; inspect `.\abt.exe devices usb` for `usage=ff00:0001` and close competing clients. Use the native Windows executable; no WSL USB attachment or WinUSB driver replacement is part of this setup. |
| Windows cannot SSH to WSL on `127.0.0.1:2222` | Check WSL's SSH server, configured port, and localhost forwarding first. This local login must work before `abt.exe usb --connect 127.0.0.1:2222` can reach it. |
| WSL `ss -ltn 'sport = :2222'` shows no listener | Start `sshd` using the commands above and leave its foreground terminal running. Starting `abt.exe` does not start the SSH server. |
| Connector logs `closed: tx=0 rx=21` and SSH closes before its greeting | The connector received client bytes but read EOF from its target without receiving any bytes. Test that target with Windows `ssh.exe` and inspect the `sshd` log; check for a missing listener before changing the bridge. |
| Peer refused stream | Read the connector's stderr: its fixed TCP target may not be listening. Check that address from the connector computer. |
| Extra TCP connection closes | One stream is active or still finishing its retry grace period. Put concurrent requests inside one SSH connection. |
| `ssh-copy-id` fails with `Connection closed` | Its consecutive probe connections can race the preceding stream's closing grace period. Use the single-connection public-key installation command above. |
| SCP stays at `0% — stalled` | Default SFTP writes are 32 KiB with up to 64 outstanding requests; the first progress update can take time over this link. Try `scp -X nrequests=2 -X buffer=4096 -P 2222 -o HostKeyAlias=airbridge-wsl FILE WSL_USER@127.0.0.1:DEST`. Avoid a concurrent SSH shell and check `abt` logs before treating the progress display as a transport failure. |
| Heartbeat timeout / stalled stream | Inspect physical connection and AirBridge device `DROP`/`TXERR`, stop competing clients. Restore transport and start a new connection; increase stall timeout only for an intentionally slow consumer. |

Session logs go to stderr and include byte counts, retries, ACKs sent, and maximum pending
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
sequential reuse, window negotiation, old-peer fallback, ACK coalescing, and
the single-frame ACK deadline, and duplex traffic through a simulated shared
eight-slot relay. See the [wire protocol](../docs/native-tunnel-protocol.md)
for the complete ABT1 contract.

| Configuration | Verified | Still pending |
| --- | --- | --- |
| 0.2.0 protocol optimization | 29 host tests, Clippy, macOS/Windows builds; real Windows/WSL speed comparison, SHA-256, passwordless SSH, and user-confirmed DROP 0 / TXERR 0 | Separate disconnect/recovery testing |
| macOS USB + macOS BLE on one Mac | 21 host tests, Clippy, release build, physical SSH/HTTP/WebSocket and SSH forwarding | Separate-computer validation |
| Windows x64 executable | Release cross-build, PE/import inspection, Wine CLI checks, physical USB endpoint carrying interactive SSH | Windows BLE endpoint and disconnect/recovery testing |
| Mac BLE → Windows USB → WSL SSH | Password and public-key login, SCP checksum, duplex SHA-256 benchmarks, concurrent HTTP/WebSocket forwarding, DROP 0 / TXERR 0 | Separate disconnect/recovery testing |

### Same-Mac hardware result — 2026-09-16

With `abt` 0.1.0 on one Mac and the current Pocket AirBridge FAP:

- Native OpenSSH command with public-key authentication and strict host-key
  verification passed.
- Direct HTTP echoed 32,771 bytes each way with matching SHA-256; 28.899 seconds,
  **2.215 KiB/s aggregate application payload** across request and response.
- Native WebSocket text, 8,193-byte binary, ping/pong, and close frames passed.
- One SSH connection carried concurrent HTTP and WebSocket requests, a SOCKS5
  health request, and a second SSH command. The 32,771-byte HTTP echo took
  29.874 seconds while those other channels shared the link.
- All successful streams reported zero retries and at most two pending frames.
  User confirmed AirBridge device **DROP 0, TXERR 0** afterwards.
- 21 automated tests, Clippy with warnings denied, and the release build passed.

These are local same-Mac checks, not a two-computer/platform compatibility claim
or a guaranteed link rate. Evidence and test helpers are in the local ignored
`.omo/evidence/native-tunnel/` directory. No firmware/FAP/browser runtime changes
were needed; this task did not repeat their previously completed regression suite.

### Mac-to-WSL hardware result — 2026-09-16

With `abt` 0.1.0, the user confirmed a successful SSH password login from the Mac through BLE,
the AirBridge device, Windows USB `abt.exe`, and WSL's SSH server. The remote shell
reported **Ubuntu 22.04.5 LTS**, kernel
`6.6.87.1-microsoft-standard-WSL2`, architecture `x86_64`. Interactive `ls`,
`cd`, and a subsequent `ls` completed. This verifies real Windows USB transport
and the two-computer interactive SSH path; it is user-reported terminal evidence.

The initial connection closed before the SSH greeting. Windows `abt` logged
`closed: tx=0 rx=21 retries=0 peak_pending=1`; direct Windows `ssh.exe` to
`127.0.0.1:2222` returned `Connection refused`, and WSL's
`ss -ltn 'sport = :2222'` showed no listener. Starting the foreground `sshd`
command above resolved the failure. No `abt` or firmware change was required.
The foreground SSH server must remain running while the tunnel is used.

File-transfer integrity, REST/WebSocket forwarding, throughput, and AirBridge device
DROP/TXERR counts were not measured in that initial interactive test. The earlier
same-Mac results remain separate. A sanitized evidence record is stored locally
at `.omo/evidence/native-tunnel/windows-wsl-ssh.json`.

### Protocol optimization result — 2026-09-16

The real Mac BLE → Windows USB → WSL path passed two rounds of each benchmark
mode before and after the change. Each round transferred 32,768 bytes per active
direction with matching SHA-256. Tests ran inside SSH forwarding with compression
disabled. Rates below are total application bytes divided by total measured time
across both rounds; duplex sums the two simultaneous directions.

| Payload direction | Original window/ACK behavior | 0.2.0 defaults | Gain |
| --- | ---: | ---: | ---: |
| Mac → WSL | 1.759 KiB/s | 2.639 KiB/s | 50.0% |
| WSL → Mac | 1.776 KiB/s | 2.547 KiB/s | 43.4% |
| Duplex, combined | 2.510 KiB/s | 2.978 KiB/s | 18.7% |

Median application ping round trip fell from 90.93 to 75.36 ms (20 samples per
configuration). Both Mac endpoint logs reported zero retries. The optimized
session negotiated window 4 and peaked at four pending frames; the baseline
used the old Windows 0.1.0 binary and a Mac endpoint forced to window 2 with
immediate ACKs. The original eight-slot AirBridge device relay, USB HID identity, firmware,
and browser assets were unchanged.

ACK coalescing reduced ACK counts by about half in the unpaced host tests, but
Mac-side hardware counts only changed from 3,453 to 3,403 in these SSH runs.
Do not assume the 4 ms ACK deadline batches every pair on this BLE connection.
These are measurements of this setup, not a guaranteed rate on other hosts.

Passwordless public-key SSH and a 993,435-byte SCP upload also passed. The
uploaded ZIP and all final package files were SHA-256 verified in WSL. The
initial copy took 9:17 through the original two-frame transport; a small binary
patch delivered the final heartbeat fix, with the reconstructed executable
verified against the local build. The user starts and restarts Windows `abt.exe`;
the agent runs the Mac endpoint and SSH-based benchmark helpers.

Concurrent HTTP and WebSocket requests shared one SSH connection with the new
defaults. HTTP echoed 32,771 bytes each way with matching SHA-256 in 24.375 s;
WebSocket text, 8,193-byte binary data, ping/pong, and close frames all matched.
The Mac endpoint recorded zero retries and four pending frames at peak.

The user confirmed **DROP 0, TXERR 0** on the AirBridge device after the optimized tests.
Evidence: `.omo/evidence/native-throughput/`. A separate physical
disconnect/recovery test was not run for this version.
