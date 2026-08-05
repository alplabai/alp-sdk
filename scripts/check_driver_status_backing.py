#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: driver_status declarations outside metadata/chips/*.yaml are bound
to the artefact they claim to describe (issue #1216).

metadata/schemas/som-preset-v1.schema.json's shared `$defs/driver_status`
vocabulary (`none` / `planned` / `partial` / `complete`) is reused by three
fields in metadata/e1m_modules/<SKU>.yaml that had NO parity enforcement --
only metadata/chips/<id>.yaml's driver_status was bound to anything (its
<alp/chips/<id>.h> header tag, via check_chip_header_status.py):

  - mailbox.driver_status              -- claims a driver for `controller:`.
    Bound to that controller's known source file under zephyr/drivers/mbox/.
  - on_module.nor_flash_driver_status /
    on_module.emmc_driver_status       -- `none` is the only truthful value
    while no Renesas-named source exists under zephyr/drivers/flash/ (nor
    flash) or zephyr/drivers/sdhc/ (eMMC) -- see the inline "no alp-sdk
    driver yet" comments in metadata/e1m_modules/E1M-V2M101.yaml:35,37
    (#1169). A `partial`/`complete` claim here must point at a real one.
  - soc_peripheral_instances[].driver_status -- `none` is the only truthful
    value while no zephyr/drivers/** source mentions the entry's `instance`
    slug (the schema's own words: "`none` ... is the honest value while no
    zephyr/drivers/** node targets it"). A `partial`/`complete` claim must be
    backed by one.

Only `partial`/`complete` are treated as claiming a driver exists now;
`none` makes no claim and `planned` is a roadmap intent, not a claim about
today's tree (mirrors metadata/chips/*.yaml's own use of `planned` for
chips with no Alp SDK driver at all, e.g. murata_lbee0zz2kl.yaml).

This gate is deliberately about the ABSENCE of a driver as much as its
presence -- a preset that upgrades one of these fields without adding the
backing driver is the overclaim this gate exists to catch.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("check_driver_status_backing: PyYAML is required.")

REQUIRES_BACKING = {"partial", "complete"}

# mailbox.controller -> the file under zephyr/drivers/mbox/ that backs it.
# Not a generic substring search: the metadata token ("renesas_mhu") and the
# driver's own DT_DRV_COMPAT / filename ("renesas_rz_mhu_b") are not
# textually related, so this map is the actual binding.
MAILBOX_DRIVER_FILES = {
    "alif_mhuv2": "mbox_alif_mhuv2.c",
    "renesas_mhu": "mbox_renesas_rz_mhu_b.c",
}

# on_module.<field> -> the zephyr/drivers/ subdir a backing driver would live
# under, and the vendor token it would have to carry (#1169: neither exists
# today for either storage route).
STORAGE_FIELDS = {
    "nor_flash_driver_status": "flash",
    "emmc_driver_status": "sdhc",
}

# A file whose DT_DRV_COMPAT token starts with "renesas" is a real driver
# binding; a bare "renesas" substring anywhere in the file (e.g. a comment
# naming the vendor in passing) is not evidence of one.
_DT_COMPAT_RENESAS_RE = re.compile(
    r"^\s*#\s*define\s+DT_DRV_COMPAT\s+renesas[a-z0-9_]*", re.IGNORECASE | re.MULTILINE
)


def _load_yaml(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def _driver_tree_text(root: Path) -> str:
    """Concatenated contents of every file under zephyr/drivers/**."""
    drivers = root / "zephyr" / "drivers"
    if not drivers.is_dir():
        return ""
    return "\n".join(
        f.read_text(encoding="utf-8", errors="replace") for f in drivers.rglob("*") if f.is_file()
    )


def _subdir_is_renesas_backed(subdir: Path) -> bool:
    if not subdir.is_dir():
        return False
    for f in subdir.rglob("*"):
        if not f.is_file():
            continue
        if "renesas" in f.name.lower():
            return True
        if _DT_COMPAT_RENESAS_RE.search(f.read_text(encoding="utf-8", errors="replace")):
            return True
    return False


def check(root: Path) -> list[str]:
    presets_dir = root / "metadata" / "e1m_modules"
    issues: list[str] = []
    if not presets_dir.is_dir():
        return issues

    drivers_text = _driver_tree_text(root)

    for preset in sorted(presets_dir.glob("*.yaml")):
        doc = _load_yaml(preset)
        rel = preset.relative_to(root)

        mailbox = doc.get("mailbox") or {}
        status = mailbox.get("driver_status")
        if status in REQUIRES_BACKING:
            controller = mailbox.get("controller")
            driver_file = MAILBOX_DRIVER_FILES.get(controller)
            if driver_file is None or not (root / "zephyr" / "drivers" / "mbox" / driver_file).is_file():
                issues.append(
                    f"{rel}: mailbox.driver_status '{status}' claims a driver for "
                    f"controller '{controller}', but no known zephyr/drivers/mbox/ "
                    f"file backs it"
                )

        on_module = doc.get("on_module") or {}
        for field, subdir_name in STORAGE_FIELDS.items():
            status = on_module.get(field)
            if status not in REQUIRES_BACKING:
                continue
            if not _subdir_is_renesas_backed(root / "zephyr" / "drivers" / subdir_name):
                issues.append(
                    f"{rel}: on_module.{field} '{status}' claims a Renesas "
                    f"storage driver, but zephyr/drivers/{subdir_name}/ has no "
                    f"renesas-named file or renesas DT_DRV_COMPAT binding"
                )

        for entry in doc.get("soc_peripheral_instances") or []:
            status = entry.get("driver_status")
            if status not in REQUIRES_BACKING:
                continue
            instance = entry.get("instance", "")
            # Leading boundary excludes only letters/digits, not `_`: driver
            # source commonly embeds the instance as a SUFFIX of a
            # snake_case compat string (e.g. `renesas_rz_rspi0`), which a
            # `\b`-based match would miss since `_` counts as a word
            # character there. The trailing boundary, unlike the leading
            # one, DOES exclude `_`: without that, an instance slug that is
            # merely a PREFIX of an unrelated identifier (e.g. instance
            # `sd1` inside the pin-name comment `SD1_CD`) reads as a match.
            pattern = rf"(?<![A-Za-z0-9]){re.escape(instance)}(?![A-Za-z0-9_])"
            if instance and not re.search(pattern, drivers_text, re.IGNORECASE):
                issues.append(
                    f"{rel}: soc_peripheral_instances[instance={instance!r}]"
                    f".driver_status '{status}' claims a driver for silicon "
                    f"peripheral '{entry.get('silicon_peripheral')}', but no "
                    f"zephyr/drivers/** source mentions instance '{instance}'"
                )

    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    issues = check(args.root)
    if not issues:
        return 0

    print(
        "check_driver_status_backing: driver_status claims an artefact that "
        "does not exist:"
    )
    for i in issues:
        print(f"  {i}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
