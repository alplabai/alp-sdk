# SPDX-License-Identifier: Apache-2.0
"""
Shared fixtures for the tests/scripts/test_orchestrate_*.py suite -- the
test_orchestrate_*.py suite split (issue #460 / #673 Phase 3,
module-size reduction).

Not a test module itself: no test_* functions/classes live here, just
the constants + helpers every split-out test_orchestrate_*.py file needs
to invoke scripts/alp_orchestrate/ the same way the monolith did.
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))


def _write_board(tmp: Path, body: str, name: str = "board.yaml") -> Path:
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _synthetic_nx9101_root(tmp_path: Path) -> Path:
    """Build a scratch metadata root carrying a synthetic E1M-NX9101
    preset, isolated from the repo's real
    `metadata/e1m_modules/imx93/hw-revisions.yaml`. Returns the scratch
    metadata root -- pass it as `load_board_yaml(..., metadata_root=...)`'s
    own argument, which is the ONLY knob that matters: every
    `alp_orchestrate` resolver reads `BoardProject.effective_metadata_root()`
    (set from that argument), not a module-level `METADATA_ROOT` global, so
    monkeypatching `alp_orchestrate.METADATA_ROOT` is inert -- the
    submodules (`loader`/`carveout`/`partition`/`kconfig`/...) import
    `METADATA_ROOT` from `.paths` at their own module scope, a separate
    binding a patch on the `alp_orchestrate` package object never touches
    (#1485).

    Why: #1025's hw_rev-buildable gate refuses the REAL E1M-NX9101
    outright (its only hw_rev, imx93 r1, is `status: tbd`) before
    `load_board_yaml` ever reaches SoM-preset-specific logic
    (mailbox-controller-TBD carve-out blocking, wifi_ble-TBD IoT
    fallback, ...) that several tests need to exercise in isolation,
    and there is no second, buildable NX9101 hw_rev to pick instead.
    `family_revision_buildable` on a MISSING hw-revisions.yaml table
    returns None (unknown), not False, so a scratch root with no
    `e1m_modules/imx93/hw-revisions.yaml` at all makes the #1025 gate
    a no-op here -- this is test isolation of the SoM-preset logic, not
    a claim about the real E1M-NX9101's buildability (which stays
    refused everywhere else in the suite)."""
    import shutil

    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs = meta / "socs" / "nxp" / "imx9"
    schemas = meta / "schemas"
    registries = meta / "registries"
    for d in (e1m, socs, schemas, registries):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    shutil.copy(real_meta / "socs" / "nxp" / "imx9" / "imx93.json",
                socs / "imx93.json")
    # `_slice_alp_conf` resolves the SoM-silicon + peripheral Kconfig
    # symbols against `project.effective_metadata_root()` too (#1485) --
    # copy the real registries so this scratch root resolves for real.
    shutil.copy(real_meta / "registries" / "silicon-kconfig.json",
                registries / "silicon-kconfig.json")
    shutil.copy(real_meta / "registries" / "peripheral-kconfig.json",
                registries / "peripheral-kconfig.json")
    # The ADR-0018 library layer resolves `libraries:` against
    # `metadata_root` too (#1485) -- copy the real curated-library
    # manifests + alias table so a test declaring `libraries: [mbedtls]`
    # against this scratch root resolves for real instead of silently
    # falling through to the SDK's own in-tree metadata/ (which is exactly
    # the bug #1485 fixed: pre-fix, a scratch-root board.yaml with no
    # `libraries/` directory here still validated, because the resolver
    # never actually consulted `metadata_root` for library manifests).
    shutil.copytree(real_meta / "libraries", meta / "libraries")
    shutil.copy(real_meta / "library-aliases-v1.json",
                meta / "library-aliases-v1.json")

    preset = e1m / "E1M-NX9101.yaml"
    preset.write_text(textwrap.dedent("""
        schema_version: 1
        sku: E1M-NX9101
        family: nxp-imx9
        silicon: nxp:imx9:imx93
        on_module:
          wifi_ble: TBD
        topology:
          a55_cluster:
            os: yocto
            app: alp-image-edge
            machine: e1m-nx9101-a55
            toolchain: poky-glibc
          m33:
            os: zephyr
            app: alp-stock-shim
            board: alp_e1m_nx9101_m33
            toolchain: arm-zephyr-eabi
        mailbox:
          controller: TBD
          channels:
            - { id: 0, reserved_for: alp_default_rpmsg }
            - { id: 1, reserved_for: app }
        default_hw_rev: r1
    """).lstrip("\n"), encoding="utf-8")

    return meta


def _synthetic_aen_unresolved_base_root(tmp_path: Path) -> Path:
    """Build a scratch metadata root carrying a synthetic E1M-AEN-shaped
    preset (`E1M-AEN899`) whose `memory_map:` still authors a `base:
    "TBD"` row, isolated from the real `metadata/e1m_modules/E1M-AEN*.yaml`
    presets (issue #2096).

    #2053 resolved `mram_main.base` from `"TBD"` to a real address on
    every shipped AEN preset -- the only shipped-preset producer of an
    authored-but-unresolved `memory_map:` base anywhere in the tree. This
    fixture keeps that shape alive purely for
    `resolve_memory_regions()` / `resolve_carve_outs()`'s own wiring
    tests, without reintroducing a real `"TBD"` sentinel into any shipped
    preset. The `memory_map:` below is a verbatim copy of
    `metadata/e1m_modules/E1M-AEN301.yaml`'s seven rows (same SoC family,
    same shape) -- not a synthetic layout invented for this fixture.

    Sibling of `_synthetic_nx9101_root` above, not an extension of it:
    that helper's docstring and its one preset body are already
    NX9101-specific (the #1025 hw_rev-buildable-gate workaround, the
    imx93 SoC copy); folding a second, unrelated SoC family + preset
    shape into the same function would make ONE helper answer two
    different fixture requests through a mode flag, which is the harder
    thing to read, not the lazier one. Follows the same "copy just
    enough of metadata/ that `load_board_yaml` resolves for real" pattern
    instead.

    `sku: E1M-AEN899` deliberately isn't a real SKU number (E1M-AEN301..
    803 are all taken and more may ship later) -- the `AEN` prefix alone
    is what `alp_project_loader._sku_family()` matches to resolve the
    `aen` family directory, so any suffix works and this one reads as
    obviously synthetic. As with `_synthetic_nx9101_root`, no
    `e1m_modules/aen/hw-revisions.yaml` table exists in this scratch
    root, so `family_revision_buildable()` returns None (unknown, not
    refused) and board.yaml can omit `som.hw_rev` entirely.

    Silicon: `alif:ensemble:e3` (`silicon_variant: AE302F80F55D5LE`,
    `mram_mb: 5.5`) -- an Alif Ensemble SoC that DOES declare an on-die
    MRAM aperture (`soc_flash_base: 0x80000000`), so
    `resolve_aperture()` resolves non-None and the aperture-dependent
    branches in `_resolved_row()` / `_region_ipc_eligibility()` are
    actually reached rather than short-circuited by `aperture is None`
    the way every non-Alif SoM's fixture is."""
    import shutil

    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs = meta / "socs" / "alif" / "ensemble"
    schemas = meta / "schemas"
    registries = meta / "registries"
    for d in (e1m, socs, schemas, registries):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    shutil.copy(real_meta / "socs" / "alif" / "ensemble" / "e3.json",
                socs / "e3.json")
    shutil.copy(real_meta / "registries" / "silicon-kconfig.json",
                registries / "silicon-kconfig.json")
    shutil.copy(real_meta / "registries" / "peripheral-kconfig.json",
                registries / "peripheral-kconfig.json")
    shutil.copytree(real_meta / "libraries", meta / "libraries")
    shutil.copy(real_meta / "library-aliases-v1.json",
                meta / "library-aliases-v1.json")

    preset = e1m / "E1M-AEN899.yaml"
    preset.write_text(textwrap.dedent("""
        schema_version: 1
        sku: E1M-AEN899
        family: aen
        silicon: alif:ensemble:e3
        silicon_variant: AE302F80F55D5LE
        display_name: "Synthetic E1M-AEN test preset (#2096)"

        topology:
          m55_hp:
            app: alp-stock-shim
            board: alp_e1m_aentest_m55_hp
            toolchain: arm-zephyr-eabi
          m55_he:
            app: alp-stock-shim
            board: alp_e1m_aentest_m55_he
            toolchain: arm-zephyr-eabi

        # Verbatim copy of metadata/e1m_modules/E1M-AEN301.yaml's seven
        # rows -- `mram_main` is the row this fixture exists to keep
        # `base: "TBD"`.
        memory_map:
          - { name: mcuboot,   base: 0x80000000, size_kib: 64,   accessible_from: [m55_he, m55_hp], cacheable: true, carveout: false, write_authority: vendor_image }
          - { name: he_slot0,  base: 0x80010000, size_kib: 2688, accessible_from: [m55_he],          cacheable: true, carveout: false, write_authority: customer_image }
          - { name: hp_slot0,  base: 0x802b0000, size_kib: 2688, accessible_from: [m55_hp],          cacheable: true, carveout: false, write_authority: customer_image }
          - { name: reserved,  base: 0x80550000, size_kib: 64,   accessible_from: [m55_he, m55_hp],                  carveout: false, write_authority: none }
          - { name: storage,   base: 0x80560000, size_kib: 96,   accessible_from: [m55_he, m55_hp],                  carveout: false, write_authority: customer_runtime }
          - { name: atoc,      base: 0x80578000, size_kib: 32,   accessible_from: [m55_he, m55_hp],                  carveout: false, write_authority: secure_enclave }
          - { name: mram_main, base: "TBD",      size_kib: 5632, accessible_from: [m55_he, m55_hp], cacheable: true, write_authority: composite }

        mailbox:
          controller: alif_mhuv2
          channels:
            - { id: 0, reserved_for: alp_default_rpmsg }
            - { id: 1, reserved_for: app }

        default_hw_rev: r1
    """).lstrip("\n"), encoding="utf-8")

    return meta


V2N_HAPPY = """
name: test-v2n-board
som:
  sku: E1M-V2N101
  hw_rev: r1

libraries:
  - name: mbedtls
    cores: [a55_cluster]
  - name: nlohmann-json
    cores: [a55_cluster]
  - name: cmsis-dsp
    cores: [m33_sm]

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    peripherals: [ethernet, usb]
    iot:         { wifi: true, mqtt: true }
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [adc, pwm, i2c, gpio]
    inference:   { default_arena_kib: 64 }

ipc:
  - kind: rpmsg
    endpoints: [a55_cluster, m33_sm]
    carve_out_kb: 512
    name: alp_default_rpmsg

diagnostics:
  log_level: info
"""
