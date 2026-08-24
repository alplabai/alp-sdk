# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_slot_claim_atomic.py.

The gate exists because this bug class survived TWO closed issues (#1115 and
#629): both were remediated from a hand-written site list rather than a grep,
so a tenth site could be written the day after each closed. A gate that only
ever runs green on the real tree proves nothing about whether it would catch
the eleventh -- every test here feeds it a corpus that SHOULD fire and asserts
that it does.

Run locally:

    python3 -m pytest tests/scripts/test_check_slot_claim_atomic.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from check_slot_claim_atomic import find_problems  # noqa: E402

# The converted shape: the compare-exchange IS the claim, so nothing ever
# assigns the flag true.
CLEAN = """\
static thing_t *_alloc(void)
{
\tfor (size_t i = 0; i < ARRAY_SIZE(_pool); ++i) {
\t\tif (alp_slot_try_claim(&_in_use[i])) {
\t\t\tmemset(&_pool[i], 0, sizeof(_pool[i]));
\t\t\treturn &_pool[i];
\t\t}
\t}
\treturn NULL;
}
"""

# Shape A: a parallel bool[] scanned in a loop.  This is what all eight
# array-shaped sites in #1630 looked like.
DIRTY_ARRAY = """\
static thing_t *_alloc(void)
{
\tfor (size_t i = 0; i < ARRAY_SIZE(_pool); ++i) {
\t\tif (!_in_use[i]) {
\t\t\t_in_use[i] = true;
\t\t\treturn &_pool[i];
\t\t}
\t}
\treturn NULL;
}
"""

# The singleton shape: a bare scalar, no loop, no subscript, and the test half
# is POSITIVE (`if (taken) fail`) rather than negated.  Every array-shaped grep
# in #1115's remediation walked straight past this, which is how the TFLM
# default arena survived.  The gate must catch it too.
DIRTY_SCALAR = """\
static int open_interpreter(void)
{
\tif (g_default_arena_in_use) {
\t\treturn ALP_ERR_NOMEM;
\t}
\tg_default_arena_in_use = true;
\treturn ALP_OK;
}
"""

# Prose describing the antipattern is not the antipattern.  src/ is full of
# this: eleven comment lines quote `in_use = true` while explaining why the
# code below no longer does it.
COMMENTED = """\
/* issue #1115: this used to be a plain `if (!in_use) { in_use = true; }`
 * scan -- two threads could both win the same slot.  Now a CAS.
 */
static thing_t *_alloc(void)
{
\tif (alp_slot_try_claim(&_in_use[0])) {
\t\treturn &_pool[0];
\t}
\treturn NULL; /* pool full; in_use = true everywhere */
}
"""


def _seed(root: Path, relpath: str, body: str) -> None:
    p = root / relpath
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(body, newline="")


def test_atomic_claim_passes(tmp_path: Path) -> None:
    _seed(tmp_path, "src/backends/thing/zephyr_drv.c", CLEAN)
    assert find_problems(tmp_path) == []


def test_array_check_then_set_is_reported(tmp_path: Path) -> None:
    _seed(tmp_path, "src/backends/thing/zephyr_drv.c", DIRTY_ARRAY)
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "src/backends/thing/zephyr_drv.c:5" in problems[0]
    assert "alp_slot_try_claim" in problems[0], "the message must name the fix"


def test_scalar_singleton_is_reported(tmp_path: Path) -> None:
    """The shape an array-shaped grep cannot see -- see DIRTY_SCALAR."""
    _seed(tmp_path, "src/backends/inference/tflm.cpp", DIRTY_SCALAR)
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "src/backends/inference/tflm.cpp:6" in problems[0]


def test_comments_are_not_reported(tmp_path: Path) -> None:
    _seed(tmp_path, "src/backends/thing/zephyr_drv.c", COMMENTED)
    assert find_problems(tmp_path) == []


def test_allowlisted_site_is_not_reported(tmp_path: Path) -> None:
    _seed(tmp_path, "src/zephyr/handles.c", DIRTY_ARRAY)
    assert find_problems(tmp_path) == []


def test_non_src_tree_is_not_scanned(tmp_path: Path) -> None:
    """The gate owns src/.  A test fixture or an example is not a handle pool."""
    _seed(tmp_path, "tests/unit/thing/src/main.c", DIRTY_ARRAY)
    assert find_problems(tmp_path) == []


def test_real_tree_is_clean() -> None:
    """The gate must be green on the repo it ships in.

    If this fails, a site was missed by the sweep -- fix the site, do not widen
    the allowlist.
    """
    assert find_problems(REPO) == []
