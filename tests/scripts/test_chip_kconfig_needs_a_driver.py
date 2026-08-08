# SPDX-License-Identifier: Apache-2.0
"""A `CONFIG_ALP_SDK_CHIP_<X>` line requires a `chips/<x>/` driver (#1241).

Every declaration in `zephyr/kconfigs/chips.kconfig` reads "Compile
chips/<part>/<part>.c", so the symbol only exists when the driver does.
Emitting the assignment for a chip with no driver writes a `CONFIG_` line
Zephyr cannot resolve.

`_slugs_from_on_module` already applies this rule per-FIELD: `ospi_memories`
and `hyperram` are excluded precisely because those parts have no
`chips/<part>/` driver. It has to hold per-CHIP as well -- a scalar
`on_module:` field can name a real chip manifest with nothing behind it,
which is what `ethernet_phy: dp83825` did. `dp83825` carries
`driver_status: none` and has no `chips/dp83825/` directory, yet every AEN
Zephyr slice emitted `CONFIG_ALP_SDK_CHIP_DP83825=y`.

Nothing else catches this: it surfaces only as emit-snapshot drift, and
`scripts/check_emit_snapshots.py` runs in no harness other than
`scripts/test-all.sh`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate.kconfig import _chip_has_driver  # noqa: E402

CHIPS_KCONFIG = REPO / "zephyr" / "kconfigs" / "chips.kconfig"
CHIPS_META = REPO / "metadata" / "chips"


def _declared_symbols() -> set[str]:
    text = CHIPS_KCONFIG.read_text(encoding="utf-8")
    return set(re.findall(r"^config (ALP_SDK_CHIP_[A-Z0-9_]+)$", text, re.M))


def _chip_ids() -> list[str]:
    ids = []
    for f in sorted(CHIPS_META.glob("*.yaml")):
        doc = yaml.safe_load(f.read_text(encoding="utf-8")) or {}
        cid = doc.get("chip_id")
        if cid:
            ids.append(str(cid))
    return ids


def test_there_are_chips_to_check() -> None:
    """Guard against the guard: an empty glob would make the sweep below
    pass while covering nothing."""
    assert len(_chip_ids()) >= 50, "chip-manifest glob has drifted"


def test_dp83825_has_no_driver_and_so_gets_no_kconfig() -> None:
    """The concrete regression. Red before the emitter honoured driver
    presence: `CONFIG_ALP_SDK_CHIP_DP83825=y` appeared in every AEN Zephyr
    slice while `ALP_SDK_CHIP_DP83825` was undeclared."""
    assert not (REPO / "chips" / "dp83825").is_dir(), (
        "chips/dp83825/ now exists -- if a real driver landed, declare "
        "ALP_SDK_CHIP_DP83825 in chips.kconfig and update this test"
    )
    assert _chip_has_driver("dp83825") is False
    assert "ALP_SDK_CHIP_DP83825" not in _declared_symbols()


def test_every_chip_with_a_driver_dir_has_a_declared_symbol() -> None:
    """The other direction: a driver that exists must be selectable, or the
    sources are dead code no Kconfig can reach."""
    missing = [
        cid
        for cid in _chip_ids()
        if (REPO / "chips" / cid).is_dir()
        and f"ALP_SDK_CHIP_{cid.upper()}" not in _declared_symbols()
    ]
    assert not missing, f"chips/<x>/ exists but no Kconfig symbol declares it: {missing}"


@pytest.mark.parametrize("cid", _chip_ids(), ids=lambda c: c)
def test_declared_symbol_implies_a_driver_dir(cid: str) -> None:
    """A declared `ALP_SDK_CHIP_<X>` whose `chips/<x>/` is absent would make
    the Kconfig help text ("Compile chips/<x>/<x>.c") a lie and select
    nothing. Parametrised so the failure names the chip."""
    if f"ALP_SDK_CHIP_{cid.upper()}" not in _declared_symbols():
        pytest.skip(f"{cid} declares no chip symbol")
    assert _chip_has_driver(cid), (
        f"ALP_SDK_CHIP_{cid.upper()} is declared in chips.kconfig but "
        f"chips/{cid}/ does not exist"
    )
