#!/usr/bin/env python3
"""Fail if the README's *current-state* version label drifts from the
declared SDK version.

The release flow (`scripts/bump_version.py`, the `cutting-a-release` skill)
updates `metadata/sdk_version.yaml`, the CHANGELOG, and the ABI snapshot --
but NOT the README's hand-written prose version labels.  So the README sat at
"Mostly pre-silicon (`v0.6`)" / "v0.6 ramp" through the entire v0.7.0 release.
This check makes that class of drift a CI failure instead of a silent miss.

It deliberately checks ONLY the small set of *current-state* labels (anchored
by regex), not every `v0.x` token -- the README's historical references
("the silicon-verified slice landed in v0.6", "from v0.6 onward") are correct
and must stay.  Add a new anchor here when a new current-state label appears.

Authoritative version: `metadata/sdk_version.yaml` (`version: MAJOR.MINOR.PATCH`).
The labels track the MAJOR.MINOR (e.g. `v0.7`); the patch is not in prose.

Exit 0 = in sync, 1 = drift (with the offending lines), 2 = setup error.
"""
from __future__ import annotations

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"
README = REPO / "README.md"

# (description, compiled regex with ONE capture group = the vMAJOR.MINOR token).
# Each anchor matches a current-state label whose version MUST equal the
# declared MAJOR.MINOR.  Keep these tightly anchored so historical refs
# ("landed in v0.6") are never matched.
_ANCHORS: list[tuple[str, re.Pattern[str]]] = [
    ("intro badge — 'Mostly pre-silicon (`vX.Y`)'",
     re.compile(r"Mostly pre-silicon \(`v(\d+\.\d+)`\)")),
    ("Status heading — '**vX.Y ramp — paper-correct'",
     re.compile(r"\*\*v(\d+\.\d+) ramp — paper-correct")),
]


def declared_minor() -> str:
    """Return the MAJOR.MINOR string from sdk_version.yaml (e.g. '0.7')."""
    text = SDK_VERSION_YAML.read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$", text, re.MULTILINE)
    if not m:
        print(f"check_version_doc_sync: could not parse 'version:' from "
              f"{SDK_VERSION_YAML.relative_to(REPO)}", file=sys.stderr)
        sys.exit(2)
    return f"{m.group(1)}.{m.group(2)}"


def main() -> int:
    want = declared_minor()
    readme = README.read_text(encoding="utf-8")
    drifts: list[str] = []
    for desc, rx in _ANCHORS:
        found = rx.search(readme)
        if found is None:
            drifts.append(f"  MISSING  {desc}: anchor not found in README.md "
                          f"(was it reworded? update scripts/check_version_doc_sync.py)")
            continue
        got = found.group(1)
        if got != want:
            drifts.append(f"  STALE    {desc}: README says v{got}, "
                          f"sdk_version.yaml declares v{want}")

    if drifts:
        print(f"README current-state version labels out of sync with "
              f"metadata/sdk_version.yaml (v{want}):", file=sys.stderr)
        print("\n".join(drifts), file=sys.stderr)
        print("\nThe release/version bump must update the README prose too "
              "(historical 'landed in vX' refs stay). -- failing.", file=sys.stderr)
        return 1

    print(f"check_version_doc_sync: OK (README current-state labels match v{want}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
