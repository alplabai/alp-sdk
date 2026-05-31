#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
firmware/cc3501e/flash.py -- CC3501E firmware-image flasher.

STUB.  Real implementation lands alongside the first prebuilt
binary from `alplabai/cc3501e-firmware`.  The CLI shape below is
the consumer-facing contract that the real implementation must
honour.

Flow (planned):
  1. Connect to the Alif main MCU via the standard debug probe
     (west flash chooses the right tool per Zephyr backend;
     OpenOCD for bare-metal).
  2. Have the Alif's bootloader-helper drive the CC3501E into
     bootloader entry mode (assert E_WIFI.NRST, raise WIFI_EN,
     pull GPIO strap pins to "external load" pattern).
  3. Stream the firmware image to the CC3501E over the inter-chip
     SPI1 using the TI bootloader protocol.  Verify the signature
     against the OTP-burned public key (the CC3501E does this
     itself before committing).
  4. Release reset and run sanity check via
     `chips/cc3501e/cc3501e.c`'s `cc3501e_get_version()`.

See `firmware/cc3501e/README.md` for the full bootstrap and
release process.
"""

import argparse
import hashlib
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Flash a signed firmware image to the on-module CC3501E.",
        epilog="See firmware/cc3501e/README.md for the consumer-facing flow.",
    )
    p.add_argument(
        "--binary",
        type=Path,
        required=True,
        help="Path to the signed firmware image (e.g. prebuilt/cc3501e-v0.1.0.bin).",
    )
    p.add_argument(
        "--signature",
        type=Path,
        help="Detached signature file.  Defaults to <binary>.sig.",
    )
    p.add_argument(
        "--sha256",
        type=Path,
        help="Expected SHA-256 file.  Defaults to <binary>.sha256.",
    )
    p.add_argument(
        "--verify",
        action="store_true",
        help="After flashing, read back the firmware version and confirm it matches the image's manifest.",
    )
    p.add_argument(
        "--probe",
        default="auto",
        help='Debug probe selector ("west", "openocd", or a device path).  "auto" picks based on the surrounding alp-sdk build.',
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="Verify the image locally (SHA-256 + signature presence) but do not actually flash.",
    )
    return p.parse_args()


def check_sha256(binary: Path, expected_file: Path | None) -> None:
    if expected_file is None:
        expected_file = binary.with_suffix(binary.suffix + ".sha256")
    if not expected_file.exists():
        print(f"warning: no SHA-256 file at {expected_file}; skipping integrity check",
              file=sys.stderr)
        return
    expected = expected_file.read_text(encoding="utf-8").strip().split()[0]
    actual = hashlib.sha256(binary.read_bytes()).hexdigest()
    if actual != expected:
        sys.exit(f"error: SHA-256 mismatch\n  expected {expected}\n  actual   {actual}")
    print(f"SHA-256 OK ({actual[:16]}...)")


def check_signature_present(binary: Path, sig_file: Path | None) -> None:
    if sig_file is None:
        sig_file = binary.with_suffix(binary.suffix + ".sig")
    if not sig_file.exists():
        sys.exit(
            f"error: no detached signature at {sig_file}\n"
            "       Signed firmware is required.  See firmware/cc3501e/README.md \"Release process\"."
        )
    # The CC3501E itself verifies the signature against its OTP-burned
    # public key during flash.  We only check that the file is present
    # so the user gets a clean error before driving the probe.
    print(f"signature present ({sig_file.name}, {sig_file.stat().st_size} bytes)")


def main() -> int:
    args = parse_args()
    if not args.binary.exists():
        sys.exit(f"error: binary not found: {args.binary}")
    check_sha256(args.binary, args.sha256)
    check_signature_present(args.binary, args.signature)

    if args.dry_run:
        print("dry-run: image looks valid; not flashing.")
        return 0

    sys.exit(
        "error: real flashing not yet implemented.\n"
        "       The CC3501E firmware repo (alplabai/cc3501e-firmware) and the\n"
        "       first prebuilt binary have not landed yet.  Once they do, this\n"
        "       script will drive the inter-chip SPI1 bootloader sequence.\n"
        "       Track progress in firmware/cc3501e/README.md \"Status\" table."
    )


if __name__ == "__main__":
    sys.exit(main())
