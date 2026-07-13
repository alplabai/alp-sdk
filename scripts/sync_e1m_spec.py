#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Refresh the vendored e1m-spec pinout snapshot.

The E1M physical footprint (312-pad geometry, default function per pad,
mechanical envelope) is the E1M standard, owned by the external
`alplabai/e1m-spec` repo.  alp-sdk vendors a **verbatim** snapshot of the
machine-readable pinout (`pinout/v1.json`, `pinout/x-v1.json`) plus the
upstream `loom-v1` schema, so that:

  * contributors editing the per-SoM pinout TSVs
    (metadata/e1m_modules/<family>/*.tsv) have the authoritative pad
    numbers in-repo -- no cross-repo hunt; and
  * CI (scripts/check_e1m_pinout.py) can cross-check every TSV-claimed
    E1M pad/function against the standard **offline**.

This script is the *only* writer of the snapshot -- the snapshot is never
hand-edited.  It is a manual, network-using dev tool (NOT a CI gate); the
gate that validates the committed snapshot is offline.

Usage:

    python3 scripts/sync_e1m_spec.py                 # bump to e1m-spec@main
    python3 scripts/sync_e1m_spec.py --ref v1.0      # pin a tag
    python3 scripts/sync_e1m_spec.py --ref <sha>     # pin a commit

Requires the GitHub CLI (`gh`) authenticated for read access to
alplabai/e1m-spec.  Rewrites:

    metadata/e1m/pinout-v1.json
    metadata/e1m/pinout-x-v1.json
    metadata/schemas/loom-v1.schema.json
    metadata/e1m/e1m-spec.lock

Exit codes: 0 -- snapshot refreshed; 1 -- fetch/verify error.
"""

from __future__ import annotations

import argparse
import base64
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SPEC_REPO = "alplabai/e1m-spec"

# upstream path -> vendored snapshot path (repo-relative)
FILES = {
    "pinout/v1.json": "metadata/e1m/pinout-v1.json",
    "pinout/x-v1.json": "metadata/e1m/pinout-x-v1.json",
    "pinout/schema/loom-v1.schema.json": "metadata/schemas/loom-v1.schema.json",
}

LOCK_PATH = REPO / "metadata" / "e1m" / "e1m-spec.lock"

LOCK_TEMPLATE = """\
# e1m-spec vendored-snapshot lock -- DO NOT EDIT BY HAND.
#
# The E1M physical footprint (pad geometry + default function assignment +
# mechanical envelope) is the E1M standard, owned by the external
# `alplabai/e1m-spec` repo.  alp-sdk vendors a *verbatim* snapshot of the
# machine-readable pinout under metadata/e1m/ so contributors editing the
# per-SoM pinout TSVs have the authoritative pad numbers in-repo, and so
# CI can cross-check them offline (scripts/check_e1m_pinout.py).  The
# snapshot is never hand-edited -- it is regenerated:
#
#     python3 scripts/sync_e1m_spec.py            # bump to e1m-spec@main
#     python3 scripts/sync_e1m_spec.py --ref <sha|tag>
#
# See docs/adr/0019-vendor-e1m-spec-pinout-snapshot.md for the rationale
# and docs/e1m-pinout.md for how each layer consumes the pinout.
source:
  repo: {repo}
  ref: {ref}
  sha: {sha}
files:
  - snapshot: metadata/e1m/pinout-v1.json
    upstream: pinout/v1.json
    id: {v1_id}
    version: "{v1_version}"
    pad_count: {v1_pads}
  - snapshot: metadata/e1m/pinout-x-v1.json
    upstream: pinout/x-v1.json
    id: {xv1_id}
    version: "{xv1_version}"
    pad_count: {xv1_pads}
  - snapshot: metadata/schemas/loom-v1.schema.json
    upstream: pinout/schema/loom-v1.schema.json
"""


def _gh(*args: str) -> bytes:
    """Run `gh <args>` and return raw stdout, or die with a clear error."""
    try:
        out = subprocess.run(
            ("gh", *args),
            check=True,
            capture_output=True,
        )
    except FileNotFoundError:
        sys.exit("error: GitHub CLI (`gh`) not found -- install it to sync the snapshot.")
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode(errors="replace").strip()
        sys.exit(f"error: `gh {' '.join(args)}` failed:\n{stderr}")
    return out.stdout


def _resolve_sha(ref: str) -> str:
    return _gh("api", f"repos/{SPEC_REPO}/commits/{ref}", "--jq", ".sha").decode().strip()


def _fetch(path: str, sha: str) -> bytes:
    """Fetch an upstream file verbatim, pinned at the resolved SHA."""
    b64 = _gh("api", f"repos/{SPEC_REPO}/contents/{path}?ref={sha}", "--jq", ".content")
    return base64.b64decode(b64)


def main() -> int:
    ap = argparse.ArgumentParser(description="Refresh the vendored e1m-spec pinout snapshot.")
    ap.add_argument("--ref", default="main", help="e1m-spec ref to pin (branch, tag, or SHA). Default: main.")
    args = ap.parse_args()

    sha = _resolve_sha(args.ref)
    print(f"e1m-spec {SPEC_REPO}@{args.ref} -> {sha}")

    blobs: dict[str, bytes] = {}
    for upstream, snapshot in FILES.items():
        data = _fetch(upstream, sha)
        dest = REPO / snapshot
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        blobs[upstream] = data
        print(f"  {upstream} -> {snapshot} ({len(data)} bytes)")

    v1 = json.loads(blobs["pinout/v1.json"])
    xv1 = json.loads(blobs["pinout/x-v1.json"])

    LOCK_PATH.write_text(
        LOCK_TEMPLATE.format(
            repo=SPEC_REPO,
            ref=args.ref,
            sha=sha,
            v1_id=v1["id"],
            v1_version=v1["version"],
            v1_pads=len(v1["pads"]),
            xv1_id=xv1["id"],
            xv1_version=xv1["version"],
            xv1_pads=len(xv1["pads"]),
        )
    )
    print(f"  lock -> {LOCK_PATH.relative_to(REPO)}")
    print("done. Run `python3 scripts/check_e1m_pinout.py` to verify.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
