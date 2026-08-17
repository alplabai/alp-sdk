# Vela Memory-Profile Sourcing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Make `tan model build` compile every Ethos-U model against the memory
model the SoM actually has, sourced from alp-sdk metadata, so the arena/SRAM
figures a board trusts describe real silicon.

**Architecture:** The vela memory profile is a *silicon* fact, not a customer
choice. It lands in alp-sdk metadata beside the `ethos_u_variant` that already
lives there, and `tan.model.adapters.ethos_u` derives `--memory-mode` (and
`--system-config` when one is available) from the SKU. The proprietary
`ensemble_vela.ini` becomes an *optional enhancement* supplied through the
environment, never through `board.yaml`.

**Tech Stack:** Python 3.12 (`python/tan/`), YAML/JSON metadata under
`alp-sdk/metadata/`, `jsonschema` Draft 2020-12, `ethos-u-vela` 5.1.0.

## Why this plan exists

`tan model build` currently invokes vela with neither `--system-config` nor
`--memory-mode` (`python/tan/model/adapters/ethos_u.py:362`, `cmd = ["vela",
str(source), "--accelerator-config", accel_config, ...]`). At
`--accelerator-config ethos-u85-256` vela therefore falls back to
`Ethos_U85_SYS_DRAM_Mid` / `Dedicated_Sram_384KB` — a DRAM-backed profile — and
reports the working set in DRAM. `E1M-AEN801` is an Alif Ensemble E8:
`metadata/socs/alif/ensemble/e8.json`'s `external_memory_interfaces` lists only
`HexSPI` and `SD/eMMC`. **There is no DRAM on the part.**

(That default is the U85's. The same SoM's two U55s take a different flagless
default and fail differently — see the per-accelerator table below; do not
carry this paragraph's `system_config` name over to them.)

That produced `req_sram_kib = 0`, which alp-sdk's on-device selector
(`src/backends/inference/alp_model_select.c:88`,
`return e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib;`)
accepts against ANY arena. tan-cli#789 closed the hole by *refusing* such a
target. This plan removes the need to refuse.

## Measured facts this plan is built on

Every number below was produced by running `ethos-u-vela 5.1.0` on the committed
`python/tests/fixtures/models/tiny_int8.tflite` — **712 B, md5
`07290259e28e1467fa184ef01b71e40a`**, byte-identical to alp-sdk's
`tests/fixtures/models/tiny_int8.tflite` — at `ethos-u85-256`. Re-measure
rather than trusting this table if vela's version moves. Pin the fixture by
hash when you do: row 1 originally read `5.359375`, which is a real vela
figure for a DIFFERENT committed fixture (`keyword_scrambled_8bit.tflite`,
29632 B, md5 `4e22b1bbb5f1a0eea728ec15d481ff7e`, same accelerator config) and
travelled from the changelog into this table before anyone re-ran it.

| Invocation | rc | `sram_memory_used` | `dram_memory_used` | `on_chip_flash_memory_used` | `off_chip_flash_memory_used` |
|---|---|---|---|---|---|
| no profile flags (today) | 0 | `0.0` | `0.265625` | `0.0` | `0.0` |
| `--memory-mode Sram_Only` | 0 | `0.03125` | `0.0` | `0.234375` | `0.0` |
| `--system-config Ethos_U85_SYS_Flash_High --memory-mode Sram_Only` | 0 | `0.03125` | `0.0` | `0.234375` | `0.0` |
| `--system-config Ethos_U85_SRAM_Only` (no `--config`) | **1** | — | — | — | — |

Every other cell above re-measured clean; only row 1's `dram_memory_used` was
wrong (and its `on_chip_flash_memory_used`, which is `0.0`, not unreported).

**The flagless default is not the same on every accelerator, so do not
generalise row 1 beyond the U85.** Same fixture, same vela, no flags:

| `--accelerator-config` | flagless `system_config` / `memory_mode` | `sram` | `dram` | `on_chip_flash` | `off_chip_flash` |
|---|---|---|---|---|---|
| `ethos-u85-256` | `Ethos_U85_SYS_DRAM_Mid` / `Dedicated_Sram_384KB` | `0.0` | `0.265625` | `0.0` | `0.0` |
| `ethos-u65-256` | `Ethos_U65_Client_Server` / `Dedicated_Sram_384KB` | `0.0` | `0.109375` | `0.0` | `0.0` |
| `ethos-u55-256` | `Ethos_U55_High_End_Embedded` / `Shared_Sram` | `0.03125` | `0.0` | `0.0` | `0.078125` |
| `ethos-u55-128` | `Ethos_U55_High_End_Embedded` / `Shared_Sram` | `0.03125` | `0.0` | `0.0` | `0.078125` |

Only the U85 and U65 defaults are DRAM-backed and report the `sram = 0.0` this
plan exists to fix. The U55 default reports a non-zero SRAM figure already; its
hazard is that the weights land in `off_chip_flash` — the OSPI0 NOR, which no
AEN SKU populates (`assembled: false` on every preset). With the declared mode:

| `--accelerator-config` | declared `memory_mode` | `sram` | `dram` | `on_chip_flash` | `off_chip_flash` |
|---|---|---|---|---|---|
| `ethos-u85-256` | `Sram_Only` | `0.03125` | `0.0` | `0.234375` | `0.0` |
| `ethos-u55-256` | `Sram_Only` | `0.03125` | `0.0` | `0.078125` | `0.0` |
| `ethos-u55-128` | `Sram_Only` | `0.03125` | `0.0` | `0.078125` | `0.0` |
| `ethos-u65-256` | `Shared_Sram` | `0.03125` | `0.078125` | `0.0` | `0.0` |

The rc=1 row of the first table — `--system-config Ethos_U85_SRAM_Only` with no
`--config` — fails with, verbatim:

```
ethosu.vela.errors.CliOptionError: 'Error: Incorrect argument to CLI option --system-config=Ethos_U85_SRAM_Only: Section System_Config.Ethos_U85_SRAM_Only not found in Vela config file'
```

**Three conclusions, and the whole design follows from them:**

1. **`--memory-mode` picks PORTS; `--system-config` maps ports to AREAS — so
   "system-config only decides bandwidth" is true under `Sram_Only` and FALSE
   under every other mode.** An earlier draft of this conclusion said it
   without the scope, on the strength of rows 2 and 3 above being identical.
   Both of those rows are `Sram_Only`, which is exactly the case where the
   claim holds, so the evidence could not have distinguished the two readings.

   The mechanism is in Arm's own `vela.ini`. `[Memory_Mode.*]` assigns
   `const_mem_area` / `arena_mem_area` / `cache_mem_area` to a **port**
   (`Axi0` or `Axi1`), and `[System_Config.*]` maps those ports to a **memory
   area** via `axi0_port` / `axi1_port`:

   - `Sram_Only` puts all three on `Axi0`, and **every** Arm section sets
     `axi0_port=Sram`. The system config therefore cannot move placement —
     which is the only reason rows 2 and 3 matched.
   - `Shared_Sram` sets `const_mem_area=Axi1`; `Dedicated_Sram*` puts const
     **and** arena on `Axi1`. `axi1_port` is `OffChipFlash` on
     `Ethos_U55_*`, `Ethos_U65_Embedded` and `Ethos_U85_SYS_Flash_*`, and
     `Dram` on `Ethos_U65_{Mid_End,High_End,Client_Server}` and
     `Ethos_U85_SYS_DRAM_*`. So under those modes the system config decides
     where the weights physically land.

   Measured, `tests/fixtures/models/person_detect_int8.tflite` (300568 B, md5
   `ac6e1b872f90cff2f52b1a28664814ed`) at `ethos-u65-256 --memory-mode
   Shared_Sram`, changing **only** `--system-config` (KiB):

   | `--system-config` | `sram` | `dram` | `off_chip_flash` |
   |---|---|---|---|
   | `Ethos_U65_Embedded` | `72.734375` | `0.0` | `228.265625` |
   | `Ethos_U65_Mid_End` | `72.734375` | `228.3125` | `0.0` |
   | `Ethos_U65_High_End` | `72.734375` | `228.25` | `0.0` |
   | `Ethos_U65_Client_Server` | `72.734375` | `228.25` | `0.0` |

   228 KiB of weights moves between DRAM and off-chip flash on the system
   config alone. The same model at `ethos-u85-256 --memory-mode Shared_Sram`
   behaves identically: `Ethos_U85_SYS_DRAM_Mid` → `dram = 228.796875`,
   `Ethos_U85_SYS_Flash_High` → `off_chip_flash = 228.84375`. Under
   `--memory-mode Sram_Only` all four U65 sections collapse to one result
   (`sram = 72.0`, `on_chip_flash = 228.25`), confirming the scope.

   **This is not hypothetical for alp-sdk:** `Shared_Sram` is exactly what
   this plan has tan pass for `E1M-NX9101`, and `metadata/socs/nxp/imx9/imx93.json`
   declares no `system_config`, so that part currently inherits vela's default
   `Ethos_U65_Client_Server` and lands its const region in DRAM. Pinning a
   `system_config` for the i.MX 93 is open work this conclusion now surfaces.

   One further caveat, also measured: even under `Sram_Only`, where the system
   config cannot move placement, it still changes the **encoded weight**
   footprint. Same model at `ethos-u85-256 --memory-mode Sram_Only`,
   `sram_memory_used` is `72.0` for all five U85 sections — the arena the fit
   gate reads is invariant — but `on_chip_flash_memory_used` is `235.265625`
   for `Ethos_U85_SYS_{Flash_High,DRAM_Mid}` and `248.953125` for
   `Ethos_U85_SYS_{Flash_Low,DRAM_Low,DRAM_High}`. Only the arena figure is
   safe to call system-config-independent.

   What survives unchanged: passing `--memory-mode` alone is what fixes the
   zero-SRAM figure the fit gate reads.
2. **`Memory_Mode.Sram_Only` is a vela BUILT-IN.** It needs no proprietary file.
   vela 5.1.0 ships `Sram_Only`, `Shared_Sram`, `Dedicated_Sram`,
   `Dedicated_Sram_256KB`, `Dedicated_Sram_384KB`, `Dedicated_Sram_512KB`
   (`<venv>/lib/python3.12/site-packages/ethosu/config_files/Arm/vela.ini`).
3. **Only the vendor-tuned `System_Config` needs the `.ini`.** Arm's built-in
   System_Config sections are `Ethos_U55_Deep_Embedded`,
   `Ethos_U55_High_End_Embedded`, `Ethos_U65_Embedded`, `Ethos_U65_Mid_End`,
   `Ethos_U65_High_End`, `Ethos_U65_Client_Server`, `Ethos_U85_SYS_Flash_Low`,
   `Ethos_U85_SYS_Flash_High`, `Ethos_U85_SYS_DRAM_Low`,
   `Ethos_U85_SYS_DRAM_Mid`, `Ethos_U85_SYS_DRAM_High`. `Ethos_U85_SRAM_Only`
   and `RTSS_HE_SRAM_Only` are NOT among them.

Also measured (re-measured, and both figures hold): under `--memory-mode
Sram_Only` at `ethos-u85-256`, `arena_cache_size = 1073741824.0` (1 GiB); with
no flags at the same config it is `384.0`, tracking the `Dedicated_Sram_384KB`
default rather than the model. Either way it is a configured cache capacity,
never a model's arena — which is why tan-cli#789 stopped reading it.

### What alp-sdk already publishes (no invention required)

| Part | `--system-config` | `--memory-mode` | Needs `.ini`? | Source |
|---|---|---|---|---|
| Alif Ensemble, U85 | `Ethos_U85_SRAM_Only` | `Sram_Only` | **yes** | `examples/aen/aen-npu-inference-alp/CMakeLists.txt:42-43` |
| Alif Ensemble, U55 | `RTSS_HE_SRAM_Only` | `Sram_Only` | **yes** | `examples/aen/aen-npu-inference-alp-u55/CMakeLists.txt:39-40` |
| NXP i.MX 93, U65 | *(none given)* | `Shared_Sram` | **no** | `vendors/nxp-imx93/README.md` ⚠ |

⚠ The two Alif rows cite build files that pass the flag to a real toolchain.
The U65 row cites an **alp-sdk-authored** page, which derives `Shared_Sram`
from no named NXP or eIQ document — so it is alp-sdk's own default, not a
vendor-stated fact, and a citation test over it proves only that alp-sdk agrees
with itself. That page now carries a provenance note saying so. Grounding the
U65 mode in an NXP primary document is open work; do not upgrade this row's
confidence without one.

`examples/aen/aen-npu-inference-alif/CMakeLists.txt:85-89` states the constraint
in the repo's own words: pass `--system-config`/`--memory-mode` *"ONLY when that
config is supplied, else Vela errors 'Section … not found' against its built-in
vela.ini."*

## Global Constraints

- **Never invent a hardware value.** A profile that is not sourced from alp-sdk
  metadata or vendor documentation is marked TBD and simply not passed. A wrong
  profile compiles firmware for the wrong machine.
- **`board.yaml` is NOT the home for any of this.** `metadata/schemas/board.schema.json`
  defines `models[].compile` as, verbatim, *"Per-backend compile configuration
  for NPU toolchains that need a per-model config + calibration **the SDK cannot
  derive** (DRP-AI, DEEPX)."* A vela profile IS derivable from the SKU, so
  putting it there would duplicate a fact `metadata/` owns and repeat the
  mistake that removed `inference.backend` from board.yaml v2.
- **Never put a local absolute path in committed metadata or `board.yaml`.**
  The `.ini` location is environment, not hardware.
- **A `--system-config` is only ever passed alongside a `--config` that defines
  it, or when it is one of Arm's built-ins.** Passing an undefined section name
  is a hard vela failure (rc=1), not a degradation.
- **Zero failures is the gate, never a pinned pass count.** Test counts on this
  project swing with `zsh` presence, built binaries and the `model-io` extra.
- alp-sdk changelog fragments are `changelog.d/<issue>.md`, DIGITS ONLY.
  tan-cli uses `changelog.d/<issue>.<kind>.md`.
- No AI/Claude attribution. "Alp Lab", never "ALP Lab".

## File Structure

**alp-sdk** (the facts):
- Modify `metadata/socs/alif/ensemble/e{3,4,5,6,7,8}.json` — add `npu_toolchain.vela`.
- Modify `metadata/socs/nxp/imx9/imx93.json` — same block, `Shared_Sram`.
- Modify `metadata/schemas/soc-spec-v1.schema.json` — define and constrain the block.
- Modify `scripts/validate_metadata.py` — cross-check the block's semantics.
- Create `tests/scripts/test_vela_profile_metadata.py`.

**tan-cli** (the consumer):
- Modify `python/tan/model/targets.py` — carry the profile onto `TargetSpec`.
- Modify `python/tan/model/adapters/ethos_u.py` — pass the flags; re-source the
  refusal from metadata.
- Modify `python/tan/core/model_doctor.py` — report the optional `.ini`.
- Modify `python/tests/model/test_targets.py`, `test_adapters.py`, `test_build.py`.

---

## Task 1: alp-sdk — carry the vela profile in SoC metadata

**Files:**
- Modify: `metadata/socs/alif/ensemble/e8.json` (and `e3`–`e7`)
- Modify: `metadata/socs/nxp/imx9/imx93.json`
- Modify: `metadata/schemas/soc-spec.schema.json`
- Test: `tests/scripts/test_vela_profile_metadata.py`

**Interfaces:**
- Produces: a `npu_toolchain.vela` object on each SoC spec that declares an
  Ethos-U NPU, consumed by tan Task 2 via `resolve_targets`.

The block, on `e8.json`:

```json
"npu_toolchain": {
  "vela": {
    "memory_mode": "Sram_Only",
    "system_config_requires_vendor_config": true,
    "vendor_config_filename": "ensemble_vela.ini",
    "source": "examples/aen/aen-npu-inference-alp/CMakeLists.txt:42-43"
  }
}
```

> **CORRECTION (post-execution, 2026-08-16).** An earlier draft of this block
> carried a scalar `"system_config": "Ethos_U85_SRAM_Only"`. That was **wrong**
> and is deliberately absent above. An Alif `System_Config` is **per core
> subsystem, not per SoC**: `examples/aen/aen-npu-inference-alp-u55/CMakeLists.txt:36-38`
> states the `ensemble_vela.ini` has no Ethos_U55-specific section and that the
> per-core `RTSS_HE` config describes the U55's memory (`axi0_port=Sram`). One
> Ensemble die therefore sources BOTH `Ethos_U85_SRAM_Only` (for its U85) and
> `RTSS_HE_SRAM_Only` (for its M55-HE U55) — e4/e6/e8 carry three Ethos-U
> accelerators and e3/e5/e7 carry two, so **no Ensemble part has only one**. A
> scalar would be correct for one accelerator and silently wrong for the rest,
> and Task 5 would hand that wrong name to `--config`. Expressing it properly
> needs a per-accelerator keyed shape and a consumer; that is an open item, not
> this task. Task 2 is unaffected — it populates `vela_system_config` only when
> `system_config_requires_vendor_config` is falsy, which yields `None` for every
> Alif part either way.

On `imx93.json` — no vendor config, so no `system_config` at all:

```json
"npu_toolchain": {
  "vela": {
    "memory_mode": "Shared_Sram",
    "system_config_requires_vendor_config": false,
    "source": "vendors/nxp-imx93/README.md"
  }
}
```

- [ ] **Step 1: failing test** — `tests/scripts/test_vela_profile_metadata.py`:

```python
import json
from pathlib import Path

import pytest

_META = Path(__file__).resolve().parents[2] / "metadata"

# Arm's own vela.ini sections, verbatim (ethos-u-vela 5.1.0). A memory_mode
# outside this set is only legal when a vendor config supplies it.
_BUILTIN_MEMORY_MODES = {
    "Sram_Only", "Shared_Sram", "Dedicated_Sram",
    "Dedicated_Sram_256KB", "Dedicated_Sram_384KB", "Dedicated_Sram_512KB",
}


def _socs_with_ethos_u():
    for p in sorted(_META.glob("socs/**/*.json")):
        spec = json.loads(p.read_text(encoding="utf-8"))
        if any(str(n.get("type", "")).startswith("ethos-u") for n in spec.get("npus", [])):
            yield p, spec


def test_every_ethos_u_soc_declares_a_vela_memory_mode():
    missing = [p.name for p, spec in _socs_with_ethos_u()
               if "memory_mode" not in spec.get("npu_toolchain", {}).get("vela", {})]
    assert not missing, (
        f"SoCs with an Ethos-U NPU but no npu_toolchain.vela.memory_mode: {missing}. "
        "Without it tan compiles against vela's DRAM-backed default."
    )


def test_declared_memory_modes_are_arm_builtins():
    # memory_mode must never need the proprietary .ini -- it is the flag that
    # fixes the footprint, so it has to work for an unlicensed customer.
    for p, spec in _socs_with_ethos_u():
        mode = spec["npu_toolchain"]["vela"]["memory_mode"]
        assert mode in _BUILTIN_MEMORY_MODES, (
            f"{p.name} declares memory_mode {mode!r}, which is not an Arm built-in "
            f"({sorted(_BUILTIN_MEMORY_MODES)}); it would need a vendor config."
        )


def test_a_vendor_system_config_is_flagged_as_needing_a_vendor_config():
    for p, spec in _socs_with_ethos_u():
        vela = spec["npu_toolchain"]["vela"]
        if "system_config" in vela:
            assert vela.get("system_config_requires_vendor_config") is True, (
                f"{p.name} names system_config {vela['system_config']!r} without "
                "system_config_requires_vendor_config: true -- tan would pass an "
                "undefined section name and vela would exit 1."
            )
            assert vela.get("vendor_config_filename"), (
                f"{p.name} requires a vendor config but does not name the file."
            )
```

- [ ] **Step 2:** run `python3 -m pytest tests/scripts/test_vela_profile_metadata.py -q`.
      Expected: FAIL — no SoC declares `npu_toolchain`.
- [ ] **Step 3:** add the `npu_toolchain.vela` block to every SoC spec that
      declares an `ethos-u*` NPU. Derive each `memory_mode` from the sources in
      the table; where no source exists for a part, STOP and report it rather
      than guessing.
- [ ] **Step 4:** extend `metadata/schemas/soc-spec.schema.json` with the object
      (`additionalProperties: false`; `memory_mode` required; `system_config`
      optional; `system_config_requires_vendor_config` boolean).
- [ ] **Step 5:** run the tests again — expect PASS — then
      `python3 scripts/validate_metadata.py` (rc must be 0) and
      `python3 scripts/gen_catalog.py` (must produce no drift).
- [ ] **Step 6:** commit. Fragment `changelog.d/1470.md` (digits only).

## Task 2: tan — carry the profile onto `TargetSpec`

**Files:**
- Modify: `python/tan/model/targets.py:20-23` (`TargetSpec`), `:64` (`resolve_targets`)
- Test: `python/tests/model/test_targets.py`

**Interfaces:**
- Consumes: Task 1's `npu_toolchain.vela`.
- Produces: `TargetSpec.vela_memory_mode: str | None` and
  `TargetSpec.vela_system_config: str | None`, read by Task 3.

`TargetSpec` today is exactly:

```python
@dataclass(frozen=True)
class TargetSpec:
    backend: str            # cpu | ethos_u | drpai | deepx_dxm1
    silicon_ref: str        # SoC ref e.g. "alif:ensemble:e7" | "deepx:dx:m1" | "*"
    accel_config: str       # vela accel-config e.g. "ethos-u55-256"; "" when N/A
```

Add two optional fields with `None` defaults so no existing construction site
breaks:

```python
    vela_memory_mode: str | None = None      # Arm built-in, e.g. "Sram_Only"
    vela_system_config: str | None = None    # ONLY when it needs no vendor config
```

`vela_system_config` is populated **only** when the SoC's block does NOT set
`system_config_requires_vendor_config: true`. A vendor-tuned name must never
reach the command line without its `.ini` — that is a hard vela rc=1.

- [ ] **Step 1: failing test** in `python/tests/model/test_targets.py`:

```python
def test_an_alif_ethos_u_target_carries_the_builtin_memory_mode_but_not_the_vendor_system_config():
    specs = resolve_targets("E1M-AEN801", metadata_root=_META)
    u85 = [s for s in specs if s.accel_config == "ethos-u85-256"]
    assert u85, "E1M-AEN801 must resolve an ethos-u85-256 target"
    assert u85[0].vela_memory_mode == "Sram_Only"
    # Ethos_U85_SRAM_Only lives only in the proprietary ensemble_vela.ini;
    # passing it without --config is vela rc=1 "Section ... not found".
    assert u85[0].vela_system_config is None


def test_an_nxp_ethos_u_target_carries_its_own_memory_mode():
    specs = resolve_targets("E1M-NX9101", metadata_root=_META)
    u65 = [s for s in specs if s.accel_config == "ethos-u65-256"]
    assert u65, "E1M-NX9101 must resolve an ethos-u65-256 target"
    assert u65[0].vela_memory_mode == "Shared_Sram"
```

- [ ] **Step 2:** run `python -m pytest tests/model/test_targets.py -q` with
      `ALP_SDK_ROOT` bound. Expected: FAIL — `TargetSpec` has no such attribute.
- [ ] **Step 3:** add the fields; populate them in `resolve_targets` from the
      SoC spec already loaded at `targets.py:76-82`, guarding the vendor case.
- [ ] **Step 4:** run again — expect PASS.
- [ ] **Step 5:** commit.

## Task 3: tan — pass the flags to vela

**Files:**
- Modify: `python/tan/model/adapters/ethos_u.py:362` (the `cmd` list), `:359`
  (`VelaAdapter.compile` signature), `python/tan/model/build.py`
- Test: `python/tests/model/test_adapters.py`, `python/tests/model/test_build.py`

**Interfaces:**
- Consumes: Task 2's `TargetSpec.vela_memory_mode` / `.vela_system_config`.
- Produces: a `Blob` whose `req_sram_kib` is non-zero for a real NPU placement,
  which removes the tan-cli#789 refusal for the default case.

`compile()` already takes `silicon_ref: str | None = None` (added by tan-cli#789
finding (g), commit `ea1f02b`). Extend the same way — optional kwargs, no
existing call site broken:

```python
    def compile(self, source: Path, *, accel_config: str, out_dir: Path,
                opts: dict | None = None, silicon_ref: str | None = None,
                vela_memory_mode: str | None = None,
                vela_system_config: str | None = None) -> Blob:
        run_dir = _run_dir(out_dir, accel_config)
        cmd = ["vela", str(source), "--accelerator-config", accel_config,
               "--output-dir", str(run_dir)]
        # --memory-mode picks the PORT each area sits on; --system-config
        # maps ports to memory areas. Under Sram_Only (all areas on Axi0,
        # axi0_port=Sram everywhere) the system config cannot move
        # placement -- but under Shared_Sram / Dedicated_Sram* it moves the
        # const region between DRAM and off-chip flash, so it is NOT
        # cosmetic there (see "Three conclusions" #1). The memory-mode is
        # what makes the fit gate's SRAM figure non-zero, and every value we
        # emit is an Arm built-in, so this works with no vendor .ini.
        if vela_memory_mode:
            cmd += ["--memory-mode", vela_memory_mode]
        if vela_system_config:
            cmd += ["--system-config", vela_system_config]
```

`build_model` passes `spec.vela_memory_mode` / `spec.vela_system_config`
alongside the `spec.silicon_ref` it already passes.

- [ ] **Step 1: failing test** — assert the command line, and assert the real
      outcome with vela installed:

```python
def test_compile_passes_the_targets_memory_mode_to_vela(monkeypatch, tmp_path):
    seen = {}
    monkeypatch.setattr(subprocess, "run", _fake_vela(seen))
    VelaAdapter().compile(_TINY, accel_config="ethos-u85-256", out_dir=tmp_path,
                          vela_memory_mode="Sram_Only")
    assert "--memory-mode" in seen["cmd"]
    assert seen["cmd"][seen["cmd"].index("--memory-mode") + 1] == "Sram_Only"


def test_a_vendor_system_config_is_never_put_on_the_command_line_alone(monkeypatch, tmp_path):
    # Ethos_U85_SRAM_Only without --config is a hard vela rc=1:
    # "Section System_Config.Ethos_U85_SRAM_Only not found in Vela config file"
    seen = {}
    monkeypatch.setattr(subprocess, "run", _fake_vela(seen))
    VelaAdapter().compile(_TINY, accel_config="ethos-u85-256", out_dir=tmp_path,
                          vela_memory_mode="Sram_Only", vela_system_config=None)
    assert "--system-config" not in seen["cmd"]


@pytest.mark.skipif(shutil.which("vela") is None, reason="needs ethos-u-vela")
def test_real_vela_with_the_soms_memory_mode_reports_a_nonzero_sram_footprint(tmp_path):
    # The whole point: 0 KiB SRAM is what defeated the on-device fit gate.
    blob = VelaAdapter().compile(_TINY, accel_config="ethos-u85-256",
                                 out_dir=tmp_path, vela_memory_mode="Sram_Only")
    assert blob.req_sram_kib > 0
    assert blob.arena_bytes > 0
```

- [ ] **Step 2:** run — expect FAIL (unexpected keyword argument).
- [ ] **Step 3:** implement the signature + `cmd` extension + `build_model` wiring.
- [ ] **Step 4:** run — expect PASS. Then drive a REAL end-to-end build and
      confirm the tan-cli#789 refusal no longer fires for `E1M-AEN801`:
      `build_model(sku="E1M-AEN801", name="tiny", source=<tiny_int8.tflite>,
      out_dir=..., metadata_root=<alp-sdk>/metadata)` must now emit an
      `ethos-u85-256` **target**, not a `skipped` coverage row.
- [ ] **Step 5:** commit. Fragment `changelog.d/789.fixed.md`.

## Task 4: tan — re-source the refusal from metadata

**Files:**
- Modify: `python/tan/model/adapters/ethos_u.py` (`_refusal_remedy`, `_refuse_zero_sram_footprint`)
- Test: `python/tests/model/test_adapters.py`

The refusal must survive — a profile can still be missing for a part marked TBD
— but its *evidence* should come from metadata rather than a hardcoded vendor
sentence. `e8.json`'s `external_memory_interfaces` lists only `HexSPI` and
`SD/eMMC`, so "vela placed the working set in DRAM and this SoC declares no DRAM
interface" is machine-checkable and correct for every part automatically.

- [ ] **Step 1: failing test** — a refusal for a SoC with no DRAM interface
      names that fact; a refusal never names a vendor file for a part whose
      metadata does not declare one.
- [ ] **Step 2-4:** implement, verify.
- [ ] **Step 5:** confirm the tan-cli#789 non-regressions still hold — the word
      `fits` appears in no `basis: static-screen` output; the note stays one
      line inside `_VELA_REFUSAL_NOTE_BUDGET = 700`; a refusal costs ONE target,
      never the package.
- [ ] **Step 6:** commit.

## Task 5: tan — the optional vendor `.ini`

**Files:**
- Modify: `python/tan/model/adapters/ethos_u.py`, `python/tan/core/model_doctor.py`
- Test: `python/tests/model/test_adapters.py`, `python/tests/commands/test_model_command.py`

A licensed customer with `ensemble_vela.ini` should get the vendor-tuned
profile. The path is environment, not hardware, so it is read from an env var —
`ALP_VELA_CONFIG` — and NEVER from `board.yaml`.

When it IS set, `--config <path>` is passed and the SoC's vendor
`system_config` becomes legal to pass alongside it.

- [ ] **Step 1:** `tan model doctor` reports the `.ini` as an optional
      prerequisite, in the shape its existing rows use (`{backend, tool,
      available, version, reason}`), with an actionable `reason` when absent.
      It must read as OPTIONAL — an unlicensed customer is not broken, they
      simply get Arm's built-in profile.
- [ ] **Step 2:** `--config` + vendor `system_config` are passed together or not
      at all. Pin with a test that setting only one of them never reaches the
      command line.
- [ ] **Step 3:** commit.

---

## Open question for the maintainer

**Which Arm built-in `system_config` best matches each Alif part when no `.ini`
is present?** vela offers `Ethos_U85_SYS_Flash_Low` and `Ethos_U85_SYS_Flash_High`
(flash-backed, closest to the E8's MRAM) as well as the DRAM family. Choosing
between Low and High is a *bandwidth* claim this plan cannot source from any
document in the repo. Under the `Sram_Only` the Alif parts declare, it moves no
weights — all five U85 sections put every area on `Sram` — and the arena figure
the fit gate reads is identical (`sram_memory_used = 72.0` on
`person_detect_int8.tflite` at `ethos-u65-256`… `ethos-u85-256`). An earlier
draft said it "changes no memory figure"; measured, that is too strong: the
*encoded weight* footprint does move (`on_chip_flash_memory_used = 235.265625`
under `Flash_High` / `DRAM_Mid` vs `248.953125` under `Flash_Low` / `DRAM_Low`
/ `DRAM_High`), because the bandwidth model feeds the weight-encoding choice.
**Recommendation: pass no `--system-config` at all when the vendor one is
unavailable**, which is what Tasks 2-3 specify. Revisit if cycle estimates or
weight-footprint accuracy become load-bearing.

**Which `system_config` should `E1M-NX9101` pin?** Separate and sharper,
surfaced by conclusion 1: `metadata/socs/nxp/imx9/imx93.json` declares
`memory_mode: Shared_Sram` with no `system_config`, and `Shared_Sram` puts
`const_mem_area` on `Axi1`. So the i.MX 93 inherits vela's default
`Ethos_U65_Client_Server` (`axi1_port=Dram`) and lands its weights in DRAM,
where `Ethos_U65_Embedded` (`axi1_port=OffChipFlash`) would land them in
flash — a 228 KiB difference on `person_detect_int8.tflite`, measured. Unlike
the Alif case, the U65 is a single accelerator, so one scalar CAN describe it;
what is missing is a source for which one the i.MX 93's memory system actually
is. Do not guess it.

## Deliberately NOT in this plan

- Shipping or sanitising `ensemble_vela.ini` — a licensing decision, not code.
- `models[].compile.ethos_u` in `board.yaml` — rejected above on the schema's
  own stated purpose.
- A flash/MRAM figure in `requires` — vela reports
  `on_chip_flash_memory_used = 0.234375` for the fixture and nothing consumes
  it; adding it is a `.alpmodel` contract change and belongs in its own slice.
- Re-pinning tan's three stale `PINNED_SDK_COMMIT` constants — that needs the
  post-merge SHA plus the HELD ADR-0026 planner port.
