# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_registries.py + scripts/alp_project_loader.py's
`silicon_to_kconfig()` -- the two registry-backed Kconfig lookups #1485's
first fix round missed (round two): `peripheral_kconfig()` was frozen into
a `_PERIPHERAL_KCONFIG` module constant at *import time* (before any
`--metadata-root` flag is parsed) in both `alp_orchestrate/slugs.py` and
`alp_project_emit/__init__.py`, and `silicon_to_kconfig()` cached its
registry read with an unkeyed `functools.lru_cache`, so a project-scoped
`--metadata-root` override never reached either lookup.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_registries.py -v
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

from alp_orchestrate import _slice_alp_conf, load_board_yaml  # noqa: E402


# A V2N101 board with an `adc` peripheral on its only zephyr core, used by
# both the silicon-symbol and the peripheral-symbol assertions below --
# `_slice_alp_conf` emits the SoM-silicon CONFIG_ALP_SOC_* line and the
# per-peripheral CONFIG_<X>=y line(s) in the same pass.
_V2N_WITH_ADC = """
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc]
"""


def test_silicon_and_peripheral_kconfig_resolve_against_explicit_metadata_root(
    tmp_path: Path,
) -> None:
    """#1485 round two: both `silicon_to_kconfig()` and
    `alp_registries.peripheral_kconfig()` must resolve against
    `project.effective_metadata_root()`, not a module-level default frozen
    at import time (peripheral) or an unkeyed cache (silicon).

    Mutates a scratch copy of `metadata/` exactly as the adversarial
    review's manual repro did: rewrite `silicon-kconfig.json`'s
    `socSymbolPrefix` to a scratch-only prefix, and re-key
    `peripheral-kconfig.json`'s `adc` entry to a scratch-only marker
    symbol. The emitted alp.conf must carry both scratch symbols under the
    override, and both real in-tree symbols with no override.
    """
    meta = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", meta)

    silicon_registry = meta / "registries" / "silicon-kconfig.json"
    silicon_data = json.loads(silicon_registry.read_text(encoding="utf-8"))
    assert silicon_data["socSymbolPrefix"] == "ALP_SOC_"
    silicon_data["socSymbolPrefix"] = "ALP_SCRATCHSOC_"
    silicon_registry.write_text(json.dumps(silicon_data, indent=2) + "\n",
                                 encoding="utf-8")

    peripheral_registry = meta / "registries" / "peripheral-kconfig.json"
    peripheral_data = json.loads(peripheral_registry.read_text(encoding="utf-8"))
    assert peripheral_data["peripherals"]["adc"] == ["ADC"]
    peripheral_data["peripherals"]["adc"] = ["ADC_SCRATCH_MARKER"]
    peripheral_registry.write_text(
        json.dumps(peripheral_data, indent=2) + "\n", encoding="utf-8")

    path = _write_board(tmp_path, _V2N_WITH_ADC)

    # Scratch root: both lookups must reflect the scratch registries.
    project = load_board_yaml(path, metadata_root=meta)
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_ALP_SCRATCHSOC_RENESAS_RZV2N_N44=y" in out
    assert "CONFIG_ALP_SOC_RENESAS_RZV2N_N44=y" not in out
    assert "CONFIG_ADC_SCRATCH_MARKER=y" in out
    assert "CONFIG_ADC=y" not in out

    # No override: must resolve against the real in-tree registries.
    project_intree = load_board_yaml(path)
    out_intree = _slice_alp_conf(project_intree, project_intree.cores["m33_sm"])
    assert "CONFIG_ALP_SOC_RENESAS_RZV2N_N44=y" in out_intree
    assert "CONFIG_ALP_SCRATCHSOC_RENESAS_RZV2N_N44=y" not in out_intree
    assert "CONFIG_ADC=y" in out_intree
    assert "CONFIG_ADC_SCRATCH_MARKER=y" not in out_intree


def demo() -> None:
    """Smallest runnable self-check: the two registries key correctly on
    `metadata_root` without a full pytest collection."""
    import tempfile

    import alp_registries
    from alp_project_loader import silicon_to_kconfig

    with tempfile.TemporaryDirectory() as td:
        meta = Path(td) / "metadata"
        shutil.copytree(REPO / "metadata", meta)
        reg = meta / "registries" / "peripheral-kconfig.json"
        data = json.loads(reg.read_text(encoding="utf-8"))
        data["peripherals"]["adc"] = ["ADC_SCRATCH_MARKER"]
        reg.write_text(json.dumps(data), encoding="utf-8")

        scratch_map = alp_registries.peripheral_kconfig(meta)
        intree_map = alp_registries.peripheral_kconfig(alp_registries.METADATA_ROOT)
        assert scratch_map["adc"] == ("ADC_SCRATCH_MARKER",)
        assert intree_map["adc"] == ("ADC",), "in-tree lookup must be unaffected"

        assert silicon_to_kconfig("renesas:rzv2n:n44", meta) == "ALP_SOC_RENESAS_RZV2N_N44"

    print("demo: OK")


if __name__ == "__main__":
    demo()
