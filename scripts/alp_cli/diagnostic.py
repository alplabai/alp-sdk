"""Diagnostic data type + Rust-style renderer for the alp validator."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

try:
    from colorama import Fore, Style
    from colorama import init as _colorama_init

    _colorama_init()
except ImportError:  # pragma: no cover - colorama is listed as a dep
    class _Stub:
        def __getattr__(self, _: str) -> str:
            return ""

    Fore = _Stub()  # type: ignore[assignment]
    Style = _Stub()  # type: ignore[assignment]

from collections.abc import Iterator

Severity = Literal["error", "warning", "note"]


@dataclass(slots=True)
class Diagnostic:
    severity: Severity
    path: Path
    line: int
    col: int
    span: int
    code: str
    message: str
    hint: str | None = None
    doc_url: str | None = None


def _doc_url(code: str) -> str:
    base = os.environ.get("ALP_DIAG_BASE_URL", "docs/diagnostics")
    return f"{base}/{code}.md"


def _use_color(color: bool | None) -> bool:
    if color is False:
        return False
    if color is True:
        return True
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stdout.isatty()


def render(diag: Diagnostic, source_text: str, color: bool | None = None) -> str:
    """Render a diagnostic as a multi-line Rust-style block."""
    use_color = _use_color(color)

    def paint(s: str, hue: str) -> str:
        return f"{hue}{s}{Style.RESET_ALL}" if use_color else s

    sev_hue = {"error": Fore.RED, "warning": Fore.YELLOW, "note": Fore.CYAN}[
        diag.severity
    ]
    header = f"{paint(f'{diag.severity}[{diag.code}]', sev_hue)}: {diag.message}"
    arrow = f"  --> {diag.path}:{diag.line}:{diag.col}"

    lines = source_text.splitlines()
    if 1 <= diag.line <= len(lines):
        src_line = lines[diag.line - 1]
    else:
        src_line = ""

    gutter_w = max(2, len(str(diag.line)))
    blank_gutter = " " * gutter_w
    src_block = (
        f"{blank_gutter} |\n"
        f"{str(diag.line).rjust(gutter_w)} | {src_line}\n"
        f"{blank_gutter} | {' ' * (diag.col - 1)}{paint('^' * max(1, diag.span), sev_hue)}"
    )

    tail: list[str] = []
    if diag.hint:
        tail.append(f"{blank_gutter} = hint: {diag.hint}")
    tail.append(f"{blank_gutter} = see: {diag.doc_url or _doc_url(diag.code)}")

    return "\n".join([header, arrow, src_block, *tail, ""])


class DiagnosticCollector:
    """Collect diagnostics across multiple validation passes."""

    def __init__(self) -> None:
        self._items: list[Diagnostic] = []

    def add(self, diag: Diagnostic) -> None:
        self._items.append(diag)

    def __iter__(self) -> Iterator[Diagnostic]:
        return iter(self._items)

    def __len__(self) -> int:
        return len(self._items)

    def has_errors(self) -> bool:
        return any(d.severity == "error" for d in self._items)

    def emit(self, source_text: str, color: bool | None = None) -> None:
        for diag in self._items:
            print(render(diag, source_text=source_text, color=color))
