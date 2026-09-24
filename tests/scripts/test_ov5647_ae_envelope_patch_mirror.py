# SPDX-License-Identifier: Apache-2.0
"""The fps-parameterized AE ceiling macros must stay byte-for-byte
(whitespace aside) in sync between the vendored patch and their
host-buildable test mirror (#2277).

`zephyr/patches/hal_alif/0011-isp-ov5647-ae-calib-envelope.patch` adds
`ov5647_ae_envelope.h` to hal_alif's vendored, non-host-buildable
`drivers/isp/isp_wrapper/inc/`. Because that header (and its `vsios_type.h`
dependency) is not part of this build,
`tests/unit/ov5647_ae_envelope/src/test_ov5647_ae_envelope.c` carries a
hand-copied mirror of its `OV5647_AE_PIXEL_RATE_HZ` /
`OV5647_AE_HTS_640X480` / `OV5647_AE_VTS_AT_FPS()` /
`OV5647_AE_MAX_INT_LINE_AT_FPS()` macros and tests the copy instead -- there
is no `#include` path from the test to the vendored patch that would keep
the two mechanically identical (and inverting that dependency would put
alp-sdk-owned test code including a vendor patch, the wrong direction).

That makes the mirror driftable by construction: an edit to either copy
that is not mirrored to the other leaves the test proving something the
shipped patch no longer does, green the whole time. This test is the
mechanical backstop the file comments both point at -- it extracts the
macro block from each source and fails if they differ (whitespace-
normalised, so reindentation alone does not trip it; anything else does).
Mirrors tests/scripts/test_isp_ae_ctrl_clamp_patch_mirror.py's shape
(#2271) for a macro block instead of a function body.
"""

from __future__ import annotations

import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]
PATCH = REPO / "zephyr" / "patches" / "hal_alif" / "0011-isp-ov5647-ae-calib-envelope.patch"
MIRROR = REPO / "tests" / "unit" / "ov5647_ae_envelope" / "src" / "test_ov5647_ae_envelope.c"

START_PATTERN = re.compile(r"^#define OV5647_AE_PIXEL_RATE_HZ\b")
END_PATTERN = re.compile(r"^#define OV5647_AE_MAX_INT_LINE_AT_FPS\(fps\)")


def _extract_macro_block(lines: list[str]) -> str:
    """Return the OV5647_AE_PIXEL_RATE_HZ..OV5647_AE_MAX_INT_LINE_AT_FPS()
    macro block, start line through end line (inclusive).

    Line-range extraction, not brace-matching: these are #define lines, not
    a function body.
    """
    start = next(i for i, line in enumerate(lines) if START_PATTERN.match(line))
    end = next(i for i in range(start, len(lines)) if END_PATTERN.match(lines[i]))

    return "\n".join(lines[start : end + 1])


_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)


def _normalise(text: str) -> str:
    """Strip `/* ... */` comments (the patch's explanatory prose between
    macros vs. the mirror's terser trailing notes are not required to
    match word-for-word, only the CODE is) and collapse whitespace so
    indentation/alignment-only drift does not fail the test either."""
    text = _BLOCK_COMMENT.sub("", text)
    return "\n".join(" ".join(line.split()) for line in text.splitlines() if line.strip())


def _patch_added_lines() -> list[str]:
    """The '+' lines of the patch, with the leading '+' stripped.

    Skips the `+++ b/...` file-header lines, which also start with '+'.
    """
    lines = []
    for raw in PATCH.read_text(encoding="utf-8").splitlines():
        if raw.startswith("+++ "):
            continue
        if raw.startswith("+"):
            lines.append(raw[1:])
    return lines


def test_patch_and_test_mirror_define_the_same_envelope_macros() -> None:
    patch_body = _extract_macro_block(_patch_added_lines())
    mirror_body = _extract_macro_block(MIRROR.read_text(encoding="utf-8").splitlines())

    assert _normalise(patch_body) == _normalise(mirror_body), (
        "OV5647 AE envelope macros have drifted between the vendored patch "
        f"({PATCH.relative_to(REPO)}) and their host-buildable test mirror "
        f"({MIRROR.relative_to(REPO)}).\n"
        "The patch is applied to hal_alif, a vendored module that is not "
        "host-buildable, so the mirror in the test file is what actually "
        "runs on native_sim -- if the two diverge, the test is pinning "
        "behaviour the shipped patch no longer has. Update BOTH copies "
        "together.\n\n"
        f"--- patch body ---\n{patch_body}\n\n"
        f"--- mirror body ---\n{mirror_body}"
    )
