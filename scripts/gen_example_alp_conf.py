#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Pre-generate each Zephyr example's per-core ``generated/alp.conf``.

Twister and a bare ``west build`` configure an example straight from its
source dir with no SDK-side step in front of them, so the per-core Kconfig
fragment (`alp_orchestrate.kconfig._slice_alp_conf`, the same function the
build plan's ``configArtefacts`` use) has to exist on disk BEFORE they run.
Each example's ``testcase.yaml`` points ``EXTRA_CONF_FILE`` at
``generated/alp.conf``; it lives under ``generated/`` (git-ignored) because
Zephyr auto-merges every ``*.conf`` beside ``prj.conf``.

Walks every example ``board.yaml`` (the corpus ``check_zephyr_conf_parity.py``
pins) and writes ``<app dir>/generated/alp.conf`` for each enabled Zephyr core,
where the app dir is the one ``west build`` is pointed at for that core.

Usage:

    python3 scripts/gen_example_alp_conf.py [example_dir ...]

With no arguments every example is generated.
"""
from __future__ import annotations

import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import check_zephyr_conf_parity as parity  # noqa: E402
from alp_orchestrate import load_board_yaml, OrchestratorError  # noqa: E402
from alp_orchestrate.kconfig import _slice_alp_conf  # noqa: E402


def _write(out: Path, text: str) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    # newline="" keeps the fragment byte-identical to the planner's on Windows.
    with open(out, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def _twin_skus(sku: str) -> list[str]:
    """Other SoM SKUs on the same silicon part as `sku` (e.g. E1M-AEN803 for
    E1M-AEN801): same PCB, so an app built for either board, but their
    populated-memory facts differ (#2597)."""
    mods = REPO / "metadata" / "e1m_modules"

    def key(name: str):
        d = yaml.safe_load((mods / f"{name}.yaml").read_text(encoding="utf-8"))
        return d.get("silicon"), d.get("silicon_variant")

    me = key(sku)
    return sorted(p.stem for p in mods.glob("E1M-*.yaml")
                  if p.stem != sku and key(p.stem) == me)


def generate(app_dir: Path, board_yaml: Path, core_id: str) -> Path:
    """Write and return ``<app_dir>/generated/alp.conf``.

    A twin SKU whose fragment differs also gets
    ``generated/<sku-lowercase-sans-E1M->/alp.conf`` (e.g. ``aen803``); the
    example's CMakeLists.txt points EXTRA_CONF_FILE at it when BOARD names that
    SKU, so the SoM facts follow the board being built, not ``som.sku``.
    """
    project = load_board_yaml(board_yaml)
    if core_id not in project.cores:
        raise OrchestratorError(f"--core {core_id} not in {board_yaml}")
    text = _slice_alp_conf(project, project.cores[core_id])
    out = app_dir / "generated" / "alp.conf"
    _write(out, text)
    for sku in _twin_skus(project.sku):
        try:
            twin = load_board_yaml(board_yaml, sku=sku)
            ttext = _slice_alp_conf(twin, twin.cores[core_id])
        except (OrchestratorError, KeyError):
            continue
        if ttext != text:
            _write(app_dir / "generated" / sku[4:].lower() / "alp.conf", ttext)
    return out


def main(argv: list[str]) -> int:
    only = {Path(a).resolve() for a in argv}
    failures, n = [], 0
    seen: set[Path] = set()
    for app_dir, board_yaml, core_id in parity.find_cases():
        if only and app_dir.resolve() not in only:
            continue
        seen.add(app_dir.resolve())
        try:
            generate(app_dir, board_yaml, core_id)
            n += 1
        except (OrchestratorError, OSError) as e:
            failures.append(f"{app_dir.relative_to(REPO).as_posix()}: {e}")
    for d in sorted(only - seen):
        for rel, why in parity.EXCLUDED_WITH_REASON.items():
            bdir = (REPO / rel).parent.resolve()
            if d == bdir or bdir in d.parents:
                print(f"SKIP {rel}: {why}")
                break
        else:
            failures.append(f"{d}: matches no Zephyr example core")
    for f in failures:
        print(f"gen_example_alp_conf: {f}", file=sys.stderr)
    print(f"gen_example_alp_conf: wrote {n} generated/alp.conf")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
