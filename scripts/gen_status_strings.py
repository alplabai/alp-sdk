#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Generate src/status_strings.c from the alp_status_t enum in
include/alp/peripheral.h.

alp_status_t is the single source of truth for every status code the SDK
returns; this generator projects it into two tiny runtime introspection
functions (mirrors alp_cap_name() / scripts/gen_soc_caps.py):

    const char *alp_status_name(alp_status_t status);
    const char *alp_status_description(alp_status_t status);

`alp_status_name` is always compiled (cheap, pure .rodata).
`alp_status_description` returns the doc-comment text (first sentence);
that table is gated behind CONFIG_ALP_STATUS_DESCRIPTIONS (default y) so a
footprint-sensitive build can drop it while keeping the name lookup.

The sentinel ALP_STATUS_ENUM_FLOOR is deliberately excluded from the
generated tables -- its own doc comment says it is "NOT a status -- never
returned by any API", and it aliases the same numeric value as the last
real error code (ALP_ERR_NOT_PROVISIONED), so including it would collide
in the designated-initialiser table.  alp_status_name()/description() both
still special-case its numeric value sanely (they see it as a valid
member of the ALP_OK..ALP_STATUS_ENUM_FLOOR range and return the
NOT_PROVISIONED strings for it -- the two are numerically identical, so
this is the only sane behaviour, not an omission).

Run:

    python3 scripts/gen_status_strings.py

CI (pr-generated-files.yml) regenerates the file on every PR that touches
include/alp/peripheral.h, then fails if the working tree diff is non-empty.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "include" / "alp" / "peripheral.h"
OUT = REPO / "src" / "status_strings.c"

SENTINEL = "ALP_STATUS_ENUM_FLOOR"

# Matches one enumerator: NAME = VALUE [,] [/**< doc */]
#
# The trailing doc-comment group is genuinely optional: it only consumes
# input when the next non-whitespace token is literally "/**<"; if the
# next token is instead the following enumerator's NAME, the optional
# group matches zero characters and finditer() picks it up as the next
# match.  This is what lets ALP_OK (no doc comment) parse cleanly.
_ENTRY_RE = re.compile(
    r"(?P<name>[A-Z][A-Z0-9_]*)\s*=\s*(?P<value>-?\d+)\s*,?\s*"
    r"(?:/\*\*<\s*(?P<doc>.*?)\s*\*/)?",
    re.DOTALL,
)

_WS_RE = re.compile(r"\s+")

# Sentence boundary: a period followed by whitespace and an uppercase
# letter.  Tolerates "e.g." / "i.e." mid-sentence -- those have no
# whitespace immediately after the inner period, so they never match.
_SENTENCE_RE = re.compile(r"(?<=\.)\s+(?=[A-Z])")


class Entry:
    def __init__(self, name: str, value: int, doc: str | None):
        self.name = name
        self.value = value
        self.doc = doc

    @property
    def description(self) -> str:
        """First sentence of the doc comment; "success" for ALP_OK."""
        if not self.doc:
            return "success" if self.name == "ALP_OK" else self.name
        text = _WS_RE.sub(" ", self.doc).strip()
        return _SENTENCE_RE.split(text, maxsplit=1)[0].strip()


def extract_entries(header_text: str) -> list[Entry]:
    m = re.search(
        r"/\*\*\s*Status codes returned by ALP peripheral functions\.\s*\*/"
        r"\s*typedef enum \{(?P<body>.*?)\}\s*alp_status_t;",
        header_text,
        re.DOTALL,
    )
    if not m:
        print(
            f"error: could not locate the alp_status_t enum in "
            f"{HEADER.relative_to(REPO)} -- has its declaration or leading "
            "doc comment moved?",
            file=sys.stderr,
        )
        sys.exit(1)

    entries: list[Entry] = []
    seen: set[str] = set()
    for em in _ENTRY_RE.finditer(m.group("body")):
        name = em.group("name")
        if name in seen:
            continue
        seen.add(name)
        entries.append(Entry(name, int(em.group("value")), em.group("doc")))
    return entries


def _c_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(entries: list[Entry]) -> str:
    # Drop the sentinel from the generated tables -- see module docstring.
    real = [e for e in entries if e.name != SENTINEL]
    floor = next(e for e in entries if e.name == SENTINEL)

    # Sanity: the real statuses must densely cover floor..0 with no gaps, and
    # ALP_STATUS_ENUM_FLOOR must equal the most-negative captured value.  A gap
    # means a member was dropped by the parser -- e.g. an auto-incremented
    # enumerator written without an explicit `= -N`, which _ENTRY_RE cannot
    # match -- and would otherwise SILENTLY return "ALP_STATUS_UNKNOWN" for a
    # real code.  Fail loudly so adding a status can never half-land.
    values = {e.value for e in real}
    missing = set(range(floor.value, 1)) - values
    if missing or floor.value != min(values):
        sys.exit(
            "gen_status_strings: alp_status_t is not dense/self-consistent "
            f"(missing values {sorted(missing)}; ALP_STATUS_ENUM_FLOOR={floor.value}, "
            f"min captured={min(values)}).  Give every enumerator an explicit "
            "'= -N' and keep ALP_STATUS_ENUM_FLOOR equal to the most-negative value."
        )

    # Pre-align designated initialiser keys so the python-only output is
    # already close to clang-format's AlignConsecutiveAssignments columns
    # (the gen_soc_caps.py cap.c precedent) -- clang-format still runs
    # below and is the source of truth, this just keeps the diff small if
    # it is ever unavailable.
    designators = [f"[-{e.name}]" for e in real]
    width = max(len(d) for d in designators)

    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " * Auto-generated by scripts/gen_status_strings.py.  DO NOT EDIT.",
        " */",
        "",
        "#include <stddef.h>",
        "",
        "#include <alp/peripheral.h>",
        "",
        "/* One more than the most negative status code (ALP_STATUS_ENUM_FLOOR),",
        " * so every declared alp_status_t value (0 .. ALP_STATUS_ENUM_FLOOR)",
        " * indexes the tables below via [-status]. */",
        f"#define ALP_STATUS_TABLE_SIZE ((size_t)(-{floor.name}) + 1u)",
        "",
        "static const char *const _status_names[ALP_STATUS_TABLE_SIZE] = {",
    ]
    for e, d in zip(real, designators):
        key = d.ljust(width)
        lines.append(f"    {key} = {_c_string(e.name)},")
    lines.append("};")
    lines.append("")
    lines.append("const char *alp_status_name(alp_status_t status)")
    lines.append("{")
    lines.append(f"    if (status > ALP_OK || status < {floor.name}) {{")
    lines.append('        return "ALP_STATUS_UNKNOWN";')
    lines.append("    }")
    lines.append("    const char *name = _status_names[(size_t)(-status)];")
    lines.append('    return name != NULL ? name : "ALP_STATUS_UNKNOWN";')
    lines.append("}")
    lines.append("")
    lines.append("#if defined(CONFIG_ALP_STATUS_DESCRIPTIONS)")
    lines.append("")
    lines.append("static const char *const _status_descriptions[ALP_STATUS_TABLE_SIZE] = {")
    for e, d in zip(real, designators):
        key = d.ljust(width)
        lines.append(f"    {key} = {_c_string(e.description)},")
    lines.append("};")
    lines.append("")
    lines.append("const char *alp_status_description(alp_status_t status)")
    lines.append("{")
    lines.append(f"    if (status > ALP_OK || status < {floor.name}) {{")
    lines.append('        return "Unknown status code (out of the declared alp_status_t range).";')
    lines.append("    }")
    lines.append("    const char *desc = _status_descriptions[(size_t)(-status)];")
    lines.append('    return desc != NULL ? desc : "No description available for this status code.";')
    lines.append("}")
    lines.append("")
    lines.append("#else /* !CONFIG_ALP_STATUS_DESCRIPTIONS */")
    lines.append("")
    lines.append("const char *alp_status_description(alp_status_t status)")
    lines.append("{")
    lines.append("    (void)status;")
    lines.append('    return "Status descriptions are compiled out '
                  '(CONFIG_ALP_STATUS_DESCRIPTIONS=n).";')
    lines.append("}")
    lines.append("")
    lines.append("#endif /* CONFIG_ALP_STATUS_DESCRIPTIONS */")

    return "\n".join(lines) + "\n"


def _clang_format(path: Path) -> None:
    """Format the generated file in place to match the repo .clang-format.

    Pinned to clang-format-22 (the CI version).  No-op (with a warning) if
    clang-format is absent; the CI in-sync gate then catches any drift.
    """
    exe = shutil.which("clang-format-22") or shutil.which("clang-format")
    if exe is None:
        print(
            f"  warning: clang-format not found; {path.name} left "
            "unformatted (CI will flag any drift)",
            file=sys.stderr,
        )
        return
    subprocess.run([exe, "-i", "--style=file", str(path)], check=True)


def main() -> int:
    entries = extract_entries(HEADER.read_text(encoding="utf-8"))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(emit(entries), encoding="utf-8")
    _clang_format(OUT)
    print(f"wrote {OUT.relative_to(REPO)} ({len(OUT.read_text().splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
