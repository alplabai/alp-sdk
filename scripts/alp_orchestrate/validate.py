#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Cross-field validators (P2.1 + P2.3 of v0.6).

The post-parse consistency rules over a resolved BoardProject / its slices: the
curated-library + TLS-provider sets, boot-signing family support,
`_validate_consistency` (the big cross-field pass), and the per-slice OS-class /
loader-rule enforcement. Pure checks -- they read the model and raise
OrchestratorError; nothing here mutates or emits. Extracted as the #285 validate
seam. The OS-class taxonomy comes from topology.py.
"""

from __future__ import annotations

import sys
from typing import TYPE_CHECKING, Optional

from .models import OrchestratorError, Slice
from .paths import REPO
from .topology import _core_os_choices, _cross_class_os

if TYPE_CHECKING:
    from .models import BoardProject  # noqa: F401


# Curated `libraries:` enum, mirrored from
# metadata/schemas/board.schema.json `cores.<id>.libraries.items.enum`.
# Used by _validate_consistency() to reject `extra_libraries:` slugs
# that collide with the curated set (the curated library MUST be
# declared via `libraries:` -- the escape hatch is for non-curated
# entries only).
_CURATED_LIBRARIES: frozenset[str] = frozenset({
    "etl", "fmt", "nlohmann_json", "doctest", "lvgl",
    "mbedtls", "cmsis_dsp", "littlefs", "tflite_micro",
    "u8g2", "gfx_compat", "madgwick_ahrs", "pid", "modbus",
    "coremqtt_sn", "libcoap", "tinygsm", "nanopb",
    "libwebsockets", "jsmn", "bearssl", "minimp3", "opus",
    "libhelix", "catch2",
})


def _boot_signing_supported_for_family(
    family: str,
) -> Optional[frozenset[str]]:
    """Return the allowed `boot.signing.algorithm:` values for a SoM
    family, or None when the family is unknown (caller accepts any
    schema-enum value).

    Per the v0.6 P2.3 cross-field-validator pass:
      - Alif Ensemble (with OPTIGA Trust M as attestation root):
        `ecdsa_p256`, `ed25519` only.  RSA is excluded -- the OPTIGA
        slot type paired with the AEN secure-boot flow is ECC-only.
      - Renesas RZ/V2N (+ V2N + DEEPX): `ecdsa_p256`, `rsa2048`,
        `rsa3072`.  ed25519 is excluded (not in the U-Boot
        fit-signature stack used on V2N today).
      - NXP i.MX 9x: `ecdsa_p256`, `rsa2048`, `rsa3072`.
      - Unknown families: None (validator falls through to the schema
        enum -- don't block on missing capability data for
        in-development presets).
    """
    f = (family or "").lower()
    if f.startswith("alif-ensemble"):
        return frozenset({"ecdsa_p256", "ed25519"})
    if f.startswith("renesas-rzv2n"):
        return frozenset({"ecdsa_p256", "rsa2048", "rsa3072"})
    if f.startswith("nxp-imx9"):
        return frozenset({"ecdsa_p256", "rsa2048", "rsa3072"})
    return None


# TLS providers acceptable for `cores.<id>.iot.tls: true` (rule 3).
# A library named `mbedtls` or `bearssl` in either `libraries:` or
# `extra_libraries:` satisfies the requirement.
_TLS_PROVIDERS: frozenset[str] = frozenset({"mbedtls", "bearssl"})


def _validate_consistency(project: "BoardProject") -> None:
    """Cross-field validation that the schema can't express cleanly.

    Runs after the project is fully assembled by `load_board_yaml()`.
    Raises `OrchestratorError` for hard violations; emits stderr
    `WARN: ...` lines for soft signals (rules 4 + 5).

    Rules (v0.6 P2.3):

    1. `ota.provider: mender` requires at least one
       `cores.<id>.os: yocto` slice.  Reason: server-mode Mender is a
       Yocto-only flow today; the Zephyr-side dispatch
       (Mender-MCU-client) lands separately per ADR 0009.
    2. `boot.signing.algorithm:` must be in the SoM family's supported
       set (see `_boot_signing_supported_for_family`).  Unknown
       families pass through (don't block on missing capability data).
    3. `cores.<id>.iot.tls: true` requires `libraries:` (curated) OR
       `extra_libraries:` (open-set) to include `mbedtls` or `bearssl`.
    4. WARNING (not error): `cores.<id>.inference.default_arena_kib`
       larger than `cores.<id>.memory.heap_kib`; inference may OOM.
    5. WARNING (not error): `cores.<id>.power.sleep_mode != disabled`
       with no `wakeup_sources:` declared; device will sleep but can't
       wake.

    Also enforces the `extra_libraries:` invariants the schema can't
    cleanly express (P2.1):

      - Exactly one of `kconfig:` / `profile:` per entry.
      - Names globally unique across every core's `extra_libraries:`.
      - Names don't collide with the curated `libraries:` enum.
      - `profile:` paths resolve to a real file (repo-relative).
    """
    # ---- extra_libraries invariants (P2.1) -----------------------
    seen_names: dict[str, str] = {}    # name -> first core_id seen on
    for core_id, slice_ in project.cores.items():
        for idx, entry in enumerate(slice_.extra_libraries):
            name = entry.get("name")
            if not isinstance(name, str) or not name:
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries[{idx}]: missing "
                    f"or non-string `name:`")
            has_kc = "kconfig" in entry and entry.get("kconfig") is not None
            has_pf = "profile" in entry and entry.get("profile") is not None
            if has_kc == has_pf:
                # Both set OR both unset -- reject.
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries entry '{name}' "
                    f"must declare exactly one of `kconfig:` or "
                    f"`profile:` (got "
                    f"{'both' if has_kc and has_pf else 'neither'}).  "
                    f"Inline `kconfig:` is the fast path; `profile:` "
                    f"points at a hw-backends.yaml-style file.")
            if name in _CURATED_LIBRARIES:
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries entry '{name}' "
                    f"collides with the curated `libraries:` enum; "
                    f"declare curated libraries via "
                    f"`cores.{core_id}.libraries:` instead.")
            if name in seen_names:
                raise OrchestratorError(
                    f"core '{core_id}' extra_libraries entry '{name}' "
                    f"collides with the same name on core "
                    f"'{seen_names[name]}'; extra_libraries names "
                    f"must be globally unique across all cores.")
            seen_names[name] = core_id
            if has_pf:
                prof = entry.get("profile")
                if not isinstance(prof, str) or not prof:
                    raise OrchestratorError(
                        f"core '{core_id}' extra_libraries entry "
                        f"'{name}' has non-string `profile:`")
                prof_path = (REPO / prof).resolve()
                if not prof_path.is_file():
                    raise OrchestratorError(
                        f"core '{core_id}' extra_libraries entry "
                        f"'{name}' `profile: {prof}` does not resolve "
                        f"to a file at {prof_path}")

    # ---- Rule 1: ota.provider requires a compatible slice -------
    # Provider-driven dispatch (ADR 0009 follow-up resolved):
    #   mender   -> Yocto (server-mode) OR Zephyr (Mender-MCU-client)
    #   hawkbit  -> Zephyr (upstream Hawkbit DDI client)
    #   mcumgr   -> Zephyr (upstream MCUmgr / SMP)
    ota = project.ota or {}
    provider = (ota.get("provider") or "").lower() if isinstance(ota, dict) else ""
    if provider == "mender":
        has_target = any(s.os in ("yocto", "zephyr")
                         for s in project.cores.values())
        if not has_target:
            raise OrchestratorError(
                "ota.provider: mender requires at least one "
                "cores.<id>.os: yocto (server-mode Mender) OR "
                "cores.<id>.os: zephyr (Mender-MCU-client); none "
                "found.  See ADR 0009 for the provider-driven "
                "dispatch.")
    elif provider == "hawkbit":
        if not any(s.os == "zephyr" for s in project.cores.values()):
            raise OrchestratorError(
                "ota.provider: hawkbit requires at least one "
                "cores.<id>.os: zephyr (Hawkbit DDI client is in "
                "Zephyr upstream); none found.")
    elif provider == "mcumgr":
        if not any(s.os == "zephyr" for s in project.cores.values()):
            raise OrchestratorError(
                "ota.provider: mcumgr requires at least one "
                "cores.<id>.os: zephyr (MCUmgr is Zephyr's SMP "
                "transport for OTA); none found.")

    # ---- Rule 2: boot.signing.algorithm supported by SoM family --
    boot = project.boot or {}
    sign = (boot.get("signing") or {}) if isinstance(boot, dict) else {}
    algo = ((sign.get("algorithm") or "").lower()
            if isinstance(sign, dict) else "")
    if algo:
        family = (project.som_preset.get("family") or "")
        supported = _boot_signing_supported_for_family(family)
        if supported is not None and algo not in supported:
            raise OrchestratorError(
                f"boot.signing.algorithm: {algo} is not supported by "
                f"SoM family '{family}'; supported algorithms for "
                f"this family: {sorted(supported)}.  Edit "
                f"boot.signing.algorithm or pick a SoM family that "
                f"supports {algo}.")

    # ---- Rule 3: iot.tls: true requires mbedtls/bearssl ----------
    for core_id, slice_ in project.cores.items():
        if not (slice_.iot or {}).get("tls"):
            continue
        libs = set(slice_.libraries or [])
        extras = {e.get("name") for e in slice_.extra_libraries
                  if isinstance(e.get("name"), str)}
        if not (libs & _TLS_PROVIDERS) and not (extras & _TLS_PROVIDERS):
            raise OrchestratorError(
                f"core '{core_id}' has iot.tls: true but neither "
                f"`libraries:` nor `extra_libraries:` declares a TLS "
                f"provider (mbedtls or bearssl).  Add one of these "
                f"to cores.{core_id}.libraries: (curated) or "
                f"cores.{core_id}.extra_libraries: (open-set).")

    # ---- Rule 4: inference arena > heap is a WARN ----------------
    for core_id, slice_ in project.cores.items():
        arena_kib = (slice_.inference or {}).get("default_arena_kib")
        heap_kib = (slice_.memory or {}).get("heap_kib")
        if (isinstance(arena_kib, int) and isinstance(heap_kib, int)
                and arena_kib > heap_kib):
            print(
                f"WARN: cores.{core_id}.inference.default_arena_kib="
                f"{arena_kib} > cores.{core_id}.memory.heap_kib="
                f"{heap_kib}; inference may OOM",
                file=sys.stderr,
            )

    # ---- Rule 5: sleep_mode != disabled with no wakeup_sources --
    for core_id, slice_ in project.cores.items():
        pwr = slice_.power or {}
        sleep_mode = (pwr.get("sleep_mode") or "disabled").lower()
        wake = pwr.get("wakeup_sources") or []
        if sleep_mode != "disabled" and not wake:
            print(
                f"WARN: cores.{core_id}.power.sleep_mode={sleep_mode} "
                f"but no wakeup_sources: declared; device will sleep "
                f"but cannot wake",
                file=sys.stderr,
            )


def _enforce_os_matches_core_class(slice_: Slice, core_type: str) -> None:
    """The runtime follows the core class and is not user-selectable
    (issue #95): a Cortex-A core runs Yocto/Linux, a Cortex-M core runs
    Zephyr/RTOS.  A board.yaml may only disable a core (`off`) or drop it to
    no-OS (`baremetal`); selecting the *other* class's OS is rejected.
    """
    if slice_.os in _cross_class_os(core_type):
        raise OrchestratorError(
            f"core '{slice_.core_id}' ({core_type or 'unclassified'}): its runtime "
            f"is determined by the core class (Cortex-A -> Yocto/Linux, "
            f"Cortex-M -> Zephyr/RTOS) and is not selectable. Set os: 'off' to "
            f"disable it or 'baremetal' for no-OS firmware -- got os: '{slice_.os}'.")


def _enforce_loader_rules(slice_: Slice) -> None:
    """Loader rules from spec §4.5: every non-off slice must declare
    enough to actually build."""
    if slice_.os == "off":
        return
    if slice_.os == "zephyr":
        if not slice_.app:
            raise OrchestratorError(
                f"core '{slice_.core_id}': os: zephyr requires `app:` "
                f"pointing at a prj.conf / CMakeLists.txt directory")
    elif slice_.os == "baremetal":
        if not slice_.app:
            raise OrchestratorError(
                f"core '{slice_.core_id}': os: baremetal requires `app:` "
                f"pointing at a CMakeLists.txt directory")
    elif slice_.os == "yocto":
        if not slice_.app and not slice_.image:
            raise OrchestratorError(
                f"core '{slice_.core_id}': os: yocto requires either "
                f"`app:` (custom recipe) or `image:` (stock recipe)")
    elif slice_.os not in _core_os_choices():
        raise OrchestratorError(
            f"core '{slice_.core_id}': unknown os '{slice_.os}'")
