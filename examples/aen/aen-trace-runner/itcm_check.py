#!/usr/bin/env python3
"""itcm_check.py NM ELF LIMIT -- fail the build when the HE image outgrows its ITCM region.

The HE image is SE-loaded whole into ITCM (build-release.sh: <= 256 KiB). _flash_used is the image's
byte count (== zephyr.bin's size), so a logo or any other asset that does not fit stops the build
here, not only at packaging.
"""
import re
import subprocess
import sys

nm, elf, limit = sys.argv[1], sys.argv[2], int(sys.argv[3])
out = subprocess.run([nm, elf], capture_output=True, encoding="utf-8", check=True).stdout
m = re.search(r"^([0-9a-fA-F]+) A _flash_used$", out, re.M)
if not m:
    sys.exit("itcm_check: no _flash_used in %s" % elf)
used = int(m.group(1), 16)
if used > limit:
    sys.exit("the HE image is %d B, over the %d B ITCM region it is loaded into" % (used, limit))
print("HE image %d B of %d B ITCM" % (used, limit))
