# SPDX-License-Identifier: Apache-2.0
"""Write the PRIVATE per-unit ledger for V2N provisioning.

``<SKU>/<serial>.unit.yaml`` (flat ``key: value``), ``<serial>.md``, per-step
logs, the xlsx regen and the ship check. The ledger lives in the private
repository and is passed in by path; nothing here names a unit or a bench.

``unit.yaml`` rules: only a line whose first non-blank character is ``#`` is a
comment (an inline ``#`` such as ``repo#1234`` is part of the value); a key the
catalogue marks ``manual`` is never overwritten by the tool.
"""

from __future__ import annotations

import os
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import yaml


def load_catalogue(path: Path) -> dict[str, dict]:
    doc = yaml.safe_load(Path(path).read_text(encoding="utf-8")) or {}
    keys = doc.get("keys")
    if doc.get("schema") != 1 or not isinstance(keys, dict):
        raise ValueError(f"{path}: want schema 1 with a 'keys' mapping")
    for key, spec in keys.items():
        missing = {"group", "source", "mode", "ship_required"} - set(spec or {})
        if missing or spec["mode"] not in ("auto", "manual"):
            raise ValueError(f"{path}: key {key}: bad spec {spec}")
    return keys


def _parse_line(line: str) -> tuple[str, str] | None:
    s = line.strip()
    if not s or s.startswith("#") or ":" not in s:
        return None
    key, _, value = s.partition(":")
    return key.strip(), value.strip().strip('"').strip("'")


def read_unit_yaml(path: Path) -> dict[str, str]:
    path = Path(path)
    if not path.exists():
        return {}
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        kv = _parse_line(line)
        if kv:
            out[kv[0]] = kv[1]
    return out


def _is_manual(key: str, catalogue: dict[str, dict]) -> bool:
    spec = catalogue.get(key)
    return bool(spec) and spec.get("mode") == "manual"


def merge_unit_yaml(path: Path, auto: dict[str, str], catalogue: dict[str, dict],
                    defaults: dict[str, str] | None = None) -> list[str]:
    """Update/insert `auto` keys; returns the keys changed.

    A key the catalogue marks manual is never touched, and neither is any
    line not named in `auto`. `defaults` are inserted only when the key is
    absent (e.g. ``disposition: bench-only`` for a unit built from an
    unsigned ``--build-dir``) -- an existing value, manual or not, is never
    replaced by a default.
    """
    path = Path(path)
    for k, v in {**auto, **(defaults or {})}.items():
        if "\n" in str(v) or "\r" in str(v):
            raise ValueError(f"{k}: value contains a newline")
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    index = {}
    for i, line in enumerate(lines):
        kv = _parse_line(line)
        if kv:
            index[kv[0]] = i
    changed = []
    for key, value in auto.items():
        value = str(value)
        if _is_manual(key, catalogue):
            continue
        new = f"{key}: {value}"
        if key in index:
            if _parse_line(lines[index[key]])[1] != value:
                lines[index[key]] = new
                changed.append(key)
        else:
            index[key] = len(lines)
            lines.append(new)
            changed.append(key)
    for key, value in (defaults or {}).items():
        if key not in index:
            index[key] = len(lines)
            lines.append(f"{key}: {value}")
            changed.append(key)
    if changed:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return changed


def append_md_section(path: Path, title: str, body: str, when: datetime) -> None:
    path = Path(path)
    stamp = when.astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
    text = f"\n## {title} ({stamp})\n\n{body.rstrip()}\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def write_log(ledger_root: Path, sku: str, serial: str, step: str, text: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    p = Path(ledger_root) / sku / "logs" / serial / f"{step}-{stamp}.log"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text if text.endswith("\n") else text + "\n", encoding="utf-8", newline="\n")
    return p


def regen_xlsx(ledger_root: Path, tool: Path, output: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(tool), "--ledger-root", str(ledger_root), "--output", str(output)],
        capture_output=True, text=True, encoding="utf-8", check=False,
        env={**os.environ, "PYTHONIOENCODING": "utf-8"})


DEBUG_MODE_REASON = ("SoC is strapped to debug mode (MD_BOOT3 high, SYS_LSI_MODE debug bit set): "
                     "a production SoM must boot in normal mode (MD_BOOT3 low)")


def ship_check(unit: dict[str, str], catalogue: dict[str, dict], family: str = "") -> list[str]:
    """Why this unit cannot ship; [] = shippable. Same rules as the private
    ledger_xlsx.py Ship check (minus its staged-manifest leg)."""
    reasons = []
    for key, spec in catalogue.items():
        # ship_required_for: manifest families (e.g. v2n-m1, whose DX-M1 must be flashed)
        # for which the key is required although the other families may leave it blank.
        required = spec.get("ship_required") or family in spec.get("ship_required_for", ())
        val = str(unit.get(key, "")).strip()
        # `unread (<error>)` is the census's record of a read that failed: not a value
        if required and "*" not in key and (not val or val == "unread" or val.startswith("unread (")):
            reasons.append(f"missing {key}")
    # The ACT88760 GPIO4 OTP default is an expected workaround, not a defect
    # (maintainer decision 2026-09-29) -- U-Boot releases it every boot. Block
    # only when cold_boot_test's own post-boot read shows it was NOT released;
    # a legacy `act88760_gpio4_defect: yes` key on an existing unit is not
    # itself a blocker any more.
    if str(unit.get("act88760_gpio4_after_boot", "")).strip().lower() == "0x88":
        reasons.append("act88760_gpio4_after_boot: 0x88 (image did not release GD32_NRST)")
    # The latched MD_BOOT3 strap (census/census_final: soc_boot_debug_en). Debug mode is for
    # bring-up benches only; the register facts are in the SoC description (sys_lsi).
    if str(unit.get("soc_boot_debug_en", "")).strip() == "1":
        reasons.append(DEBUG_MODE_REASON)
    # functional_test's summary. Its per-check test_ft_<check> keys are detail (informational
    # failures and fixture skips never block).
    # Only `pass` ships: a unit on which functional_test never ran, was skipped, crashed or failed
    # has no proof that its interfaces work. (A unit provisioned before the step existed is
    # blocked until the step has run on it: intended.)
    functional = str(unit.get("test_functional", "")).strip()
    if not functional:
        reasons.append("missing test_functional (functional_test has not run on this unit)")
    elif not functional.startswith("pass"):
        reasons.append(f"test_functional: {functional}")
    disposition = str(unit.get("disposition", "")).strip()
    if disposition != "ship":
        reasons.append(f"disposition is {disposition or 'unset'}, not ship")
    defects = str(unit.get("known_defects", "")).strip()
    if defects and defects.lower() != "none":
        reasons.append(f"known_defects: {defects}")
    overrides = str(unit.get("provision_overrides", "")).strip()
    if overrides and overrides.lower() != "none":
        reasons.append(f"provision_overrides: {overrides}")
    if str(unit.get("rootfs_bundle_version", "")).startswith("build-dir:"):
        reasons.append("provisioned from an unsigned --build-dir, not a release bundle")
    return reasons


def promote_manifest(ledger_dir: Path, serial: str) -> Path:
    src = Path(ledger_dir) / f"{serial}.manifest.staged.bin"
    dst = Path(ledger_dir) / f"{serial}.manifest.bin"
    if dst.exists():
        raise ValueError(f"{dst} already exists; refusing to replace a written manifest")
    if not src.is_file():
        raise ValueError(f"{src} missing")
    os.replace(src, dst)
    return dst
