#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-slice Kconfig (alp.conf) emitter.

`_slice_alp_conf` renders a Zephyr slice's Kconfig fragment off the parsed model
+ the resolved capabilities / chip-driver / library / peripheral enables;
`_emit_extra_library_profile` renders an extra `libraries:` profile fragment.
Extracted as the #285 kconfig emit seam. The slug / peripheral-Kconfig tables
come from the slugs.py leaf; the `_CHIP_SUBSYSTEMS` / `_LIBRARY_KCONFIG` tables
are lazy-imported from alp_project inside the function (the existing
alp_project<->alp_orchestrate cycle-break), with the same in-body sys.path setup.
"""

from __future__ import annotations

from typing import Any, Optional

import yaml

from alp_project import (
    resolve_capabilities,
    silicon_to_kconfig,
    som_unpopulated_capabilities,
)

from . import libraries as _library_layer
from .models import BoardProject, Slice
from .paths import METADATA_ROOT, REPO
from .partition import resolve_storage_partitions
from .slugs import (
    _PERIPHERAL_KCONFIG,
    _board_define_slug,
    _slugs_from_helper_firmware,
    _slugs_from_on_module,
    _som_define_slug,
)


def _emit_extra_library_profile(
    name: str,
    profile_rel: str,
    project: "BoardProject",
) -> list[str]:
    """Walk an extra_libraries `profile:` file and emit the per-class
    first-match Kconfig lines for the active SoM.

    Mirrors the curated `metadata/library-profiles/<lib>/hw-backends.yaml`
    matcher in `alp_project._emit_library_hw_backends`: each accelerator
    class declares a `priority:` list whose entries carry
    `silicon:` / `soc_family:` / `requires_cap:` matchers plus a
    `kconfig:` directive.  First matching entry per class wins; an
    `sw_fallback:` block with `kconfig:` is always emitted at the end
    so the build has a SW floor when no HW backend matches.

    Returns the list of emitted lines (each line is a complete
    `CONFIG_*=y` or `# ...` comment); empty list when the profile
    parses but nothing matches and there's no sw_fallback.

    Failures (malformed YAML, missing keys) emit a single `#`-prefixed
    diagnostic comment so the customer sees the failure in the slice's
    alp.conf rather than getting silent drop-out.
    """
    profile_path = (REPO / profile_rel).resolve()
    try:
        doc = yaml.safe_load(profile_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as e:
        return [f"# extra_libraries[{name}] profile parse failed: {e}"]
    if not isinstance(doc, dict):
        return [f"# extra_libraries[{name}] profile is not a mapping"]

    # Match keys come from the active SoM.  Mirror
    # alp_project._SOC_FAMILY_TOKEN so extra_libraries entries can
    # share existing curated-profile match keys when desired.
    silicon_ref = project.som_preset.get("silicon") or ""
    family = (project.som_preset.get("family") or "").lower()
    soc_family_token: Optional[str] = None
    if family.startswith("alif-ensemble"):
        soc_family_token = "alif_ensemble"
    elif family.startswith("renesas-rzv2n"):
        soc_family_token = "renesas_rzv2n"
    elif family.startswith("nxp-imx9"):
        soc_family_token = "nxp_imx9"

    # resolve_capabilities merges SoC-JSON defaults + SoM overrides.
    capabilities = resolve_capabilities(project.som_preset, METADATA_ROOT)

    def _cap_truthy(cap_name: str) -> bool:
        v = capabilities.get(cap_name)
        if v is None:
            return False
        if isinstance(v, bool):
            return v
        if isinstance(v, int):
            return v > 0
        s = str(v).lower()
        if s in ("true", "yes"):
            return True
        if s in ("false", "no", "null", "none", "0", ""):
            return False
        try:
            return int(s) > 0
        except ValueError:
            return False

    def _entry_matches(e: dict[str, Any]) -> bool:
        sili = e.get("silicon")
        sf = e.get("soc_family")
        cap = e.get("requires_cap")
        if sili is not None and sili != silicon_ref:
            return False
        if sf is not None and sf != soc_family_token:
            return False
        if cap is not None and not _cap_truthy(str(cap)):
            return False
        return True

    out: list[str] = []
    accelerators = doc.get("accelerators") or []
    if isinstance(accelerators, list):
        for cls in accelerators:
            if not isinstance(cls, dict):
                continue
            cls_name = cls.get("class") or "<unknown>"
            for p in (cls.get("priority") or []):
                if not isinstance(p, dict):
                    continue
                if not _entry_matches(p):
                    continue
                kc = p.get("kconfig")
                if isinstance(kc, str) and kc:
                    out.append(f"{kc}  # {name} / {cls_name}")
                break

    sw = doc.get("sw_fallback")
    if isinstance(sw, dict):
        kc = sw.get("kconfig")
        if isinstance(kc, str) and kc:
            out.append(f"{kc}  # {name} / sw_fallback")

    return out


def _slice_alp_conf(project: BoardProject, slice_: Slice) -> str:
    """Per-core Kconfig fragment for a Zephyr slice.

    Emits the full Kconfig the slice needs: baseline + log + silicon +
    per-core peripherals/libraries + SoM-intrinsic chip drivers (auto-
    derived from ``on_module:`` + ``helper_firmware:`` in the SoM preset)
    + board-populated chip drivers (from board.yaml ``board.populated:``
    + the board preset) + the Zephyr subsystem enables those chip drivers
    need (e.g. an enabled ``rv3028c7`` chip driver pulls in ``CONFIG_I2C=y``).

    Swapping ``som.sku:`` in board.yaml automatically changes the SoM-
    intrinsic chip set with no other board.yaml edits required.
    """
    silicon = project.som_preset.get("silicon")
    kconfig = silicon_to_kconfig(silicon)
    diagnostics = project.diagnostics

    # Lazy-import alp_project tables — alp_project imports us, so a
    # top-level import would cycle.  Only paid when emitting Zephyr
    # fragments.
    import sys as _sys
    from pathlib import Path as _Path
    _scripts = _Path(__file__).resolve().parent.parent  # the scripts/ dir
    if str(_scripts) not in _sys.path:
        _sys.path.insert(0, str(_scripts))
    from alp_project import (  # type: ignore
        _CHIP_SUBSYSTEMS,
        _LIBRARY_KCONFIG,
    )

    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_orchestrate.py "
                 "-- do not edit.")
    lines.append(f"# Per-core Kconfig fragment for slice "
                 f"`{slice_.core_id}` ({slice_.os}).")
    lines.append("")
    lines.append("CONFIG_ALP_SDK=y")
    lines.append("CONFIG_LOG=y")
    lines.append("CONFIG_PRINTK=y")
    if diagnostics.get("last_error", True):
        lines.append("CONFIG_THREAD_LOCAL_STORAGE=y")
    log_level = diagnostics.get("log_level")
    if log_level is not None:
        log_level_kc = {
            "error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4,
        }
        if log_level in log_level_kc:
            lines.append(f"CONFIG_LOG_DEFAULT_LEVEL={log_level_kc[log_level]}")
    lines.append("")
    if kconfig:
        lines.append(f"# SoM silicon ({silicon} via {project.sku})")
        lines.append(f"CONFIG_{kconfig}=y")
        lines.append("")

    # Cross-EVK board facade selector (<alp/board.h>).
    # Emitted only when the project resolves to a named board preset
    # (e.g. `preset: e1m-x-evk` -> board_name "E1M-X-EVK").
    # CONFIG_COMPILER_OPT passes the define into every source file the
    # Zephyr build compiles, so the facade header can resolve the
    # correct EVK-specific BOARD_* aliases without per-app prj.conf
    # entries or extra_args.  Inline boards (no preset/name) skip this;
    # native_sim testcases that do need the define set it via extra_args.
    # Per-SKU capability-restriction gate for <alp/soc_caps.h>: emitted
    # ONLY when the SoM preset declares `silicon_capabilities.unpopulated`
    # (no SKU in the current catalogue does), so unrestricted SKUs keep a
    # byte-identical fragment.  Folded into the same CONFIG_COMPILER_OPT
    # string as the board facade define because that Kconfig is
    # single-value (last write wins, not additive).
    som_def = ""
    if som_unpopulated_capabilities(project.som_preset):
        som_def = f"-DALP_SOM_{_som_define_slug(project.sku)}"
    if project.board_name:
        # Cross-EVK board facade selector for <alp/board.h>.  NOTE:
        # CONFIG_COMPILER_OPT is a single-value Kconfig string (last write
        # wins, not additive) -- an app must NOT also set it in prj.conf or
        # this -D is silently dropped; pass extra defines another way.
        lines.append("# Cross-EVK board facade selector (<alp/board.h>);"
                     " CONFIG_COMPILER_OPT is single-value (do not also set in prj.conf).")
        if som_def:
            lines.append("# + ALP_SOM_* capability-restriction gate: this SKU "
                         "leaves silicon capabilities unpopulated "
                         "(SoM preset silicon_capabilities.unpopulated).")
        opt = f"-DALP_BOARD_{_board_define_slug(project.board_name)}"
        if som_def:
            opt = f"{opt} {som_def}"
        lines.append(f'CONFIG_COMPILER_OPT="{opt}"')
        lines.append("")
    elif som_def:
        lines.append("# ALP_SOM_* capability-restriction gate (<alp/soc_caps.h>);"
                     " CONFIG_COMPILER_OPT is single-value (do not also set in prj.conf).")
        lines.append(f'CONFIG_COMPILER_OPT="{som_def}"')
        lines.append("")

    # ----------------------------------------------------------------
    # SoM-intrinsic chip drivers — derived from on_module: + helper_firmware:
    # in the SoM preset.  These are NOT declared by the customer; they
    # are determined by which SoM SKU the project targets.  Swapping
    # `som.sku:` from E1M-V2N101 to E1M-AEN701 automatically swaps
    # the on-module chip set without any board.yaml changes.
    # ----------------------------------------------------------------
    som_chips: set[str] = set()
    om = project.som_preset.get("on_module") or {}
    if om:
        for slug in _slugs_from_on_module(om):
            som_chips.add(slug)
    hf = project.som_preset.get("helper_firmware") or []
    for slug in _slugs_from_helper_firmware(hf):
        som_chips.add(slug)

    # Slugs that map to BLOCK_ Kconfig symbols rather than CHIP_.
    # These live under blocks/ + <alp/blocks/*.h> because they are
    # SDK-level *block* utilities (`alp_button_led_*`, `alp_pdm_mic_*`)
    # rather than third-party-IC chip drivers; see blocks/README.md.
    _BLOCK_SLUGS = frozenset({"button_led", "pdm_mic"})

    def _slug_kconfig(slug: str) -> str:
        kind = "BLOCK" if slug in _BLOCK_SLUGS else "CHIP"
        return f"CONFIG_ALP_SDK_{kind}_{slug.upper()}"

    chip_subsystems: set[str] = set()
    if som_chips:
        sku_str = project.sku
        lines.append(f"# SoM-intrinsic chip drivers (from `{sku_str}` "
                     f"on_module + helper_firmware)")
        for chip in sorted(som_chips):
            lines.append(f"{_slug_kconfig(chip)}=y")
            for s in _CHIP_SUBSYSTEMS.get(chip, ()):
                chip_subsystems.add(s)
        lines.append("")

    # ----------------------------------------------------------------
    # Board-populated chip drivers.  Single source: the resolved
    # board_preset dict, which comes from either the shared
    # metadata/boards/<preset>.yaml or the project's inline
    # top-level fields (synthesised by the loader).  No project-level
    # override merge -- the schema's `oneOf` rule rejects mixing.
    #
    # Chip drivers compile per-chip via the
    # zephyr_library_sources_ifdef(CONFIG_ALP_SDK_CHIP_<NAME> ...)
    # (or CONFIG_ALP_SDK_BLOCK_<NAME> for the `button_led` / `pdm_mic`
    # SDK-level block helpers) entries in zephyr/CMakeLists.txt.
    #
    # We emit the chip-driver block on every zephyr slice — Kconfig
    # dedupes when multiple per-core fragments overlay onto the same
    # base.  This keeps `--core <id>` invocations self-sufficient
    # (the slice carries everything it needs to compile) without
    # depending on cross-slice ordering.
    # ----------------------------------------------------------------
    populated: dict[str, bool] = dict(
        (project.board_preset or {}).get("populated") or {})
    if populated:
        lines.append("# Board-populated chip drivers (from the resolved "
                     "board definition)")
        for chip, on in sorted(populated.items()):
            # Deduplicate: if the SoM block already emitted =y, skip
            # the board line to avoid redundant CONFIG entries.
            if on and chip in som_chips:
                continue
            lines.append(f"{_slug_kconfig(chip)}={'y' if on else 'n'}")
            if on:
                for s in _CHIP_SUBSYSTEMS.get(chip, ()):
                    chip_subsystems.add(s)
        lines.append("")

    # Project-declared chips (board.yaml top-level `chips:` array).
    # Adds chip drivers the application links directly via
    # <alp/chips/<name>.h> -- e.g. when the project plugs an external
    # sensor into a board's headers that's not in the board's
    # `populated:` table.  Each entry maps to CHIP_<NAME>=y (or
    # BLOCK_<NAME>=y for the SDK-level block helpers).
    project_chips = [c for c in (project.chips or [])
                     if c not in som_chips and not populated.get(c)]
    if project_chips:
        lines.append("# Project-declared chips (board.yaml `chips:` array)")
        for chip in sorted(set(project_chips)):
            lines.append(f"{_slug_kconfig(chip)}=y")
            for s in _CHIP_SUBSYSTEMS.get(chip, ()):
                chip_subsystems.add(s)
        lines.append("")

    # Zephyr subsystems: union of (chip-driver-required subsystems for
    # the first zephyr core's chip block) + (this core's `peripherals:`
    # array, which adds to the union per spec §4.6).
    periph_subsystems: set[str] = set()
    for periph in slice_.peripherals or []:
        kc = _PERIPHERAL_KCONFIG.get(periph)
        if kc:
            periph_subsystems.add(kc)
    all_subsystems = chip_subsystems | periph_subsystems
    if all_subsystems:
        lines.append(f"# Zephyr subsystems required on core "
                     f"`{slice_.core_id}` (chip drivers + peripherals)")
        for s in sorted(all_subsystems):
            lines.append(f"CONFIG_{s}=y")
        lines.append("")

    if slice_.libraries:
        lines.append(f"# Libraries declared on core "
                     f"`{slice_.core_id}`")
        for lib in sorted(slice_.libraries):
            kcs = _LIBRARY_KCONFIG.get(lib)
            if kcs:
                for kc in kcs:
                    lines.append(kc)
            else:
                lines.append(
                    f"# TODO: wire library '{lib}' once its v0.4 enable lands")
        lines.append("")

    # Project-wide curated third-party libraries (top-level `libraries:`,
    # ADR 0018).  Emitted from the metadata/libraries/<name>.yaml manifests;
    # resolve_selection() has already rejected any incompatible selection.
    # Guard keeps a project with no `libraries:` byte-identical.
    library_lines = _library_layer.zephyr_kconfig_lines(project, slice_)
    if library_lines:
        lines.append("# Curated third-party libraries "
                     "(project `libraries:`, ADR 0018)")
        lines.extend(library_lines)
        lines.append("")

    # ----------------------------------------------------------------
    # Open-set extra_libraries (v0.6 P2.1).  Each entry declares either
    # inline `kconfig:` lines (emitted verbatim) or a `profile:` path
    # to a hw-backends.yaml-style file we walk with the same silicon /
    # soc_family / requires_cap matcher as the curated libraries.
    # _validate_consistency() guarantees exactly-one of kconfig/profile
    # and the profile file resolves, so the per-entry checks here are
    # just for emit-time safety.
    # ----------------------------------------------------------------
    if slice_.extra_libraries:
        lines.append(f"# Extra libraries declared on core "
                     f"`{slice_.core_id}` (open-set escape hatch)")
        for entry in sorted(slice_.extra_libraries,
                            key=lambda e: e.get("name", "")):
            name = entry.get("name", "<unnamed>")
            kc_lines = entry.get("kconfig")
            profile = entry.get("profile")
            if kc_lines:
                lines.append(f"# extra_libraries[{name}] (inline kconfig)")
                for kc in kc_lines:
                    lines.append(str(kc))
            elif profile:
                lines.append(f"# extra_libraries[{name}] (profile: {profile})")
                profile_lines = _emit_extra_library_profile(
                    name, profile, project)
                if profile_lines:
                    lines.extend(profile_lines)
                else:
                    lines.append(
                        f"# (no matching backend for "
                        f"silicon={project.som_preset.get('silicon')} "
                        f"in {profile})")
        lines.append("")

    # Inference dispatchers.  Driven entirely by the SoM preset's
    # `capabilities:` matrix + SoC-JSON `cores[]` -- NOT by board.yaml.
    # The customer never picks a backend at build time; the SDK compiles
    # in every dispatcher the silicon supports, and apps choose
    # per-handle at runtime via alp_inference_open(.backend=...).
    # CPU/TFLM is the universal SW fallback and is always on.
    #
    # Two layers of variant detail are emitted alongside the umbrella
    # switches so the SDK driver code + (upstream) kernel libraries
    # compile against the right per-silicon variant:
    #
    #   - CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U{55,65,85}=y -- picked from
    #     the SoM preset's `npu_population:` list (preferred) with a
    #     capability-count fallback for SoMs that haven't declared the
    #     fine-grained population block yet.  U85 carries Arm's larger
    #     MAC array + TensorOptimized kernels; U55 carries the smaller
    #     MAC + reference kernels; U65 is i.MX 93-only.
    #
    #   - CONFIG_ALP_SDK_INFERENCE_TFLM_{NEON,HELIUM,REF}=y -- picked
    #     from the SoC JSON's `cores[<slice.core_id>].vector_extension`
    #     so the CPU-side TFLM kernel set matches the target core's SIMD
    #     reality (NEON on A-cluster, Helium MVE on M55, scalar / REF
    #     on baseline M33 / M4).  Per-core in case a single SoM hosts
    #     multiple core classes (E7 = A32 + M55, all three Helium /
    #     Neon flavours).
    #
    # resolve_capabilities merges SoC-JSON defaults with SoM overrides
    # so silicon-determined caps (ethos_u55_count, drp_ai, ...) resolve
    # even when removed from the SoM YAML (capability unification, slice 3b).
    capabilities = resolve_capabilities(project.som_preset, METADATA_ROOT)
    inference_lines: list[str] = ["CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y"]

    # ---- G-2 -- CPU-class TFLM kernel selector --------------------
    # Look up this slice's core in the SoC spec; pick exactly one of
    # NEON / HELIUM / REF based on the vector_extension field.  Defaults
    # to REF when the SoC JSON is silent (paper-correct on the scalar
    # M33s -- iMX 93 m33, V2N m33_sm).
    tflm_kernel_kc: str = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y"
    for c in (project.soc_spec.get("cores") or []):
        if c.get("id") != slice_.core_id:
            continue
        vec = (c.get("vector_extension") or "").lower()
        ctype = (c.get("type") or "").lower()
        if vec == "neon" or ctype.startswith("cortex-a"):
            tflm_kernel_kc = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_NEON=y"
        elif vec == "helium":
            tflm_kernel_kc = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y"
        else:
            tflm_kernel_kc = "CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y"
        break
    inference_lines.append(tflm_kernel_kc)

    # ---- G-1 -- per-variant Ethos-U selector ---------------------
    # Prefer the SoM preset's `inference.npu_population[]` (richer --
    # also names the role + paired-core); fall back to the capability
    # counts (ethos_u{55,65,85}_count) for SoMs that haven't yet
    # declared the per-instance block.  Both AEN401 / AEN601 / AEN801
    # populate npu_population[]; AEN701 declares U55s there too; the
    # i.MX 93 SoM relies on the capability-count fallback today.
    ethos_variants: set[str] = set()
    npu_pop = (project.som_preset.get("inference") or {}).get("npu_population") or []
    for entry in npu_pop:
        v = (entry.get("variant") if isinstance(entry, dict) else "") or ""
        v = v.lower()
        if v in ("u55", "u65", "u85"):
            ethos_variants.add(v)
    # Capability-count fallback (handles SoMs without npu_population:).
    if (capabilities.get("ethos_u55_count") or 0) > 0:
        ethos_variants.add("u55")
    if (capabilities.get("ethos_u65_count") or 0) > 0:
        ethos_variants.add("u65")
    if (capabilities.get("ethos_u85_count") or 0) > 0:
        ethos_variants.add("u85")
    ethos_present = bool(ethos_variants)
    if ethos_present:
        # Per-silicon Ethos-U backend (Slice 3 registry layout):
        # Alif Ensemble (AEN) -> _BACKEND_ETHOS_U_AEN
        # NXP i.MX 93        -> _BACKEND_ETHOS_U_N93
        if silicon == "nxp:imx9:imx93":
            inference_lines.append("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93=y")
        else:
            inference_lines.append("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y")
        for v in sorted(ethos_variants):
            inference_lines.append(f"CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_{v.upper()}=y")
    # DRP-AI3 and DEEPX DX-M1 have NO Zephyr Kconfig -- deliberately.
    # Both engines are A55/Linux-side only (DRP-AI3 via the MERA/TVM
    # userspace runtime, DX-M1 via libdxrt over the A55's PCIe); an
    # M-class Zephyr slice cannot drive either (issues #58/#59), so it
    # gets TFLM only.  Their build wiring lives on the cmake-args /
    # Yocto emit paths (_slice_cmake_args below).
    lines.append("# Inference dispatchers (from SoM capabilities -- "
                 "customer does not pick)")
    lines.extend(inference_lines)
    lines.append("")

    # ----------------------------------------------------------------
    # Per-slice memory tuning (board.yaml `cores.<id>.memory:`).
    # ----------------------------------------------------------------
    mem = slice_.memory or {}
    mem_lines: list[str] = []
    if mem.get("stack_kib"):
        mem_lines.append(f"CONFIG_MAIN_STACK_SIZE={int(mem['stack_kib']) * 1024}")
    if mem.get("isr_stack_kib"):
        mem_lines.append(f"CONFIG_ISR_STACK_SIZE={int(mem['isr_stack_kib']) * 1024}")
    if mem.get("heap_kib") is not None:
        mem_lines.append(f"CONFIG_HEAP_MEM_POOL_SIZE={int(mem['heap_kib']) * 1024}")
    if mem_lines:
        lines.append("# Per-slice memory tuning (board.yaml cores.<id>.memory:)")
        lines.extend(mem_lines)
        lines.append("")

    # ----------------------------------------------------------------
    # Per-slice power-management profile.
    # ----------------------------------------------------------------
    pwr = slice_.power or {}
    pwr_lines: list[str] = []
    sleep_mode = (pwr.get("sleep_mode") or "disabled").lower()
    if sleep_mode != "disabled":
        pwr_lines.append("CONFIG_PM=y")
        pwr_lines.append("CONFIG_PM_DEVICE=y")
        # PM_STATE_* selection is silicon-family-specific; lowest-power
        # state the hierarchy keeps is mirrored in a single hint flag.
        pwr_lines.append(f"# Sleep target: {sleep_mode}")
    for wake in (pwr.get("wakeup_sources") or []):
        # Wake-source declarations land as hint comments only.  Zephyr
        # marks a device as wakeup-capable via the DT `wakeup-source;`
        # property + a runtime pm_device_wakeup_enable() call; there is
        # no top-level CONFIG_PM_DEVICE_WAKE_<SUBSYS> Kconfig symbol, so
        # emitting one trips the build with `undefined symbol` warnings
        # (-> kconfig errors -> twister CMake build failure).  The real
        # DT-overlay + runtime-enable plumbing lands per silicon in v0.7;
        # until then the wake-source list is documented in the generated
        # alp.conf for the customer to wire by hand.
        if isinstance(wake, str) and not wake.startswith("E1M_"):
            pwr_lines.append(
                f"# wakeup source: {wake} "
                "(DT wakeup-source; + pm_device_wakeup_enable() pending v0.7)")
        elif isinstance(wake, str):
            pwr_lines.append(f"# wakeup source: {wake} (per-silicon Kconfig pending)")
    if pwr_lines:
        lines.append("# Per-slice power-management profile (board.yaml cores.<id>.power:)")
        lines.extend(pwr_lines)
        lines.append("")

    # ----------------------------------------------------------------
    # Storage partitions (board.yaml `storage:` block).  Emits per-fs
    # Kconfig for every resolved (non-blocked) partition.  Blocked
    # partitions produce a hint comment so the slice build still
    # compiles but the runtime mount fails loudly via the DTS overlay.
    #
    # Per-slice ownership of partitions is not modelled today: every
    # Zephyr/baremetal slice gets the union.  Kconfig dedupes when
    # multiple per-core fragments overlay onto the same base.  v0.7
    # may add `core:` to storage entries for per-slice scoping.
    # ----------------------------------------------------------------
    partitions = resolve_storage_partitions(project)
    ok_partitions = [p for p in partitions if p.status == "ok"]
    fs_seen: set[str] = set()
    for p in ok_partitions:
        fs_seen.add(p.fs)
    fs_kconfig: list[str] = []
    if "littlefs" in fs_seen:
        fs_kconfig.append("CONFIG_FILE_SYSTEM=y")
        fs_kconfig.append("CONFIG_FILE_SYSTEM_LITTLEFS=y")
    if "fat" in fs_seen:
        fs_kconfig.append("CONFIG_FILE_SYSTEM=y")
        fs_kconfig.append("CONFIG_FAT_FILESYSTEM_ELM=y")
    if "ext4" in fs_seen:
        fs_kconfig.append("CONFIG_FILE_SYSTEM=y")
        fs_kconfig.append("CONFIG_FILE_SYSTEM_EXT2=y")
    if fs_kconfig or ok_partitions:
        lines.append("# Storage partitions (board.yaml `storage:`)")
        # FILE_SYSTEM=y appears twice in the list when multiple fs
        # types are enabled; dedupe before emit.
        for kc in sorted(set(fs_kconfig)):
            lines.append(kc)
        # Per-partition LITTLEFS partition labels surface as a hint
        # comment only.  Modern Zephyr does NOT define a per-partition
        # CONFIG_FS_LITTLEFS_PARTITION_<NAME> Kconfig -- the partition
        # gets matched via the DT `fixed-partitions` node + the
        # chosen `zephyr,storage-partition` / FIXED_PARTITION_ID()
        # macro at runtime.  Emitting a Kconfig stem trips Zephyr's
        # "assignment to undefined symbol" warning + aborts the build.
        for p in ok_partitions:
            if p.fs == "littlefs":
                lines.append(
                    f"# partition[{p.name}] -> mount at runtime via "
                    f"FIXED_PARTITION_ID({p.name}_partition)")
        blocked = [p for p in partitions if p.status == "blocked"]
        for p in blocked:
            lines.append(f"# BLOCKED storage[{p.name}]: "
                         f"{p.reason or 'unknown reason'}")
        lines.append("")

    # ----------------------------------------------------------------
    # OTA Zephyr client (board.yaml `ota:` block, provider-driven dispatch).
    #
    # Resolved design Q on the Zephyr-side OTA client (ADR 0009 follow-up):
    #   provider: mender   -> Mender-MCU-client (out-of-tree, BSD-3)
    #   provider: hawkbit  -> Zephyr Hawkbit DDI client (upstream)
    #   provider: mcumgr   -> Zephyr MCUmgr (upstream; transport is the app's call)
    #
    # Yocto slices keep the existing Mender server-mode dispatch in
    # _slice_local_conf().  This block handles the Zephyr side: per-slice
    # Kconfig that compiles the matching client in.  Settings (server URL,
    # poll interval, tenant token) thread through Kconfig string values
    # when declared in `ota:`; placeholders (${VAR}) pass through verbatim
    # so the build system substitutes at link time.
    # ----------------------------------------------------------------
    ota = project.ota or {}
    provider = (ota.get("provider") or "").lower() if isinstance(ota, dict) else ""
    if provider and provider in ("mender", "hawkbit", "mcumgr"):
        lines.append(f"# OTA Zephyr client (board.yaml `ota.provider: {provider}`)")
        srv = (ota.get("server") or {}) if isinstance(ota.get("server"), dict) else {}
        poll = ota.get("poll_interval_s")
        if provider == "mender":
            # Mender-MCU-client.  The west.yml `mender` group is NOT
            # active by default (the upstream mender-mcu-client
            # repository is pinned alongside the v0.7 OTA wiring), so
            # CONFIG_MENDER_* would resolve to undefined-symbol warnings
            # under twister and abort the build.  Emit the settings as
            # hint comments so the configuration is visible to the
            # customer (and to anyone reviewing the generated alp.conf)
            # while keeping the build clean.  When the mender west
            # group flips active the comments here go back to live
            # CONFIG_* lines in a single commit.
            lines.append("# Mender-MCU-client wiring is pending the v0.7 OTA module")
            lines.append("# (mender-mcu-client west group activation).")
            lines.append("# CONFIG_MENDER_MCU_CLIENT=y")
            if srv.get("url"):
                lines.append(f'# CONFIG_MENDER_SERVER_URL="{srv["url"]}"')
            if srv.get("tenant"):
                lines.append(f'# CONFIG_MENDER_TENANT_TOKEN="{srv["tenant"]}"')
            if ota.get("artifact_name"):
                lines.append(f'# CONFIG_MENDER_ARTIFACT_NAME="{ota["artifact_name"]}"')
            if isinstance(poll, int) and poll > 0:
                lines.append(f"# CONFIG_MENDER_UPDATE_POLL_INTERVAL={poll}")
        elif provider == "hawkbit":
            # Zephyr upstream's Hawkbit DDI client.
            lines.append("CONFIG_HAWKBIT=y")
            lines.append("CONFIG_HAWKBIT_SHELL=y")
            if srv.get("url"):
                lines.append(f'CONFIG_HAWKBIT_SERVER="{srv["url"]}"')
            if isinstance(poll, int) and poll > 0:
                lines.append(f"CONFIG_HAWKBIT_POLL_INTERVAL={poll}")
        elif provider == "mcumgr":
            # MCUmgr is upstream; the transport (UART/BLE/UDP) stays the
            # app's call -- we only enable the base SMP server.
            lines.append("CONFIG_MCUMGR=y")
            lines.append("CONFIG_MCUMGR_GRP_IMG=y")
            lines.append("CONFIG_MCUMGR_GRP_OS=y")
            lines.append("# MCUmgr transport (UART/BLE/UDP) is the app's call;")
            lines.append("# enable the matching CONFIG_MCUMGR_TRANSPORT_* in your prj.conf.")
        lines.append("")

    # ----------------------------------------------------------------
    # Per-module log levels (board.yaml `diagnostics.modules:`).
    #
    # Zephyr's per-module CONFIG_<MOD>_LOG_LEVEL symbol exists only when
    # the matching module has called LOG_MODULE_REGISTER() at build time.
    # ALP_* SDK-side modules have not registered any LOG modules yet, so
    # emitting `CONFIG_ALP_<MOD>_LOG_LEVEL=N` trips the Zephyr build with
    # `undefined symbol ALP_<MOD>_LOG_LEVEL`.  We restrict the active
    # emit to modules whose Kconfig declaration is known to exist and
    # downgrade ALP_* + unknown modules to a hint comment until their
    # LOG_MODULE_REGISTER call lands.
    # ----------------------------------------------------------------
    modules = (project.diagnostics or {}).get("modules") or {}
    if modules:
        level_to_n = {"off": 0, "error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4}
        lines.append("# Per-module log-level overrides (board.yaml diagnostics.modules:)")
        for mod, lvl in sorted(modules.items()):
            n = level_to_n.get(str(lvl).lower())
            if n is None:
                continue
            kc_stem = mod.upper()
            if kc_stem.startswith("ALP_"):
                lines.append(
                    f"# CONFIG_{kc_stem}_LOG_LEVEL={n} "
                    "(pending LOG_MODULE_REGISTER on this SDK module)")
            else:
                lines.append(f"CONFIG_{kc_stem}_LOG_LEVEL={n}")
        lines.append("")

    return "\n".join(lines) + "\n"


def _slice_local_conf(project: BoardProject, slice_: Slice) -> str:
    """Per-core local.conf snippet for a Yocto slice."""
    machine = slice_.machine or f"e1m-{project.sku.lower().replace('e1m-', '')}"
    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_orchestrate.py "
                 "-- append to local.conf.")
    lines.append(f"# Per-core slice `{slice_.core_id}` "
                 f"(image: {slice_.image or 'custom'})")
    lines.append(f'MACHINE = "{machine}"')
    if slice_.libraries:
        imageinstall = " ".join(
            f"lib-{lib.replace('_', '-')}" for lib in slice_.libraries)
        lines.append(f'IMAGE_INSTALL:append = " {imageinstall}"')
    # Project-wide curated third-party libraries (top-level `libraries:`,
    # ADR 0018) with a Yocto integration section.  Guard keeps a project
    # with no such libraries byte-identical.
    library_pkgs = _library_layer.yocto_image_install(project, slice_)
    if library_pkgs:
        joined = " ".join(library_pkgs)
        lines.append(f'IMAGE_INSTALL:append = " {joined}"')
    if slice_.image:
        lines.append(f"# bitbake target: {slice_.image}")

    # Mender OTA wiring (board.yaml `ota:` block).  Emits Mender
    # `?=` (weak) assignments so hand-edited local.conf values in
    # the build dir still win.
    ota = project.ota or {}
    if ota.get("provider") == "mender":
        lines.append("")
        lines.append("# OTA (board.yaml `ota:` block)")
        lines.append('INHERIT += "mender-full"')
        if ota.get("artifact_name"):
            lines.append(f'MENDER_ARTIFACT_NAME ?= "{ota["artifact_name"]}"')
        srv = ota.get("server") or {}
        if srv.get("url"):
            lines.append(f'MENDER_SERVER_URL ?= "{srv["url"]}"')
        if srv.get("tenant"):
            lines.append(f'MENDER_TENANT_TOKEN ?= "{srv["tenant"]}"')
        sto = ota.get("storage") or {}
        if sto.get("device"):
            lines.append(f'MENDER_STORAGE_DEVICE_BASE ?= "{sto["device"]}"')
        if sto.get("boot_part_mb"):
            lines.append(f'MENDER_BOOT_PART_SIZE_MB ?= "{sto["boot_part_mb"]}"')
        if sto.get("total_size_mb"):
            lines.append(f'MENDER_STORAGE_TOTAL_SIZE_MB ?= "{sto["total_size_mb"]}"')
        if ota.get("poll_interval_s"):
            lines.append(f'MENDER_INVENTORY_INTERVAL ?= "{ota["poll_interval_s"]}"')

    return "\n".join(lines) + "\n"


def _slice_cmake_args(project: BoardProject, slice_: Slice) -> str:
    """Per-core cmake -D args for a baremetal / yocto slice.

    NPU-dispatch enables (DRP-AI, DEEPX) are driven from the SoM
    preset's `capabilities:` matrix -- never from board.yaml.  On
    multi-NPU SKUs (V2M101 = DRP-AI3 + DEEPX DX-M1) every available
    NPU is enabled so apps can dispatch concurrent independent models
    via alp_inference_open(.backend=...) per-handle.
    """
    family = project.som_preset.get("family") or "unknown"
    # resolve_capabilities merges SoC-JSON defaults with SoM overrides.
    capabilities = resolve_capabilities(project.som_preset, METADATA_ROOT)
    lines: list[str] = []
    lines.append("# Auto-generated by scripts/alp_orchestrate.py "
                 "-- pass to cmake.")
    lines.append(f"-DALP_SOM_SKU={project.sku}")
    lines.append(f"-DALP_SOM_FAMILY={family}")
    # Per-SKU capability-restriction gate for <alp/soc_caps.h>: emitted
    # ONLY when the SoM preset declares `silicon_capabilities.unpopulated`,
    # so every unrestricted SKU's cmake-args output stays byte-identical.
    if som_unpopulated_capabilities(project.som_preset):
        lines.append(f"-DALP_SOM_{_som_define_slug(project.sku)}")
    lines.append(f"-DALP_CORE_ID={slice_.core_id}")
    # Cross-EVK board facade selector (<alp/board.h>).
    # Emitted only when the project resolves to a named board preset
    # (e.g. `preset: e1m-x-evk` -> board_name "E1M-X-EVK").
    # Inline boards with no preset/name skip this; the facade defaults
    # to an #error forcing the builder to set it explicitly (e.g. via
    # extra_args for native_sim testcases).
    if project.board_name:
        lines.append(f"-DALP_BOARD_{_board_define_slug(project.board_name)}")
    if slice_.toolchain:
        lines.append(f"-DALP_TOOLCHAIN={slice_.toolchain}")
    if capabilities.get("drp_ai"):
        # Must match the option name in src/yocto/CMakeLists.txt
        # (ALP_SDK_USE_DRPAI_V2N -- compiles inference_drpai.cpp).
        lines.append("-DALP_SDK_USE_DRPAI_V2N=ON")
    if capabilities.get("deepx_dxm1"):
        lines.append("-DALP_SDK_USE_DEEPX_DXM1=ON")
    # Project-wide curated third-party libraries (top-level `libraries:`,
    # ADR 0018) with a baremetal integration section.  Guard keeps a project
    # with no such libraries byte-identical.
    for arg in _library_layer.baremetal_cmake_args(project, slice_):
        lines.append(arg)
    return "\n".join(lines) + "\n"
