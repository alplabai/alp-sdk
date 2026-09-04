# SPDX-License-Identifier: Apache-2.0
"""
Pins the confidential-vendor-filename leak class fixed for the Alif E4/E6/E8
Hardware Reference Manual `hwrm.source` fields (see metadata/socs/alif/ensemble/
e4.json, e6.json, e8.json): an NDA-bound vendor PDF's own filename (e.g.
"Confidential-Alif_E4_HWRM_v0.3.pdf") is itself confidential -- publishing it
in this public repo discloses both the classification and the exact document
identity, even though the PDF bytes never land in git. `check_public_private.py`
has no rule for this class today (`grep -ni confidential
scripts/check_public_private.py` is empty, and the gate returned rc=0 both
before and after the E8 fix) -- extending that gate is a separate, shared
concern tracked in #1540 (in review elsewhere) and is out of scope here; this
is a narrower pin that at least stops a *regression* on the surfaces most
likely to reintroduce the leak.

The walk, deliberately chosen:
  - `metadata/**` -- where all three real hits lived (the `hwrm.source` /
    `datasheet.source` fields this class actually leaked from), so this is
    the load-bearing root.
  - `docs/**` -- a prior sweep found a hit under docs/superpowers/specs/;
    prose citing a plan/spec can paste a real filename as easily as a JSON
    field can.
  - `vendors/**` -- vendored third-party sources sometimes carry upstream
    README/LICENSE/notes text that could name a bundled confidential vendor
    doc.

Deliberately NOT walked:
  - `tests/scripts/` -- this file's own docstring above and the mutation
    fixture used to prove this test (a synthetic "Confidential-Alif_E5_HWRM_
    v9.9.pdf"-shaped string injected into a real file, then reverted) would
    trip over a self-referential scan, exactly as already documented in
    `test_soc_npu_pairing.py`'s HG-subsystem / A32-reach pins.
  - `doxygen-out/` -- a generated (and, independently of this change,
    oddly-tracked) HTML mirror of `docs/**` prose. It has no authored
    content of its own; whatever text lands there is downstream of
    `docs/**`, which this test already covers at the source.

No file in the three walked roots legitimately quotes a `Confidential-
<name>.pdf`-shaped string today: the only prose that names the
"Confidential-" classification convention by name
(`docs/superpowers/specs/2026-06-26-aen401-xhci-usb-host-design.md:62`,
"the E4 HWRM is `Confidential-`.") stops short of a full filename, so the
regex below needs no exemption list. If a future doc ever needs to explain
the naming convention with a real worked example, that file would need an
explicit skip added here -- do not loosen the regex to dodge it.

Run locally:

    python -m pytest tests/scripts/test_no_confidential_vendor_filenames.py -v
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import validate_metadata as V  # noqa: E402

# A vendor PDF's own confidential-marked filename, e.g.
# "Confidential-Alif_E4_HWRM_v0.3.pdf". Anchored on the literal "Confidential-"
# prefix immediately followed by a contiguous filename-shaped run ending in
# ".pdf", so ordinary prose about the naming convention ("the Confidential-
# source rule") cannot match -- there, "source" is followed by a space, not
# ".pdf", so no contiguous run from the prefix ever reaches the suffix.
_CONFIDENTIAL_VENDOR_FILENAME_RE = re.compile(
    r"Confidential-[A-Za-z0-9_.-]+\.pdf", re.IGNORECASE
)

_WALK_GLOBS = ("metadata/**/*", "docs/**/*", "vendors/**/*")


def _scan() -> list[str]:
    hits: list[str] = []
    for pattern in _WALK_GLOBS:
        for p in sorted(V.REPO.glob(pattern)):
            if not p.is_file():
                continue
            try:
                text = p.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for m in _CONFIDENTIAL_VENDOR_FILENAME_RE.finditer(text):
                hits.append(f"{p.relative_to(V.REPO)}: {m.group(0)!r}")
    return hits


def test_no_shipped_metadata_docs_or_vendors_file_names_a_confidential_vendor_pdf():
    """A vendor document's own confidential-marked filename must never
    appear verbatim under metadata/**, docs/**, or vendors/** -- that is
    exactly the class removed from the Alif E4/E6/E8 `hwrm.source` fields,
    which this repo's public/private gate (check_public_private.py) has no
    rule for."""
    hits = _scan()
    assert hits == [], (
        "shipped metadata/docs/vendors text names a confidential vendor "
        "PDF's own filename -- replace it with a non-filename document "
        "identifier (title / doc id / revision), per the pattern used for "
        "the Alif E4/E6/E8 HWRM `hwrm.source` fields: " + "; ".join(hits)
    )
