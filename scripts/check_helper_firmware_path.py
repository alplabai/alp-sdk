#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every `helper_firmware[].firmware_path` in every
`metadata/e1m_modules/*.yaml` SoM preset resolves to a real, repo-relative
file on disk.

Issue #1372: `firmware_path` is a repo-relative path shipped in six AEN
presets (pointing at `firmware/cc3501e/prebuilt/cc3501e-v0.2.0.bin`) and
nothing anywhere validated that it resolves -- not the schema (`minLength: 1`
only, no `format`/existence constraint), not `scripts/alp_cli/validator.py`
(shape-only), not `scripts/alp_orchestrate/manifest.py` (projects the value
verbatim into `helper_mcus[]` with no `Path.exists()`), and not tan-cli's
`HelperMcu.firmware_path` (carried through but never dereferenced). A stale
path -- moved, renamed, or removed out from under a preset -- stayed
indistinguishable from a good one until someone tried to flash.

`firmware_path` is OPTIONAL and its omission is meaningful: the schema's own
description says the key should be left out entirely when no artefact exists
yet. No SoM preset declares `flash_method` today, so `tan flash` skips every
helper entry before ever inspecting this field; an entry that gains a
`flash_method` while `firmware_path` stays absent then gets `tan flash`'s
clean "has no output_artefact / firmware_path; can't flash" refusal instead
of resolving a sentinel/`TBD` path. This gate therefore only checks a value
that IS present -- it never requires the key.

Run locally:

    python3 scripts/check_helper_firmware_path.py

CI wires this in pr-metadata-validate.yml.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    presets_dir = root / "metadata" / "e1m_modules"
    if not presets_dir.is_dir():
        return problems

    for preset in sorted(presets_dir.glob("*.yaml")):
        with preset.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        for entry in doc.get("helper_firmware") or []:
            firmware_path = entry.get("firmware_path")
            if firmware_path is None:
                # Deliberately absent -- schema-legal, and the point of the
                # key: no artefact exists yet. No preset declares
                # `flash_method` today, so `tan flash` skips the entry before
                # ever reaching this field; only an entry that also gains a
                # `flash_method` would reach `tan flash`'s clean "has no
                # output_artefact / firmware_path" refusal. Not this gate's
                # job either way.
                continue
            name = entry.get("name", "<unnamed>")
            resolved = (root / firmware_path).resolve()
            # Path.is_file() (not .exists()) -- a firmware image is always a
            # regular file; a directory left behind by a bad rename should
            # fail the same way a missing path does, not slip through.
            if not resolved.is_file():
                problems.append(
                    f"{preset.name}: helper_firmware[{name!r}].firmware_path "
                    f"={firmware_path!r} does not resolve to a file "
                    f"(resolved {resolved}) -- fix the path, or omit the key "
                    f"entirely if no artefact exists yet"
                )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_helper_firmware_path: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every helper_firmware[].firmware_path resolves to a real file.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
