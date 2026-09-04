# SPDX-License-Identifier: Apache-2.0
"""`memory:` is DERIVED from the `on_module` population facts -- pin the
derivation, because for one commit only a comment held it.

Every AEN preset states the rule in prose
(`metadata/e1m_modules/E1M-AEN801.yaml`: *"dram_mbit  <- 0:
on_module.hyperram is `assembled: false`"*).  Nothing bound it: setting
`hyperram.assembled: true` while leaving `dram_mbit: 0` was FULLY GREEN --
`validate_metadata.py` rc=0 AND `pytest tests/scripts/` rc=0 -- and
`dram_mbit: 128` against an unpopulated part left only one hardcoded
string assertion red.

`_check_som_memory_population` in `scripts/validate_metadata.py` is the
binding; these tests are the second, independent copy of it, so removing
the check reddens something other than the check's own file.  The `0`
vs `TBD` semantics are #915's and are load-bearing here: `0` is a
RESOLVED fact ("this SKU populates none"), `TBD` an open question
("nobody has written the capacity down yet", E1M-NX9101's state).
"""

import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]

AEN_PRESETS = [
    "E1M-AEN301", "E1M-AEN401", "E1M-AEN501",
    "E1M-AEN601", "E1M-AEN701", "E1M-AEN801",
]


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_som_memory_population", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


def _write_fixture(name: str, body: str) -> Path:
    # _check_som_memory_population() reports paths relative to REPO, so the
    # fixture must live inside the checkout.
    p = REPO / "metadata" / "e1m_modules" / f".test-{name}.yaml"
    p.write_text(body, encoding="utf-8")
    return p


def _check(name: str, body: str):
    vm = _load_vm()
    p = _write_fixture(name, body)
    try:
        return vm._check_som_memory_population([p])
    finally:
        p.unlink(missing_ok=True)


def _hyperram(assembled: str, dram: str, capacity: str = "256") -> str:
    return (
        "on_module:\n"
        "  hyperram:\n"
        "    chip:           W958D8NBYA5I\n"
        f"    assembled:      {assembled}\n"
        f"    capacity_mbit:  {capacity}\n"
        "memory:\n"
        f"  dram_mbit:            {dram}\n"
        "  flash_mbit:           0\n"
    )


def _ospi(assembled0: str, assembled1: str, flash: str) -> str:
    return (
        "on_module:\n"
        "  ospi_memories:\n"
        "    ospi0:\n"
        "      chip:           MX25UM25645GXDI00\n"
        f"      assembled:      {assembled0}\n"
        "      capacity_mbit:  256\n"
        "      role:           app_storage\n"
        "    ospi1:\n"
        "      chip:           TBD\n"
        f"      assembled:      {assembled1}\n"
        "      capacity_mbit:  128\n"
        "      role:           data_log\n"
        "memory:\n"
        "  dram_mbit:            0\n"
        f"  flash_mbit:           {flash}\n"
    )


# --- the derivation the AEN presets actually declare -------------------

def test_accepts_unpopulated_hyperram_with_zero_dram():
    assert not _check("mem-hyperram-absent-zero", _hyperram("false", "0"))


def test_rejects_populated_hyperram_with_zero_dram():
    """The mutation that used to be fully green."""
    failures = _check("mem-hyperram-fitted-zero", _hyperram("true", "0"))
    assert failures
    assert "memory.dram_mbit=0" in failures[0][1][0]
    assert "on_module.hyperram (`assembled: true`) is populated" in failures[0][1][0]


def test_rejects_unpopulated_hyperram_with_a_capacity():
    failures = _check("mem-hyperram-absent-capacity", _hyperram("false", "128"))
    assert failures
    assert "`assembled: false`" in failures[0][1][0]


def test_rejects_unpopulated_hyperram_spelled_tbd():
    """`0` is resolved, `TBD` is open -- a preset that has answered the
    population question may not re-open it (#915)."""
    failures = _check("mem-hyperram-absent-tbd", _hyperram("false", '"TBD"'))
    assert failures
    assert "never TBD" in failures[0][1][0]


def test_rejects_populated_hyperram_whose_capacity_disagrees():
    failures = _check("mem-hyperram-fitted-mismatch", _hyperram("true", "128"))
    assert failures
    assert "capacity_mbit=256" in failures[0][1][0]


def test_accepts_populated_hyperram_matching_its_capacity():
    assert not _check("mem-hyperram-fitted-match", _hyperram("true", "256"))


def test_accepts_populated_hyperram_with_tbd_capacity():
    """A fitted part whose OWN capacity is unwritten leaves the module
    figure genuinely underivable -- TBD is the honest answer, not a
    failure."""
    assert not _check("mem-hyperram-fitted-tbd-cap",
                      _hyperram("true", '"TBD"', capacity='"TBD"'))


def test_rejects_optional_hyperram_with_zero_dram():
    """`optional` means some BOM variant DOES carry it, so `0` ("no such
    part on any current BOM variant") contradicts it."""
    failures = _check("mem-hyperram-optional-zero", _hyperram("optional", "0"))
    assert failures
    assert "`assembled: optional`" in failures[0][1][0]


def test_accepts_optional_hyperram_with_any_capacity():
    """The capacity of the variant that fits the part is BOM-dependent, so
    only the `0` contradiction above is decidable."""
    assert not _check("mem-hyperram-optional-cap", _hyperram("optional", "128"))


# --- flash is bound to the OSPI memories, symmetrically ----------------

def test_accepts_unpopulated_ospi_memories_with_zero_flash():
    assert not _check("mem-ospi-absent-zero", _ospi("false", "false", "0"))


@pytest.mark.parametrize("a0,a1", [("true", "false"), ("false", "true")])
def test_rejects_any_populated_ospi_memory_with_zero_flash(a0, a1):
    failures = _check(f"mem-ospi-fitted-{a0}-{a1}", _ospi(a0, a1, "0"))
    assert failures
    assert "memory.flash_mbit=0" in failures[0][1][0]


def test_rejects_unpopulated_ospi_memories_with_a_capacity():
    failures = _check("mem-ospi-absent-capacity", _ospi("false", "false", "256"))
    assert failures
    assert "on_module.ospi_memories.ospi0" in failures[0][1][0]
    assert "on_module.ospi_memories.ospi1" in failures[0][1][0]


def test_a_mixed_fitted_and_optional_set_states_each_part_s_own_status():
    """A blanket `assembled: true` across a mixed set would misreport the
    optional half, so each offender is named with its own state."""
    failures = _check("mem-ospi-mixed-zero", _ospi("true", "optional", "0"))
    assert failures
    msg = failures[0][1][0]
    assert "on_module.ospi_memories.ospi0 (`assembled: true`)" in msg
    assert "on_module.ospi_memories.ospi1 (`assembled: optional`)" in msg


def test_flash_derives_from_the_sum_of_every_fitted_ospi_memory():
    assert not _check("mem-ospi-both-fitted-sum", _ospi("true", "true", "384"))
    failures = _check("mem-ospi-both-fitted-bad", _ospi("true", "true", "256"))
    assert failures
    assert "sum to" in failures[0][1][0]
    assert "capacity_mbit=384" in failures[0][1][0]


def test_missing_assembled_key_reads_as_populated():
    """The schema's own default is `true` ("Population status: true
    (default)"), so an entry with no `assembled:` describes a FITTED part
    -- it must not read as "unknown" and skip the check."""
    body = (
        "on_module:\n"
        "  ospi_memories:\n"
        "    ospi0:\n"
        "      chip:           MX25UM25645GXDI00\n"
        "      capacity_mbit:  256\n"
        "      role:           app_storage\n"
        "memory:\n"
        "  dram_mbit:            0\n"
        "  flash_mbit:           0\n"
    )
    failures = _check("mem-ospi-default-assembled", body)
    assert failures
    assert "`assembled: true`" in failures[0][1][0]


# --- presets that declare no population fact are SKIPPED, not guessed --

def test_skips_a_preset_that_declares_no_on_module_memory_parts():
    """V2N/V2M carry LPDDR4X + eMMC and declare neither block; E1M-NX9101
    declares neither and leaves both figures TBD.  There is nothing to
    bind to, and inventing a population fact would be inventing a
    hardware value."""
    assert not _check(
        "mem-no-population-facts",
        "on_module:\n"
        "  silicon:              renesas:rzv2n:n44\n"
        "memory:\n"
        "  dram_mbit:            32768\n"
        "  flash_mbit:           32768\n",
    )


def test_skips_a_preset_with_no_memory_block():
    assert not _check("mem-no-memory-block", "sku: E1M-TST001\n")


def test_an_unbound_preset_prints_an_explicit_skip_line(capsys):
    """A skip that prints NOTHING is indistinguishable from a file the loop
    never reached.  Measured at d3ea9c8f: an out-of-scope preset produced no
    line at all from this check, so "not cross-checked" was an absence you
    had to notice rather than a fact you could read."""
    vm = _load_vm()
    p = _write_fixture(
        "mem-skip-line",
        "silicon: renesas:rzv2n:n44\n"
        "on_module:\n"
        "  silicon:              renesas:rzv2n:n44\n"
        "memory:\n"
        "  dram_mbit:            32768\n"
        "  flash_mbit:           32768\n",
    )
    try:
        assert not vm._check_som_memory_population([p])
    finally:
        p.unlink(missing_ok=True)
    out = capsys.readouterr().out
    assert "SKIP " in out
    assert ".test-mem-skip-line.yaml" in out
    assert "nothing bound" in out


def test_a_bound_preset_prints_ok_rather_than_skip(capsys):
    vm = _load_vm()
    p = _write_fixture("mem-ok-line", _hyperram("false", "0"))
    try:
        assert not vm._check_som_memory_population([p])
    finally:
        p.unlink(missing_ok=True)
    out = capsys.readouterr().out
    assert "OK   " in out
    assert "dram_mbit <- on_module.hyperram" in out
    assert "SKIP " not in out


# --- ...but an Alif Ensemble preset may NOT reach that skip ------------
#
# Skipping-when-absent is right for a family whose external memory the SDK
# has no model of, and WRONG for one where these two blocks are the whole
# model.  Measured at d3ea9c8f: deleting both blocks from E1M-AEN801.yaml
# and restating `dram_mbit: 256` / `flash_mbit: 256` -- the exact 32-MiB
# claim #915 had just deleted -- left `validate_metadata.py` at rc=0 with
# NO line printed for that preset.  A new AEN SKU could have shipped the
# retracted claim invisibly.

_ALIF_NO_BLOCKS = (
    "sku: E1M-AEN801\n"
    "silicon: alif:ensemble:e8\n"
    "on_module:\n"
    "  silicon:              alif:ensemble:e8\n"
    "memory:\n"
    "  dram_mbit:            256\n"
    "  flash_mbit:           256\n"
)


def test_an_alif_ensemble_preset_may_not_omit_the_population_blocks():
    failures = _check("mem-alif-no-blocks", _ALIF_NO_BLOCKS)
    assert failures
    msgs = failures[0][1]
    assert len(msgs) == 2
    joined = "\n".join(msgs)
    assert "on_module.hyperram" in joined
    assert "on_module.ospi_memories" in joined
    assert "memory.dram_mbit" in joined
    assert "memory.flash_mbit" in joined
    assert "is an Alif Ensemble part" in joined


@pytest.mark.parametrize("present,missing", [
    ("hyperram", "ospi_memories"),
    ("ospi_memories", "hyperram"),
])
def test_an_alif_ensemble_preset_must_declare_both_blocks(present, missing):
    """Declaring one and omitting the other must not buy a pass on the
    omitted figure."""
    blocks = {
        "hyperram": (
            "  hyperram:\n"
            "    chip:           W958D8NBYA5I\n"
            "    assembled:      false\n"
            "    capacity_mbit:  256\n"
        ),
        "ospi_memories": (
            "  ospi_memories:\n"
            "    ospi0:\n"
            "      chip:           MX25UM25645GXDI00\n"
            "      assembled:      false\n"
            "      capacity_mbit:  256\n"
            "      role:           app_storage\n"
        ),
    }
    body = (
        "sku: E1M-AEN801\n"
        "silicon: alif:ensemble:e8\n"
        "on_module:\n"
        + blocks[present] +
        "memory:\n"
        "  dram_mbit:            0\n"
        "  flash_mbit:           0\n"
    )
    failures = _check(f"mem-alif-only-{present}", body)
    assert failures
    msgs = failures[0][1]
    assert len(msgs) == 1
    assert f"on_module.{missing}" in msgs[0]


def test_an_empty_ospi_memories_block_does_not_satisfy_the_requirement():
    """`ospi_memories: {}` binds nothing, so it must not read as a
    declaration -- an empty block is the same evasion with extra keystrokes."""
    body = (
        "sku: E1M-AEN801\n"
        "silicon: alif:ensemble:e8\n"
        "on_module:\n"
        "  hyperram:\n"
        "    chip:           W958D8NBYA5I\n"
        "    assembled:      false\n"
        "    capacity_mbit:  256\n"
        "  ospi_memories:    {}\n"
        "memory:\n"
        "  dram_mbit:            0\n"
        "  flash_mbit:           0\n"
    )
    failures = _check("mem-alif-empty-ospi", body)
    assert failures
    assert "on_module.ospi_memories" in failures[0][1][0]


def test_the_requirement_is_derived_from_the_silicon_ref_not_a_sku_list():
    """An E1M-AEN901 that does not exist yet is covered the day it lands:
    the trigger is `silicon: alif:ensemble:*`, never the SKU string."""
    body = _ALIF_NO_BLOCKS.replace("sku: E1M-AEN801", "sku: E1M-AEN901")
    body = body.replace("alif:ensemble:e8", "alif:ensemble:e9")
    failures = _check("mem-alif-future-sku", body)
    assert failures
    assert "silicon='alif:ensemble:e9'" in failures[0][1][0]


@pytest.mark.parametrize("silicon", [
    "renesas:rzv2n:n44", "nxp:imx9:imx93", "deepx:dx:m1",
])
def test_a_non_ensemble_preset_may_still_omit_the_blocks(silicon):
    """The control side: V2N/V2M/NX9101 declare neither block by design and
    must keep skipping, not start failing."""
    body = (
        f"silicon: {silicon}\n"
        "on_module:\n"
        f"  silicon:              {silicon}\n"
        "memory:\n"
        "  dram_mbit:            32768\n"
        "  flash_mbit:           32768\n"
    )
    assert not _check(f"mem-non-alif-{silicon.replace(':', '-')}", body)


def test_every_real_alif_ensemble_preset_is_in_scope_of_the_requirement():
    """Guards the derivation itself: were the `silicon:` spelling to drift,
    all six AEN presets would silently fall OUT of the requirement.  This
    goes red instead of the requirement quietly covering nothing."""
    import yaml

    vm = _load_vm()
    for sku in AEN_PRESETS:
        doc = yaml.safe_load(
            (REPO / "metadata" / "e1m_modules" / f"{sku}.yaml").read_text(
                encoding="utf-8"))
        assert str(doc["silicon"]).startswith(vm._ALIF_ENSEMBLE_SILICON_PREFIX), (
            f"{sku}: silicon={doc['silicon']!r} no longer matches "
            f"{vm._ALIF_ENSEMBLE_SILICON_PREFIX!r}, so the mandatory-blocks "
            f"rule would no longer apply to it"
        )


# --- the real shipped presets, not just synthetic fixtures -------------

@pytest.mark.parametrize("sku", AEN_PRESETS)
def test_real_aen_preset_derivation_holds(sku):
    vm = _load_vm()
    real = REPO / "metadata" / "e1m_modules" / f"{sku}.yaml"
    assert real.is_file()
    assert not vm._check_som_memory_population([real])


@pytest.mark.parametrize("sku", AEN_PRESETS)
def test_real_aen_preset_is_actually_bound(sku):
    """A check that silently skips every real preset is not a check.  Assert
    both figures are genuinely under the rule on all six AEN SKUs."""
    import yaml

    doc = yaml.safe_load(
        (REPO / "metadata" / "e1m_modules" / f"{sku}.yaml").read_text(encoding="utf-8"))
    on_module = doc["on_module"]
    assert isinstance(on_module.get("hyperram"), dict)
    assert isinstance(on_module.get("ospi_memories"), dict)
    assert on_module["ospi_memories"]
    assert doc["memory"]["dram_mbit"] == 0
    assert doc["memory"]["flash_mbit"] == 0


def test_every_som_preset_passes_the_check():
    vm = _load_vm()
    presets = sorted((REPO / "metadata" / "e1m_modules").glob("E1M-*.yaml"))
    assert len(presets) >= 11
    assert not vm._check_som_memory_population(presets)
