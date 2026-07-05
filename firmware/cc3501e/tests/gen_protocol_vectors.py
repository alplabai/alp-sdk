#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Regenerate firmware/cc3501e/tests/protocol_vectors.txt.

Authoritative, human-readable source-of-truth for the cc3501e-bridge
wire frames, shared by the firmware transport tests
(tests/zephyr/cc3501e_bridge_transport/) and any future host-driver tests
under tests/zephyr/chips/cc3501e/.

The cc3501e frame is a 4-byte little-endian header + payload, with NO
start-of-frame byte and NO CRC (a short hardwired point-to-point link):

    REQUEST : cmd | flags | payload_len(LE16) | payload[payload_len]
    REPLY   : cmd | flags | payload_len(LE16) | status | data[...]

The reply echoes the request cmd, uses flags=0 (solicited), and carries
the response status (ALP_CC3501E_RESP_*) as the first payload byte.

Run from the alp-sdk repo root:

    python3 firmware/cc3501e/tests/gen_protocol_vectors.py

Use --check in CI to fail on drift.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

# --- Opcodes / codes -- keep aligned with include/alp/protocol/cc3501e.h.
CMD_PING = 0x00
CMD_GET_VERSION = 0x01
CMD_RESET = 0x02
CMD_GET_MAC = 0x03
CMD_WIFI_SCAN_START = 0x10  # representative not-yet-implemented v1 opcode
CMD_SOCK_OPEN = 0x20
CMD_SOCK_CLOSE = 0x24

FLAG_SOLICITED = 0x00

RESP_OK = 0x00
RESP_ERR_INVALID = 0x01
RESP_ERR_BUSY = 0x02
RESP_ERR_NOT_READY = 0x05
RESP_ERR_PROTOCOL = 0x07

# Wire-protocol version GET_VERSION reports (ALP_CC3501E_PROTOCOL_VERSION).
PROTOCOL_VERSION = 2


def frame(cmd: int, flags: int, payload: bytes = b"") -> bytes:
    """Build a cc3501e frame: cmd | flags | payload_len(LE16) | payload."""
    return bytes([cmd, flags, len(payload) & 0xFF, (len(payload) >> 8) & 0xFF]) + payload


def reply(cmd: int, status: int, data: bytes = b"") -> bytes:
    """Build a solicited reply frame (payload = status + data)."""
    return frame(cmd, FLAG_SOLICITED, bytes([status]) + data)


HEADER = """\
# cc3501e-bridge canonical wire-test vectors
#
# Consumed by the firmware transport tests
# (tests/zephyr/cc3501e_bridge_transport/) and any future host-driver
# tests (tests/zephyr/chips/cc3501e/), so the two sides cannot diverge.
#
# Frame: 4-byte LE header [cmd | flags | payload_len(LE16)] + payload.
# No SOF, no CRC.  Reply payload[0] is the response status.
#
# Format: one `<name> = <hex>` vector per non-comment line; `#` comments.
# Regenerate with `python3 firmware/cc3501e/tests/gen_protocol_vectors.py`.
"""


def build_vectors() -> list[tuple[str, str, str | None]]:
    out: list[tuple[str, str, str | None]] = []

    out.append(("ping_request", frame(CMD_PING, 0).hex().upper(),
                "cmd=PING | flags=0 | len=0"))
    out.append(("ping_reply_ok", reply(CMD_PING, RESP_OK).hex().upper(),
                "cmd=PING | flags=0 | len=1 | status=OK"))

    out.append(("get_version_request", frame(CMD_GET_VERSION, 0).hex().upper(),
                "cmd=GET_VERSION | flags=0 | len=0"))
    out.append((
        f"get_version_reply_proto{PROTOCOL_VERSION}",
        reply(CMD_GET_VERSION, RESP_OK,
              bytes([PROTOCOL_VERSION & 0xFF, (PROTOCOL_VERSION >> 8) & 0xFF])).hex().upper(),
        f"cmd=GET_VERSION | len=3 | status=OK | version={PROTOCOL_VERSION} (LE16)",
    ))

    out.append(("get_mac_request", frame(CMD_GET_MAC, 0).hex().upper(),
                "cmd=GET_MAC | flags=0 | len=0"))
    # GET_MAC is async (P0-4/P0-6): the first request submits the worker job
    # and replies BUSY; the host re-issues until the worker has the result.
    out.append(("get_mac_reply_busy_submitted", reply(CMD_GET_MAC, RESP_ERR_BUSY).hex().upper(),
                "cmd=GET_MAC | len=1 | status=BUSY -- job submitted, host re-issues"))
    out.append(("get_mac_reply_not_ready_stub", reply(CMD_GET_MAC, RESP_ERR_NOT_READY).hex().upper(),
                "cmd=GET_MAC | len=1 | status=NOT_READY -- re-issued; stub has no radio"))

    out.append(("reset_request", frame(CMD_RESET, 0).hex().upper(),
                "cmd=RESET | flags=0 | len=0"))
    out.append(("reset_reply_ok", reply(CMD_RESET, RESP_OK).hex().upper(),
                "cmd=RESET | len=1 | status=OK -- firmware reboots after the ack is read"))

    out.append((
        "wifi_scan_start_reply_invalid",
        reply(CMD_WIFI_SCAN_START, RESP_ERR_INVALID).hex().upper(),
        "cmd=WIFI_SCAN_START | len=1 | status=INVALID -- v1 opcode not implemented in v0.1",
    ))

    # TCP/UDP sockets (0x20..0x24): worker-routed, poll-by-repeat like GET_MAC.
    # SOCK_OPEN req = sock_open_t { family=IPV4(0) | type=STREAM(0) | proto=0 | rsvd }.
    out.append(("sock_open_tcp_request",
                frame(CMD_SOCK_OPEN, 0, bytes([0x00, 0x00, 0x00, 0x00])).hex().upper(),
                "cmd=SOCK_OPEN | len=4 | family=IPV4 type=STREAM proto=0"))
    # First request submits the worker job and replies BUSY; host re-issues.
    out.append(("sock_open_reply_busy_submitted", reply(CMD_SOCK_OPEN, RESP_ERR_BUSY).hex().upper(),
                "cmd=SOCK_OPEN | len=1 | status=BUSY -- job submitted, host re-issues"))
    # Re-issued on the stub (no IP stack) -> NOT_READY.
    out.append(("sock_open_reply_not_ready_stub",
                reply(CMD_SOCK_OPEN, RESP_ERR_NOT_READY).hex().upper(),
                "cmd=SOCK_OPEN | len=1 | status=NOT_READY -- re-issued; stub has no IP stack"))
    # SOCK_CLOSE req = sock_close_t { handle(LE16)=1 | reserved(LE16) } = 4 B.
    out.append(("sock_close_request",
                frame(CMD_SOCK_CLOSE, 0, bytes([0x01, 0x00, 0x00, 0x00])).hex().upper(),
                "cmd=SOCK_CLOSE | len=4 | handle=1"))
    # Bad length: a 3-byte SOCK_OPEN payload is rejected up front (not worker-routed).
    out.append(("sock_open_bad_len_reply_invalid",
                reply(CMD_SOCK_OPEN, RESP_ERR_INVALID).hex().upper(),
                "cmd=SOCK_OPEN | len=1 | status=INVALID -- payload length != sizeof(sock_open_t)"))

    # Framing error: declared payload_len doesn't match the captured bytes.
    out.append((
        "ping_bad_len_reply_protocol",
        reply(CMD_PING, RESP_ERR_PROTOCOL).hex().upper(),
        "reply to a frame whose payload_len mismatches the byte count -> PROTOCOL error",
    ))

    return out


def emit(vectors: list[tuple[str, str, str | None]]) -> str:
    chunks = [HEADER]
    for name, value, comment in vectors:
        if comment:
            chunks.append(f"# {comment}")
        chunks.append(f"{name:<34} = {value}")
    chunks.append("")
    return "\n".join(chunks)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the on-disk file does not match the generated content")
    parser.add_argument("--out", type=pathlib.Path,
                        default=pathlib.Path(__file__).parent / "protocol_vectors.txt")
    args = parser.parse_args(argv)

    rendered = emit(build_vectors())
    if args.check:
        if not args.out.exists():
            print(f"missing: {args.out}", file=sys.stderr)
            return 1
        if args.out.read_text(encoding="utf-8") != rendered:
            print(f"DRIFT: {args.out} does not match generator output. Rerun without --check.",
                  file=sys.stderr)
            return 1
        print(f"OK: {args.out} matches generator output.")
        return 0

    args.out.write_text(rendered, encoding="utf-8")
    print(f"wrote {len(rendered)} bytes to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
