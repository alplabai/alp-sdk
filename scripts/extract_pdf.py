#!/usr/bin/env python3
"""
Datasheet text extractor for the chip-metadata pipeline.

Usage: extract_pdf.py <input.pdf> <output.txt>

Writes one page per delimited block so downstream grep/awk can target
sections by page number.
"""
import sys
from pypdf import PdfReader

if len(sys.argv) != 3:
    sys.exit(f"usage: {sys.argv[0]} <input.pdf> <output.txt>")

src, dst = sys.argv[1], sys.argv[2]
reader = PdfReader(src)

with open(dst, "w", encoding="utf-8") as f:
    f.write(f"=== {src} ({len(reader.pages)} pages) ===\n")
    for idx, page in enumerate(reader.pages, start=1):
        f.write(f"\n========== PAGE {idx} ==========\n")
        try:
            f.write(page.extract_text() or "")
        except Exception as e:
            f.write(f"[extract failure: {e}]\n")

print(f"wrote {dst}: {len(reader.pages)} pages")
