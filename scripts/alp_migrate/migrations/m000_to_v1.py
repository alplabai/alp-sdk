# SPDX-License-Identifier: Apache-2.0
"""Migration #001: unversioned board.yaml -> schemaVersion 1 (epic #610 WS6-b).

The adoption step. Stamps an explicit `schemaVersion: 1` at the top of the
mapping. Pure structural transform, no C surface, so it reports no
needs-manual items.
"""
from __future__ import annotations

FROM: int | None = None
TO: int = 1


def transform(doc, report) -> None:
    """Insert `schemaVersion: 1` as the first key, comment-preserving.

    `doc` is a ruamel CommentedMap; `.insert(pos, key, value)` places the key
    without disturbing existing keys' comments/order. ruamel attaches a
    leading (pre-document) comment to the *map itself* (`doc.ca.comment`),
    not to whichever key happens to render first -- so a plain `.insert(0,
    ...)` would otherwise leave a top-of-file comment sitting above the new
    `schemaVersion` line. Re-home that leading comment onto the original
    first key before inserting, so it renders in its original place (now
    just below `schemaVersion`) instead of above it.
    """
    lead = doc.ca.comment
    if lead and lead[1] and len(doc):
        first_key = next(iter(doc))
        doc.ca.comment = None
        existing = doc.ca.items.get(first_key)
        if existing:
            existing[1] = list(lead[1]) + (existing[1] or [])
        else:
            doc.ca.items[first_key] = [None, list(lead[1]), None, None]
    doc.insert(0, "schemaVersion", TO)
    report.steps.append("m000_to_v1: stamped schemaVersion: 1")
