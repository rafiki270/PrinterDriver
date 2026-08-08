#!/usr/bin/env python3
"""Capability probe for ESC/POS print-completion acknowledgements.

See docs/testing-plan.md for interpretation of results and required
fault-injection follow-up tests. Stop CUPS and any other client before
running — this needs exclusive use of the printer connection.

Usage:
    python3 scripts/printer_probe.py <printer-ip> [port]
"""
from __future__ import annotations

import socket
import sys
import time

ESC_INITIALIZE = b"\x1b@"
ASB_OFF = b"\x1d\x61\x00"
DLE_EOT_PRINTER = b"\x10\x04\x01"
GS_R_PAPER = b"\x1d\x72\x01"


def process_id_command(token: bytes) -> bytes:
    if len(token) != 4:
        raise ValueError("Process ID must be exactly four bytes")
    if any(value < 0x20 or value > 0x7E for value in token):
        raise ValueError("Process ID must contain printable ASCII only")
    return b"\x1d\x28\x48\x06\x00\x30\x30" + token


def receive_until_idle(
    sock: socket.socket,
    total_timeout: float,
    idle_timeout: float = 0.3,
) -> bytes:
    deadline = time.monotonic() + total_timeout
    last_data_at: float | None = None
    result = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            result.extend(chunk)
            last_data_at = time.monotonic()
        except socket.timeout:
            if (
                last_data_at is not None
                and time.monotonic() - last_data_at >= idle_timeout
            ):
                break
    return bytes(result)


def main() -> None:
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <printer-ip> [port]", file=sys.stderr)
        raise SystemExit(1)

    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9100

    print(f"Connecting to {host}:{port}...")
    with socket.create_connection((host, port), timeout=3.0) as sock:
        print("Connected")
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(0.2)

        # Reset and disable unsolicited ASB so the probe output is simpler.
        sock.sendall(ESC_INITIALIZE + ASB_OFF)
        receive_until_idle(sock, total_timeout=0.5)

        # Does the interface return any real-time status at all?
        sock.sendall(DLE_EOT_PRINTER)
        response = receive_until_idle(sock, total_timeout=2.0)
        print(
            "DLE EOT response:",
            response.hex(" ") if response else "NO RESPONSE",
        )

        # Test Epson GS ( H Function 48 process-ID completion marker.
        token = b"P001"
        expected = b"\x37\x22" + token + b"\x00"
        sock.sendall(b"GS(H) completion probe P001\n\n" + process_id_command(token))
        response = receive_until_idle(sock, total_timeout=10.0)
        print(
            "GS(H):",
            "SUPPORTED" if expected in response else "NO MATCH",
            response.hex(" ") if response else "NO RESPONSE",
        )

        # Test queued GS r 1 print-completion response.
        sock.sendall(b"GS r completion probe\n\n" + GS_R_PAPER)
        response = receive_until_idle(sock, total_timeout=10.0)
        print(
            "GS r response:",
            response.hex(" ") if response else "NO RESPONSE",
        )


if __name__ == "__main__":
    main()
