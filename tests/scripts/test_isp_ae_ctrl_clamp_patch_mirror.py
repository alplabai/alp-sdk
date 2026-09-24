# SPDX-License-Identifier: Apache-2.0
"""`isp_clamp_ctrl_val()` must stay byte-for-byte (whitespace aside) in sync
between the vendored patch and its host-buildable test mirror (#2271).

`zephyr/patches/hal_alif/0010-isp-clamp-ae-writeback-to-ctrl-range.patch`
adds `isp_clamp_ctrl_val()` to hal_alif's vendored, non-host-buildable
`drivers/isp/isp_wrapper/src/isp_api_wrapper.c`. Because that file cannot be
built on the host, `tests/unit/isp_ae_ctrl_clamp/src/test_isp_ae_ctrl_clamp.c`
carries a hand-copied mirror of the same function and tests the copy instead
-- there is no `#include` path from the test to the vendored patch that would
keep the two mechanically identical (and inverting that dependency would put
alp-sdk-owned test code including a vendor patch, the wrong direction).

That makes the mirror driftable by construction: an edit to either copy that
is not mirrored to the other leaves the test proving something the shipped
patch no longer does, green the whole time. This test is the mechanical
backstop the file comments both point at -- it extracts the function body
from each source and fails if they differ (whitespace-normalised, so
reindentation alone does not trip it; anything else does).
"""

from __future__ import annotations

import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[2]
PATCH = (
    REPO
    / "zephyr"
    / "patches"
    / "hal_alif"
    / "0010-isp-clamp-ae-writeback-to-ctrl-range.patch"
)
MIRROR = (
    REPO
    / "tests"
    / "unit"
    / "isp_ae_ctrl_clamp"
    / "src"
    / "test_isp_ae_ctrl_clamp.c"
)

FUNC_SIGNATURE = re.compile(r"^static inline int32_t isp_clamp_ctrl_val\(")


def _extract_function_body(lines: list[str]) -> str:
    """Return `isp_clamp_ctrl_val()`'s signature-through-closing-brace text.

    Walks brace depth rather than counting a fixed line count, so the
    extraction survives either copy growing or shrinking a line as long as
    the function itself does not change shape.
    """
    start = next(i for i, line in enumerate(lines) if FUNC_SIGNATURE.match(line))

    depth = 0
    opened = False
    end = None
    for i in range(start, len(lines)):
        depth += lines[i].count("{") - lines[i].count("}")
        if "{" in lines[i]:
            opened = True
        if opened and depth == 0:
            end = i
            break
    assert end is not None, "unbalanced braces while extracting isp_clamp_ctrl_val()"

    return "\n".join(lines[start : end + 1])


def _normalise(text: str) -> str:
    """Collapse whitespace so indentation-only drift does not fail the test."""
    return "\n".join(" ".join(line.split()) for line in text.splitlines() if line.strip())


def _patch_added_lines() -> list[str]:
    """The '+' lines of the patch, with the leading '+' stripped.

    Skips the `+++ b/...` file-header line, which also starts with '+'.
    """
    lines = []
    for raw in PATCH.read_text(encoding="utf-8").splitlines():
        if raw.startswith("+++ "):
            continue
        if raw.startswith("+"):
            lines.append(raw[1:])
    return lines


def test_patch_and_test_mirror_define_the_same_clamp_function() -> None:
    patch_body = _extract_function_body(_patch_added_lines())
    mirror_body = _extract_function_body(MIRROR.read_text(encoding="utf-8").splitlines())

    assert _normalise(patch_body) == _normalise(mirror_body), (
        "isp_clamp_ctrl_val() has drifted between the vendored patch "
        f"({PATCH.relative_to(REPO)}) and its host-buildable test mirror "
        f"({MIRROR.relative_to(REPO)}).\n"
        "The patch is applied to hal_alif, a vendored module that is not "
        "host-buildable, so the mirror in the test file is what actually "
        "runs on native_sim -- if the two diverge, the test is pinning "
        "behaviour the shipped patch no longer has. Update BOTH copies "
        "together.\n\n"
        f"--- patch body ---\n{patch_body}\n\n"
        f"--- mirror body ---\n{mirror_body}"
    )
