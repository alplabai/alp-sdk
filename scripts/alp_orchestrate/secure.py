#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Secure-boot config emitters -- sysbuild.conf + the TF-M child-image overlay.

`emit_sysbuild_conf` renders the sysbuild.conf overlay from board.yaml's `boot:`
block (the MCUboot wiring); `emit_tfm_sysbuild_conf` renders the TF-M sysbuild
child-image overlay from `security.psa:`. Pure string templating off the parsed
BoardProject -- the leaf-most emit seam (#285), a sink the build-plan emitter
imports later.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

from .models import BoardProject, OrchestratorError
from .paths import REPO

# SoM-preset `family:` (metadata/e1m_modules/*.yaml, e.g.
# "alif-ensemble") -> the short slug the curated
# zephyr/sysbuild/<slug>/sysbuild.conf directory uses.  Mirrors the
# alif-ensemble / renesas-rzv2n family-token `startswith` match already
# duplicated in kconfig.py / validate.py.  Only families that actually
# ship a curated file are listed -- renesas-rzv2n / nxp-imx9 boot via
# U-Boot/Yocto, not sysbuild, so they have none today; adding one is a
# metadata-only change (drop the file under zephyr/sysbuild/<slug>/),
# no code change needed here.
_FAMILY_SYSBUILD_DIRS: dict[str, str] = {
    "alif-ensemble": "aen",
}

# The per-family `boot.method:` default `board.schema.json` documents but
# nothing implemented (#562): "the SoM family supplies the default
# (AEN/N93 -> mcuboot, V2N/V2N-M1 -> none on the Zephyr slice since
# U-Boot owns boot on Linux)".  `emit_sysbuild_conf` used to hard-default
# every family to `mcuboot`, so a Renesas project that omitted `method:`
# was pushed down the MCUboot path -- and, with `rsa3072` (a value
# `validate.py` PERMITS for renesas-rzv2n precisely because its boot
# chain is the U-Boot/FIT stack, not sysbuild), hard-raised and took the
# whole build-plan emit with it.  Same `family:`-token `startswith` match
# as `_FAMILY_SYSBUILD_DIRS` above and `validate._boot_signing_supported_
# for_family`.  `boot.method:` has no other consumer anywhere in the
# tree, so this emitter is the only site the default has to exist at.
_FAMILY_BOOT_METHOD_DEFAULTS: dict[str, str] = {
    "alif-ensemble": "mcuboot",
    "nxp-imx9":      "mcuboot",
    "renesas-rzv2n": "none",
}

# What an UNRECOGNISED `family:` token defaults to.  Deliberately the
# historical value, so an in-development preset whose family string is
# not in the table above emits exactly what it emitted before #562 --
# the fix is "recognised families get their documented default", not
# "the default silently flips for everyone".
_BOOT_METHOD_FALLBACK = "mcuboot"


def _default_boot_method(project: BoardProject) -> str:
    """The `boot.method:` this project's SoM family implies."""
    family = (project.som_preset.get("family") or "").lower()
    return next((v for k, v in _FAMILY_BOOT_METHOD_DEFAULTS.items()
                 if family.startswith(k)), _BOOT_METHOD_FALLBACK)


def _has_zephyr_slice(project: BoardProject) -> bool:
    """Whether any core in this project builds under Zephyr.

    sysbuild is a Zephyr-build concept: `build/alp_sysbuild.conf` is
    consumed only as `west build --sysbuild -- -DSB_CONF_FILE=<path>`.
    A project with no Zephyr slice has no sysbuild configure for the
    overlay to reach, so emitting one is dead output at best -- and at
    worst (#562) a hard refusal of a project that never runs sysbuild.
    """
    return any(s.os == "zephyr" for s in project.cores.values())


def _boot_target_is_single_slot(project: BoardProject) -> bool:
    """Whether this project resolves to a disjoint-slot0 boot shape on
    ANY of its Zephyr M55 cores -- i.e. any MCUboot swap mode (scratch,
    move, overwrite) has no slot1/scratch partition to swap into.

    Reuses `gen_zephyr_board._aen_role_slot0_map` -- the SAME resolver
    the board-DT generator (`_aen_dts` / `_aen_flash_partitions`) and
    `loader._resolve_slot0_load_address` already call to answer this
    exact question -- instead of re-deriving the answer from
    `memory_map:` region NAMES.  The two disagree in general:
    `metadata/schemas/som-preset-v1.schema.json` documents
    `memory_map:` as an SDK build-policy override "ONLY for non-stock
    partitioning", not something only a disjoint-slot0 SoM sets --
    `docs/v0.6-tbd-and-assumptions.md` lists filling the V2N, AEN E7
    and iMX93 memory_maps as open items.  A `memory_map:` present for
    an unrelated reason (an rpmsg carve-out, say) that declares no
    `<role>_slot0` region makes `_aen_flash_partitions` fall through to
    the STOCK two-slot layout -- a REAL `image-1` + `image-scratch` --
    regardless of what its OWN region names happen to be, so a check
    that scans those names for the substrings "slot1"/"scratch" can
    answer True (single-slot) on a target that generates a real slot1
    and a real scratch partition.  `_aen_role_slot0_map` asks the real
    question instead: does THIS role have its own `<role>_slot0`
    region (the only shape that drops slot1/scratch, #1069 --
    every AEN preset sets one: each SoM's
    M55 cores share one physical App MRAM, so slot0 is split into
    per-core windows and the secondary/scratch slot was dropped
    rather than forced to fit)?

    Only `m55_he`/`m55_hp` roles are AEN slot0-XIP cores (same
    convention as `loader._resolve_slot0_load_address`); a project
    with neither in `cores:` -- any non-AEN family, or an AEN project
    with no Zephyr M55 slice -- is never single-slot by this check.
    """
    memory_map = project.som_preset.get("memory_map")
    if not memory_map:
        return False
    from gen_zephyr_board import ZephyrBoardEmitError, _aen_role_slot0_map
    for core_id in project.cores:
        if not core_id.startswith("m55_"):
            continue
        role = core_id[len("m55_"):]
        if role not in ("he", "hp"):
            continue
        try:
            if _aen_role_slot0_map(memory_map, role) is not None:
                return True
        except ZephyrBoardEmitError as exc:
            raise OrchestratorError(
                f"cannot resolve boot.swap_algorithm default for "
                f"{project.sku}: {exc}") from exc
    return False


def sysbuild_family_base_conf(project: BoardProject) -> Optional[Path]:
    """Path to the curated `zephyr/sysbuild/<family>/sysbuild.conf` for
    this project's SoM family, or None when the family has no curated
    profile on disk.

    This is the LAYERING base (issue #807): a customer `boot:` overlay
    from `emit_sysbuild_conf` builds ON TOP of this file via a
    semicolon-separated `SB_CONF_FILE` list rather than replacing it,
    so a `boot:` block no longer forks family boot policy into two
    divergent places.  Verified against the pinned Zephyr 4.4.0's
    `share/sysbuild/cmake/modules/sysbuild_kconfig.cmake`: `SB_CONF_FILE`
    accepts a CMake list (`foreach(conf_file ${conf_file_expanded})`
    splits on the list separator) and every listed file lands in the
    merged sysbuild `.config`, later files winning on a repeated
    symbol -- confirmed empirically via a `native_sim` sysbuild
    configure with two `;`-joined conf files, both symbols present in
    the merged `.config`.
    """
    family = (project.som_preset.get("family") or "").lower()
    slug = next((v for k, v in _FAMILY_SYSBUILD_DIRS.items()
                 if family.startswith(k)), None)
    if slug is None:
        return None
    base = REPO / "zephyr" / "sysbuild" / slug / "sysbuild.conf"
    return base if base.is_file() else None


def emit_sysbuild_conf(project: BoardProject) -> str:
    """Emit the sysbuild.conf overlay for the project's `boot:` block.

    Returns a SB_CONFIG_*-style snippet the build wires into
    `build/alp_sysbuild.conf` (consumed via `west build --sysbuild
    -- -DSB_CONF_FILE=<abs path>`).  Returns an empty string when the project
    does not declare a `boot:` block (the SDK's stock per-family
    defaults at zephyr/sysbuild/<family>/sysbuild.conf apply), and when
    the project has NO Zephyr slice at all (#562) -- sysbuild exists
    only inside a Zephyr build, so a Yocto-only project has nothing to
    apply the overlay to.  On such a project the `boot:` block governs
    the U-Boot/FIT chain instead, which this emitter has never rendered;
    the previous behaviour was to render it as MCUboot anyway and, on
    `rsa3072`, to raise below and fail the project's entire build-plan
    emit with sysbuild advice for a platform that never runs sysbuild.

    `boot.method:` is optional and the SoM family supplies its default
    (`_FAMILY_BOOT_METHOD_DEFAULTS` -- the value board.schema.json has
    always documented).  It is NOT an unconditional `mcuboot`.

    Every `SB_CONFIG_*` symbol emitted here MUST exist in the pinned
    Zephyr's `share/sysbuild/**/Kconfig*` -- sysbuild treats an
    undefined-symbol warning as FATAL, so one invented name aborts the
    whole `boot:` configure (issue #807).  Slot/scratch partition
    *sizes* have no sysbuild expression at all: MCUboot takes slot
    geometry from the board DT partitions, per the Zephyr 4.4 Kconfig
    help text on every `MCUBOOT_MODE_*` choice -- that's also why
    `boot.slots` / `boot.scratch_size_kib` were dropped from the
    schema rather than fixed (see board.schema.json for the full
    rationale).
    """
    boot = project.boot or {}
    if not boot:
        return ""
    if not _has_zephyr_slice(project):
        return ""
    method = (boot.get("method") or _default_boot_method(project)).lower()
    if method == "none":
        return ("# Auto-generated from board.yaml `boot:` block.\n"
                "SB_CONFIG_BOOTLOADER_MCUBOOT=n\n")
    if method != "mcuboot":
        return ""

    lines: list[str] = [
        "# Auto-generated by scripts/alp_orchestrate.py from "
        "board.yaml `boot:`.",
        "# Drives the sysbuild MCUboot child image.  Customers who",
        "# omit `boot:` get the SDK's stock per-family defaults.",
        "",
        "SB_CONFIG_BOOTLOADER_MCUBOOT=y",
    ]
    sign = boot.get("signing") or {}
    algo = (sign.get("algorithm") or "").lower()
    if algo == "rsa3072":
        # sysbuild's BOOT_SIGNATURE_TYPE_RSA choice has no key-length
        # knob -- length is an mcuboot-image CONFIG_BOOT_SIGNATURE_
        # TYPE_RSA_LEN symbol (bootloader/mcuboot/boot/zephyr/Kconfig),
        # a different Kconfig namespace sysbuild never forwards into.
        # Until a per-image `sysbuild/mcuboot.conf` overlay seam exists
        # (see docs/secure-boot.md), rsa3072 has no honest emit -- fail
        # loud rather than silently ship rsa2048's default length.
        raise OrchestratorError(
            "boot.signing.algorithm: rsa3072 has no sysbuild "
            "expression yet -- SB_CONFIG_BOOT_SIGNATURE_TYPE_RSA has "
            "no key-length knob (that's an mcuboot-image CONFIG_BOOT_"
            "SIGNATURE_TYPE_RSA_LEN symbol, a different Kconfig "
            "namespace sysbuild doesn't forward).  Use rsa2048 (mcuboot's "
            "RSA default length) or ecdsa_p256/ed25519 until a per-image "
            "sysbuild/mcuboot.conf overlay seam ships.")
    algo_kc = {
        "ecdsa_p256": "SB_CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y",
        "rsa2048":    "SB_CONFIG_BOOT_SIGNATURE_TYPE_RSA=y",
        "ed25519":    "SB_CONFIG_BOOT_SIGNATURE_TYPE_ED25519=y",
    }.get(algo)
    if algo_kc:
        lines.append(algo_kc)
    if sign.get("key_file"):
        lines.append(f'SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="{sign["key_file"]}"')
    swap_explicit = boot.get("swap_algorithm")
    swap = (swap_explicit or "").lower()
    single_slot = _boot_target_is_single_slot(project)
    if swap_explicit and swap in ("scratch", "move", "overwrite") and single_slot:
        # #1413: this SKU's `memory_map:` has no slot1/scratch region
        # (see _boot_target_is_single_slot) -- every two-slot MCUboot
        # swap mode needs one.  An explicit request for one here isn't
        # a default that quietly drifted, it's a `boot:` block asking
        # for a partition that doesn't exist on this target's DT, so
        # fail loud rather than emit a config that can't boot.
        raise OrchestratorError(
            f"boot.swap_algorithm: {swap} needs a slot1/scratch "
            f"partition that {project.sku}'s disjoint-slot0 "
            "`memory_map:` (metadata/e1m_modules/"
            f"{project.sku}.yaml, #1069) doesn't declare -- this "
            "single-slot target only supports the stock "
            "single-app boot.  Drop `boot.swap_algorithm:` (it now "
            "defaults correctly on single-slot targets) rather than "
            "setting it explicitly.")
    if not swap_explicit:
        # Historically defaulted unconditionally to "scratch" (#1413):
        # wrong on a single-slot target, whose DT has no scratch
        # partition to swap into.  Single-slot targets default to the
        # single-app boot their curated zephyr/sysbuild/aen/sysbuild.conf
        # base already ships; every other target keeps the historical
        # "scratch" default.
        swap = "none" if single_slot else "scratch"
    swap_kc = {
        "scratch":   "SB_CONFIG_MCUBOOT_MODE_SWAP_SCRATCH=y",
        "move":      "SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE=y",
        "overwrite": "SB_CONFIG_MCUBOOT_MODE_OVERWRITE_ONLY=y",
        "none":      "SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y",
    }.get(swap)
    if swap_kc:
        lines.append(swap_kc)
    return "\n".join(lines) + "\n"


def emit_tfm_sysbuild_conf(project: BoardProject) -> str:
    """Emit the TF-M sysbuild child-image overlay for `security.psa:`.

    Returns a `SB_CONFIG_*` + `CONFIG_PSA_CRYPTO_*` snippet the build
    wires into `build/sysbuild/tfm/tfm.conf` (picked up by sysbuild
    convention from its `sysbuild/tfm/` path).  Returns an empty
    string when the project omits `security.psa:` or sets
    `tfm: false` -- PSA Crypto then runs entirely non-secure
    (mbedTLS-only) and no child image is built.

    The TF-M boundary lives on the same M55-HP core as the non-secure
    app via Armv8-M TrustZone-M; see ADR 0013.
    """
    security = project.security or {}
    psa = security.get("psa") or {}
    if not psa or not psa.get("tfm"):
        return ""

    # TF-M build type follows the project's MCUboot build_type when
    # set so the secure + bootloader artefacts ship the same flavour.
    # Default is Release; debug builds use MinSizeRel for parity with
    # the upstream sysbuild template.
    boot = project.boot or {}
    build_type = (boot.get("build_type") or "Release")
    if build_type.lower() == "debug":
        tfm_build_type = "Debug"
    elif build_type.lower() in ("minsizerel", "min_size_rel"):
        tfm_build_type = "MinSizeRel"
    elif build_type.lower() == "release":
        tfm_build_type = "Release"
    else:
        # Pass through verbatim if the customer set an exact CMake
        # build-type string we don't pre-canonicalise.
        tfm_build_type = build_type

    lines: list[str] = [
        "# Auto-generated by scripts/alp_orchestrate.py from "
        "board.yaml `security.psa:`.",
        "# Drives the sysbuild TF-M child image on the M55-HP core via "
        "Armv8-M TrustZone-M.",
        "# See docs/adr/0013-tfm-boundary-m55-hp-trustzone.md and "
        "docs/security-audit-plan.md.",
        "",
        "SB_CONFIG_TFM=y",
        f"SB_CONFIG_TFM_BUILD_TYPE={tfm_build_type}",
    ]

    persistent_slots = psa.get("persistent_slots")
    if persistent_slots is None:
        # Schema default mirrors v0.6: 16 slots.
        persistent_slots = 16
    lines.append(
        f"CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT={int(persistent_slots)}"
    )

    its = psa.get("its_storage")
    if its:
        lines.append(f'CONFIG_PSA_CRYPTO_ITS_BACKING_STORE="{its}"')
    ps = psa.get("ps_storage")
    if ps:
        lines.append(f'CONFIG_PSA_CRYPTO_PS_BACKING_STORE="{ps}"')

    att_root = (psa.get("attestation_root") or "none").lower()
    if att_root == "optiga_trust_m":
        lines.append(
            "# Attestation root anchored in OPTIGA Trust M (on-module "
            "secure element).")
        lines.append(
            "# See src/security/optiga_trust_m_bridge.c for the "
            "PSA <-> OPTIGA bridge driver.")
        lines.append("CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y")
    elif att_root == "tfm_internal":
        lines.append("CONFIG_ALP_SDK_PSA_ATTESTATION_TFM_INTERNAL=y")

    return "\n".join(lines) + "\n"
