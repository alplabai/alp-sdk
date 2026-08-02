# SPDX-License-Identifier: Apache-2.0
"""Every conformance class must appear in docs/testing.md's coverage table (#1098).

This table has now drifted twice in the same direction: #937 (the class
COUNT was stale) and #1098 (the per-header TABLE was missing `<alp/i3c.h>`,
and -- found while fixing it -- `<alp/dac.h>` too). Both times a class was
enrolled in `tests/zephyr/conformance/src/main.c` and the docs quietly did
not follow, so a reader checking whether a class had portable-API coverage
was told it did not exist.

Adding one more row does not stop a third occurrence; this does. The next
`.name = "..."` added to the conformance array fails here until the table
grows a matching entry.

Representation is deliberately broader than "has its own `<alp/X.h>` row",
because four classes legitimately do not:

* `gpio`/`i2c`/`spi`/`uart` live under `<alp/peripheral.h>` sub-rows -- they
  have no per-class header to name.
* `qenc` is folded into the `<alp/counter.h>` row (same header, and its
  registry test is cited there).
* `i2c_target`/`spi_target` are named in the portable-class lifecycle row
  rather than getting rows of their own.

Encoding those four shapes here is the point: a future class that matches
none of them has genuinely been forgotten, which is exactly the #937/#1098
failure.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONFORMANCE_MAIN = REPO / "tests" / "zephyr" / "conformance" / "src" / "main.c"
TESTING_DOC = REPO / "docs" / "testing.md"

# Classes carried by an `<alp/peripheral.h>` sub-row -> the label that row uses.
_PERIPHERAL_SUBROWS = {
    "gpio": "— GPIO",
    "i2c": "— I²C",
    "spi": "— SPI",
    "uart": "— UART",
}
# Classes deliberately folded into another header's row -> the token that proves it.
_FOLDED_INTO_ANOTHER_ROW = {
    "qenc": "qenc_registry",
    "i2c_target": "I²C/SPI target modes",
    "spi_target": "I²C/SPI target modes",
}


def _conformance_class_names() -> list[str]:
    """The `.name = "..."` of every entry in the conformance class array."""
    # encoding="utf-8" is mandatory, not decoration: Python defaults to the
    # locale encoding, which is cp1252 on the Windows CI leg, and both files
    # read here carry non-ASCII (docs/testing.md has `I²C` and `✅`). Without
    # it this raises UnicodeDecodeError on Windows only -- caught by the
    # python-smoke (windows-latest) leg that #1023/#1032 made able to fail.
    return re.findall(
        r'\.name\s*=\s*"([^"]+)"', CONFORMANCE_MAIN.read_text(encoding="utf-8")
    )


def test_conformance_array_is_readable():
    """Guards the regex itself -- a silently-empty list would pass everything below."""
    names = _conformance_class_names()
    assert len(names) >= 14, (
        f"only {len(names)} conformance class name(s) parsed out of "
        f"{CONFORMANCE_MAIN.relative_to(REPO)} -- the `.name = \"...\"` shape "
        f"this test greps for has probably changed, so the coverage assertion "
        f"below is no longer checking anything"
    )
    assert "i3c" in names, "i3c dropped out of the conformance suite (#937/#1098)"


def test_every_conformance_class_is_represented_in_the_coverage_table():
    """Fails when a class is enrolled in conformance but absent from docs/testing.md."""
    doc = TESTING_DOC.read_text(encoding="utf-8")
    missing = []
    for name in _conformance_class_names():
        if f"<alp/{name}.h>" in doc:
            continue
        if name in _PERIPHERAL_SUBROWS and _PERIPHERAL_SUBROWS[name] in doc:
            continue
        if name in _FOLDED_INTO_ANOTHER_ROW and _FOLDED_INTO_ANOTHER_ROW[name] in doc:
            continue
        missing.append(name)

    assert not missing, (
        f"conformance class(es) {missing} have no representation in "
        f"{TESTING_DOC.relative_to(REPO)}'s per-header coverage table. Add an "
        f"`<alp/<class>.h>` row stating the coverage that ACTUALLY exists (do "
        f"not copy a sibling row's shape -- several classes have no "
        f"`tests/unit/<class>_registry/`), or, if the class is deliberately "
        f"carried by another row, record that here in _PERIPHERAL_SUBROWS / "
        f"_FOLDED_INTO_ANOTHER_ROW so the exemption is explicit rather than "
        f"silent."
    )
