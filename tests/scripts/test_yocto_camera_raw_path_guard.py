# SPDX-License-Identifier: Apache-2.0
"""Guard: the Linux camera backend must never read the V4L2 mapping byte-wise.

The mmap'd V4L2 buffer is uncached on RZ/V2N, so a per-byte unpack straight
from it cost 346 ms/frame (1456x1088 RAW10) against 23.9 ms through the cached
bounce rows (`cam_cr10_unpack_mapped`).  A rebase once re-introduced the old
loop; this keeps it out.
"""

import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "src/backends/camera/yocto_drv.c"


def test_mapped_buffer_goes_through_the_bounce_row():
    src = SRC.read_text(encoding="utf-8")
    assert not re.search(r"\bcam_cr10_unpack\s*\(", src), (
        "byte-wise cam_cr10_unpack() is back; unpack the uncached mapping via "
        "cam_cr10_unpack_mapped() (cached bounce row)"
    )
    # The only consumer of the unpack row routine is the bounce wrapper: it
    # must be fed `scratch` (cached), never a mapped pointer.
    calls = [c for c in re.findall(r"cam_cr10_unpack_row\(\s*(\w+)", src) if c != "const"]  # not the definition
    assert calls and set(calls) == {"scratch"}, calls
    assert re.search(r"memcpy\(\s*scratch[^;]*,\s*src", src)
    # Capture hands the mapped buffer only to the _mapped variant.
    for m in re.finditer(r"cam_cr10_unpack\w*\(\s*([^,]+),", src):
        if "->map[" in m.group(1):
            assert m.group(0).startswith("cam_cr10_unpack_mapped"), m.group(0)
