# SPDX-License-Identifier: Apache-2.0
"""`metadata/socs/**/*.json` -> `npu_toolchain.vela` -- the Ethos-U memory profile.

Vela invoked with neither `--system-config` nor `--memory-mode` falls back to
`Ethos_U85_SYS_DRAM_Mid` / `Dedicated_Sram_384KB`, a DRAM-backed profile, and
reports the whole working set in DRAM -- `sram_memory_used = 0.0`.  On an Alif
Ensemble part that is a lie about the silicon: `metadata/socs/alif/ensemble/
e8.json`'s `external_memory_interfaces` lists only `HexSPI` and `SD/eMMC`, so
there is no DRAM for the arena to live in.  A zero SRAM figure then passes
alp-sdk's own on-device fit gate against ANY arena
(`src/backends/inference/alp_model_select.c`:
`return e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib;`).

`npu_toolchain.vela` publishes the profile as the SILICON fact it is, next to
the `npus[]` array that already says which accelerator is on the die, so a
consumer (`tan model build`) derives `--memory-mode` from the SKU instead of
inheriting vela's default.  These tests pin the invariants that make the block
safe to consume; `scripts/validate_metadata.py`'s `_check_soc_vela_memory_profile`
enforces the same set inside the metadata gate.
"""
import json
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"

#: `[Memory_Mode.*]` section names shipped in Arm's own vela.ini, verbatim
#: (ethos-u-vela 5.1.0, `<venv>/lib/python3.12/site-packages/ethosu/
#: config_files/Arm/vela.ini`).  A memory_mode outside this set exists only in
#: a proprietary vendor config -- and memory_mode is the flag that fixes the
#: footprint, so it has to work for a customer who has no vendor config at all.
_BUILTIN_MEMORY_MODES = {
    "Sram_Only",
    "Shared_Sram",
    "Dedicated_Sram",
    "Dedicated_Sram_256KB",
    "Dedicated_Sram_384KB",
    "Dedicated_Sram_512KB",
}

#: The subset of the above whose `arena_mem_area` is `Axi1` -- the non-SRAM
#: memory, which that same vela.ini documents as "assumed to be
#: read-writeable".  Read-writeable non-SRAM means DRAM: these four modes
#: cannot be honoured by a part that declares no DRAM interface.  This is the
#: exact class vela's own default (`Dedicated_Sram_384KB`) falls into.
_DRAM_BACKED_MEMORY_MODES = {
    "Dedicated_Sram",
    "Dedicated_Sram_256KB",
    "Dedicated_Sram_384KB",
    "Dedicated_Sram_512KB",
}

#: `[System_Config.*]` section names shipped in the same Arm vela.ini.
#: Anything else (Alif's `Ethos_U85_SRAM_Only`, `RTSS_HE_SRAM_Only`) lives only
#: in the proprietary `ensemble_vela.ini`; passing one without `--config` is a
#: hard vela rc=1 -- "Section System_Config.<name> not found in Vela config
#: file" -- not a degradation.
_BUILTIN_SYSTEM_CONFIGS = {
    "Ethos_U55_Deep_Embedded",
    "Ethos_U55_High_End_Embedded",
    "Ethos_U65_Embedded",
    "Ethos_U65_Mid_End",
    "Ethos_U65_High_End",
    "Ethos_U65_Client_Server",
    "Ethos_U85_SYS_Flash_Low",
    "Ethos_U85_SYS_Flash_High",
    "Ethos_U85_SYS_DRAM_Low",
    "Ethos_U85_SYS_DRAM_Mid",
    "Ethos_U85_SYS_DRAM_High",
}


def _all_socs():
    for path in sorted(_META.glob("socs/**/*.json")):
        yield path, json.loads(path.read_text(encoding="utf-8"))


def _has_ethos_u(spec: dict) -> bool:
    return any(str(n.get("type", "")).startswith("ethos-u")
               for n in spec.get("npus", []))


def _socs_with_ethos_u():
    return [(p, s) for p, s in _all_socs() if _has_ethos_u(s)]


def _vela(spec: dict) -> dict:
    return spec.get("npu_toolchain", {}).get("vela", {})


def _has_dram(spec: dict) -> bool:
    """True when the SoC declares a DRAM-class external memory interface.

    Matched on `DDR` appearing in the interface `kind` so every spelling in
    the tree resolves (`LPDDR4/4X`, `LPDDR4X / LPDDR5`) without a hand-kept
    allow-list that a new part could fall outside of.  Flash-class interfaces
    (`OctalSPI`, `HexSPI`, `xSPI`, `FlexSPI`, `SD/eMMC`) are not DRAM.
    """
    return any("DDR" in str(e.get("kind", "")).upper()
               for e in spec.get("external_memory_interfaces", []))


def _accelerator_identities(spec: dict) -> set:
    """The distinct `(type, subtype)` Ethos-U descriptions on this SoC.

    The E8 carries three (`ethos-u85/generative`, `ethos-u55/high-perf`,
    `ethos-u55/high-efficiency`); the i.MX 93 carries one.
    """
    return {(n.get("type"), n.get("subtype"))
            for n in spec.get("npus", [])
            if str(n.get("type", "")).startswith("ethos-u")}


def _rel(path: Path) -> str:
    return path.relative_to(_ROOT).as_posix()


def test_at_least_one_soc_declares_an_ethos_u_npu():
    # Guards every other test in this file against silently passing on an
    # empty set if the glob or the `ethos-u` prefix ever stops matching.
    assert _socs_with_ethos_u(), "no metadata/socs/**/*.json declares an ethos-u* NPU"


def test_every_ethos_u_soc_declares_a_vela_memory_mode():
    missing = [_rel(p) for p, spec in _socs_with_ethos_u()
               if "memory_mode" not in _vela(spec)]
    assert not missing, (
        f"SoCs with an Ethos-U NPU but no npu_toolchain.vela.memory_mode: {missing}. "
        "Without it tan compiles against vela's DRAM-backed default."
    )


def test_declared_memory_modes_are_arm_builtins():
    # memory_mode must never need the proprietary .ini -- it is the flag that
    # fixes the footprint, so it has to work for an unlicensed customer.
    for path, spec in _socs_with_ethos_u():
        mode = _vela(spec)["memory_mode"]
        assert mode in _BUILTIN_MEMORY_MODES, (
            f"{_rel(path)} declares memory_mode {mode!r}, which is not an Arm built-in "
            f"({sorted(_BUILTIN_MEMORY_MODES)}); it would need a vendor config."
        )


def test_no_soc_without_dram_declares_a_dram_backed_memory_mode():
    # The bug this whole block exists to kill: vela's default
    # Dedicated_Sram_384KB places the arena in read-writeable non-SRAM, which
    # the Alif Ensemble parts do not have.
    for path, spec in _socs_with_ethos_u():
        mode = _vela(spec)["memory_mode"]
        if mode in _DRAM_BACKED_MEMORY_MODES and not _has_dram(spec):
            kinds = [e.get("kind") for e in spec.get("external_memory_interfaces", [])]
            pytest.fail(
                f"{_rel(path)} declares memory_mode {mode!r}, which places the tensor "
                f"arena in read-writeable non-SRAM, but its external_memory_interfaces "
                f"are {kinds} -- no DRAM. Vela would report the working set in a memory "
                f"the silicon does not have."
            )


def test_a_vendor_system_config_is_flagged_as_needing_a_vendor_config():
    for path, spec in _socs_with_ethos_u():
        vela = _vela(spec)
        if "system_config" not in vela:
            continue
        if vela["system_config"] in _BUILTIN_SYSTEM_CONFIGS:
            continue
        assert vela.get("system_config_requires_vendor_config") is True, (
            f"{_rel(path)} names system_config {vela['system_config']!r} without "
            "system_config_requires_vendor_config: true -- tan would pass an "
            "undefined section name and vela would exit 1."
        )
        assert vela.get("vendor_config_filename"), (
            f"{_rel(path)} requires a vendor config but does not name the file."
        )


def test_a_profile_that_requires_a_vendor_config_names_the_file():
    # Independent of `system_config`: a consumer that knows a vendor config is
    # required still has to be able to tell a customer WHICH file, and
    # `tan model doctor` reports it as an optional prerequisite by name.
    for path, spec in _socs_with_ethos_u():
        vela = _vela(spec)
        if vela.get("system_config_requires_vendor_config") is not True:
            continue
        filename = vela.get("vendor_config_filename")
        assert filename, (
            f"{_rel(path)} sets system_config_requires_vendor_config: true but does "
            "not name the file a customer would have to supply."
        )
        assert "/" not in filename and "\\" not in filename, (
            f"{_rel(path)} declares vendor_config_filename {filename!r} -- it must be "
            "a basename. Where the file lives is environment, not silicon, and a "
            "committed absolute path is a leak."
        )


def test_a_scalar_system_config_is_only_declared_on_a_single_accelerator_soc():
    # A vela System_Config describes ONE accelerator's (or one core
    # subsystem's) memory view, not a die.  The E8 sources two different ones
    # -- `Ethos_U85_SRAM_Only` for its U85 (examples/aen/
    # aen-npu-inference-alp/CMakeLists.txt:42-43) and `RTSS_HE_SRAM_Only` for
    # its M55-HE-paired U55 (examples/aen/aen-npu-inference-alp-u55/
    # CMakeLists.txt:39-40) -- so one scalar on that SoC would be right for one
    # accelerator and wrong for the other.
    for path, spec in _socs_with_ethos_u():
        if "system_config" not in _vela(spec):
            continue
        ids = _accelerator_identities(spec)
        assert len(ids) == 1, (
            f"{_rel(path)} declares a single system_config but carries "
            f"{len(ids)} distinct Ethos-U accelerators {sorted(map(str, ids))}. "
            "A System_Config is per-accelerator (Alif's are per-core subsystem: "
            "RTSS_HE_SRAM_Only); one scalar cannot describe them all."
        )


def test_every_vela_profile_cites_a_source_that_states_its_memory_mode():
    # Grounding, not decoration: the cited line range must actually contain
    # the memory_mode string. A stale or off-by-one citation fails here rather
    # than silently outliving the fact it was supposed to prove.  Checked per
    # citation, never over their union: a SoC that cites two files would
    # otherwise let a live citation mask a dead one.
    for path, spec in _socs_with_ethos_u():
        vela = _vela(spec)
        source = vela.get("source")
        assert source, f"{_rel(path)} declares npu_toolchain.vela with no source"
        for citation in source.split("; "):
            rel, _, span = citation.rpartition(":")
            start, _, end = span.partition("-")
            cited = _ROOT / rel
            assert cited.is_file(), (
                f"{_rel(path)} cites {citation!r} but {rel} does not exist"
            )
            lines = cited.read_text(encoding="utf-8").splitlines()
            assert 1 <= int(start) <= int(end) <= len(lines), (
                f"{_rel(path)} cites {citation!r} but {rel} has {len(lines)} lines"
            )
            quoted = lines[int(start) - 1:int(end)]
            assert any(vela["memory_mode"] in line for line in quoted), (
                f"{_rel(path)} declares memory_mode {vela['memory_mode']!r} but no line "
                f"in cited range {citation!r} says so."
            )


def test_a_soc_with_no_ethos_u_npu_declares_no_vela_profile():
    # A vela profile on a DRP-AI / DEEPX part would be metadata nobody can act
    # on -- vela does not compile for those accelerators.
    stray = [_rel(p) for p, spec in _all_socs()
             if not _has_ethos_u(spec) and _vela(spec)]
    assert not stray, (
        f"SoCs with npu_toolchain.vela but no Ethos-U NPU: {stray}"
    )
