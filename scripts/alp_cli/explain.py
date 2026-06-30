"""`alp explain <code>` -- look up an Alp error / diagnostic code.

Reads the generated metadata/error-catalog.json (single-sourced from the
`ALP_ERR_*` enum in include/alp/peripheral.h and the docs/diagnostics/ALP-B*.md
landing pages, see scripts/gen_error_catalog.py) and prints the code's
summary / cause / fix / doc-link -- so a firmware engineer (or their agent)
can run `alp explain ALP_ERR_NO_BACKEND` (or `alp explain ALP-B003`) instead
of grepping headers.

Lookup is case-insensitive and accepts both code shapes (`ALP_ERR_*` and
`ALP-Bxxx`).  An unknown code prints the closest matches.  `--json` emits the
raw catalog entry for tooling.
"""

from __future__ import annotations

import difflib
import json
from pathlib import Path

import click

# Reuse the validator's colour plumbing (NO_COLOR / tty aware).
from alp_cli.diagnostic import Fore, Style, _use_color

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / "metadata" / "error-catalog.json"

# Human-readable order + label for the fields we render.
_FIELD_ORDER = (
    ("summary", "summary"),
    ("cause", "cause"),
    ("fix", "fix"),
    ("doc", "doc"),
)


def _load_catalog() -> dict[str, dict]:
    """Return the {code: entry} map from the generated catalog."""
    if not CATALOG.is_file():
        raise click.ClickException(
            f"error catalog not found at {CATALOG.relative_to(REPO)} -- run "
            "`python3 scripts/gen_error_catalog.py`")
    data = json.loads(CATALOG.read_text(encoding="utf-8"))
    return data.get("codes", {})


def _normalise(code: str) -> str:
    """Canonicalise user input for case-insensitive matching."""
    return code.strip().upper()


def _render(entry: dict, color: bool) -> str:
    """Render a catalog entry as a coloured multi-line block."""
    def paint(s: str, hue: str) -> str:
        return f"{hue}{s}{Style.RESET_ALL}" if color else s

    kind = entry.get("kind", "")
    head = f"{paint(entry['code'], Fore.CYAN)}  ({kind})"
    lines = [head]
    for key, label in _FIELD_ORDER:
        val = entry.get(key)
        if val:
            lines.append(f"  {paint(label + ':', Fore.YELLOW)} {val}")
    return "\n".join(lines)


@click.command(name="explain",
               help="Explain an Alp error / diagnostic code (cause, fix, doc).")
@click.argument("code")
@click.option("--json", "as_json", is_flag=True,
              help="Emit the raw catalog entry as JSON.")
@click.option("--no-color", is_flag=True, help="Disable ANSI colours.")
def explain_cmd(code: str, as_json: bool, no_color: bool) -> None:
    """Look up CODE (e.g. ALP_ERR_NO_BACKEND or ALP-B003) in the catalog.

    Case-insensitive; accepts both the `ALP_ERR_*` and `ALP-Bxxx` shapes.
    Exits non-zero on an unknown code.
    """
    codes = _load_catalog()
    by_upper = {_normalise(k): k for k in codes}

    key = by_upper.get(_normalise(code))
    if key is None:
        suggestions = difflib.get_close_matches(
            _normalise(code), list(by_upper), n=5, cutoff=0.4)
        suggested = [by_upper[s] for s in suggestions]
        if as_json:
            click.echo(json.dumps(
                {"error": "unknown-code", "code": code,
                 "suggestions": suggested}, indent=2))
        else:
            msg = f"unknown code '{code}'"
            if suggested:
                msg += "; did you mean: " + ", ".join(suggested) + "?"
            else:
                msg += " -- run `alp explain` against a code from " \
                       "metadata/error-catalog.json"
            click.echo(msg, err=True)
        raise SystemExit(1)

    entry = codes[key]
    if as_json:
        click.echo(json.dumps(entry, indent=2, ensure_ascii=False))
        return
    click.echo(_render(entry, _use_color(False if no_color else None)))
