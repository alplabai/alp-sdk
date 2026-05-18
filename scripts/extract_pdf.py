#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Datasheet text extractor for the chip-metadata pipeline.

Writes one page per delimited block so downstream grep/awk can target
sections by page number.
"""
import argparse
import sys
from pypdf import PdfReader


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Input PDF file path")
    parser.add_argument("output", help="Output text file path")
    args = parser.parse_args(argv)

    reader = PdfReader(args.input)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(f"=== {args.input} ({len(reader.pages)} pages) ===\n")
        for idx, page in enumerate(reader.pages, start=1):
            f.write(f"\n========== PAGE {idx} ==========\n")
            try:
                f.write(page.extract_text() or "")
            except Exception as e:
                f.write(f"[extract failure: {e}]\n")

    print(f"wrote {args.output}: {len(reader.pages)} pages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
