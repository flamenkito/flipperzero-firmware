#!/usr/bin/env python3
"""Measure an abt TCP path with deterministic, incompressible, SHA-256 checked data."""

import argparse
import concurrent.futures
import hashlib
import ipaddress
import json
import socket
import statistics
import time

MAX_SIZE = 16 * 1024 * 1024


def address(value):
    host, port = value.rsplit(":", 1)
    ip = ipaddress.ip_address(host)
    if ip.version != 4 or not ip.is_loopback or not 0 < int(port) < 65536:
        raise argparse.ArgumentTypeError("use a loopback IPv4 address and port")
    return host, int(port)


def payload(size, direction):
    return hashlib.shake_256(f"abt-bench-v1-{direction}".encode()).digest(size)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def send_json(sock, value):
    sock.sendall(json.dumps(value, separators=(",", ":")).encode() + b"\n")


def read_json(reader):
    line = reader.readline(4097)
    if not line or len(line) > 4096 or not line.endswith(b"\n"):
        raise ValueError("missing or oversized benchmark response")
    return json.loads(line)


def read_exact(reader, size):
    data = reader.read(size)
    if len(data) != size:
        raise ValueError(f"stream ended at {len(data)} of {size} bytes")
    return data


def exchange(sock, reader, outgoing, incoming_size):
    # Read and write concurrently so duplex tests cannot deadlock on socket buffers.
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        sending = pool.submit(sock.sendall, outgoing)
        incoming = read_exact(reader, incoming_size)
        sending.result()
    return incoming


def serve_connection(sock):
    sock.settimeout(120)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    with sock.makefile("rb") as reader:
        while True:
            request = read_json(reader)
            mode = request["mode"]
            if mode == "quit":
                return
            if mode == "ping":
                send_json(sock, request)
                continue
            size = request["size"]
            if mode not in ("upload", "download", "duplex") or not 1 <= size <= MAX_SIZE:
                raise ValueError("invalid benchmark request")
            outgoing = payload(size, "server") if mode != "upload" else b""
            expected = payload(size, "client") if mode != "download" else b""
            send_json(sock, {"ready": True})
            incoming = exchange(sock, reader, outgoing, len(expected))
            if digest(incoming) != digest(expected):
                raise ValueError("client-to-server SHA-256 mismatch")
            send_json(sock, {"received_sha256": digest(incoming)})


def serve(args):
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(args.listen)
        listener.listen(1)
        print(f"Benchmark server listening on {args.listen[0]}:{args.listen[1]}", flush=True)
        while True:
            sock, peer = listener.accept()
            with sock:
                try:
                    serve_connection(sock)
                    print(f"PASS {peer}: all requested hashes matched", flush=True)
                except (OSError, ValueError, KeyError, TypeError) as error:
                    print(f"FAIL {peer}: {error}", flush=True)
            if args.once:
                return


def client(args):
    with socket.create_connection(args.connect, timeout=120) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with sock.makefile("rb") as reader:
            rtts = []
            for seq in range(21):
                request = {"mode": "ping", "seq": seq}
                start = time.perf_counter()
                send_json(sock, request)
                if read_json(reader) != request:
                    raise ValueError("ping mismatch")
                if seq:
                    rtts.append((time.perf_counter() - start) * 1000)
            print(json.dumps({"label": args.label, "mode": "ping", "samples": len(rtts),
                              "median_ms": statistics.median(rtts), "max_ms": max(rtts)}), flush=True)
            for round_number in range(1, args.rounds + 1):
                for mode in ("upload", "download", "duplex"):
                    outgoing = payload(args.size, "client") if mode != "download" else b""
                    expected = payload(args.size, "server") if mode != "upload" else b""
                    send_json(sock, {"mode": mode, "size": args.size})
                    if read_json(reader) != {"ready": True}:
                        raise ValueError("server not ready")
                    start = time.perf_counter()
                    incoming = exchange(sock, reader, outgoing, len(expected))
                    reply = read_json(reader)
                    elapsed = time.perf_counter() - start
                    if digest(incoming) != digest(expected) or reply["received_sha256"] != digest(outgoing):
                        raise ValueError("transfer SHA-256 mismatch")
                    print(json.dumps({"label": args.label, "round": round_number, "mode": mode,
                                      "tx_bytes": len(outgoing), "rx_bytes": len(incoming),
                                      "seconds": elapsed,
                                      "combined_kib_s": (len(outgoing) + len(incoming)) / elapsed / 1024,
                                      "tx_sha256": digest(outgoing), "rx_sha256": digest(incoming)}), flush=True)
            send_json(sock, {"mode": "quit"})
            sock.shutdown(socket.SHUT_WR)
            if reader.read(1):
                raise ValueError("unexpected trailing bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    server = commands.add_parser("serve")
    server.add_argument("--listen", type=address, default=("127.0.0.1", 18080))
    server.add_argument("--once", action="store_true")
    server.set_defaults(run=serve)
    test = commands.add_parser("client")
    test.add_argument("--connect", type=address, default=("127.0.0.1", 2222))
    test.add_argument("--size", type=int, default=32768)
    test.add_argument("--rounds", type=int, default=1)
    test.add_argument("--label", default="abt")
    test.set_defaults(run=client)
    args = parser.parse_args()
    if args.command == "client" and (not 1 <= args.size <= MAX_SIZE or not 1 <= args.rounds <= 100):
        parser.error("size must be 1..16777216 bytes and rounds 1..100")
    args.run(args)


if __name__ == "__main__":
    main()
