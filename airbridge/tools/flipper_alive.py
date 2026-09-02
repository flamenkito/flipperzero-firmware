#!/usr/bin/env python3
"""Flipper Zero alive probe — detects ALIVE / HUNG / ABSENT states."""

import sys
import glob
import time
import argparse
import serial


def find_default_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def do_probe(port):
    try:
        ser = serial.Serial(port, 115200, timeout=5.0)
    except serial.SerialException as e:
        return "ABSENT", str(e)
    try:
        ser.reset_input_buffer()
        ser.write(b"\r")
        ser.flush()
        deadline = time.time() + 5.0
        buf = b""
        while time.time() < deadline:
            avail = ser.in_waiting
            if avail:
                buf += ser.read(avail)
                if b">:" in buf:
                    ser.close()
                    reboot = b"Firmware version:" in buf
                    return "ALIVE", reboot
            time.sleep(0.1)
        ser.close()
        return "HUNG", None
    except serial.SerialException as e:
        ser.close()
        return "ABSENT", str(e)


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--port")
    parser.add_argument("--wait", type=float, default=None)
    args = parser.parse_args()

    port = args.port if args.port else find_default_port()
    if not port:
        print("ABSENT: no usbmodem port")
        sys.exit(1)

    if args.wait is not None:
        deadline = time.time() + args.wait
        while True:
            verdict, detail = do_probe(port)
            print(f"{verdict}: {port}" + (f" ({detail})" if detail else ""))
            if verdict == "ALIVE":
                sys.exit(0)
            if time.time() + 2 > deadline:
                sys.exit(2 if verdict == "HUNG" else 1)
            time.sleep(2)

    verdict, detail = do_probe(port)
    if verdict == "ALIVE":
        msg = f"ALIVE: CLI responsive on {port}"
        if detail:
            msg += " (recent reboot detected)"
        print(msg)
        sys.exit(0)
    elif verdict == "HUNG":
        print(f"HUNG: port {port} enumerates but firmware not answering CLI")
        sys.exit(2)
    else:
        print(f"ABSENT: {detail}")
        sys.exit(1)


if __name__ == "__main__":
    main()
