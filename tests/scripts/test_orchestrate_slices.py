# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- SoM-intrinsic chip driver
auto-enable (_slugs_from_on_module(), _slugs_from_helper_firmware()) and
per-slice Kconfig / local.conf emission (_slice_alp_conf(),
_slice_local_conf()), including the IoT wifi/BLE/MQTT/TLS provider
dispatch.

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_slices.py -v
"""

from __future__ import annotations

import shutil
import sys
import textwrap
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    BoardProject,
    _slice_alp_conf,
    _slice_local_conf,
    _slugs_from_helper_firmware,
    _slugs_from_on_module,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# SoM-intrinsic chip driver auto-enable (Phase 4 on_module fix)
# Tests cover: _slugs_from_on_module, _slugs_from_helper_firmware,
# and _slice_alp_conf integration for V2N101 / AEN701 / NX9101.
# ---------------------------------------------------------------------


def test_slugs_from_on_module_v2n101() -> None:
    """V2N101 on_module: all non-TBD scalar chip slugs extracted;
    tps628640 (assembled: optional) is excluded; silicon field excluded."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-V2N101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_on_module(preset["on_module"])

    # Expected on-module chips (non-optional).
    for expected in ("act8760", "clk_5l35023b", "da9292", "eeprom_24c128",
                     "gd32g553", "murata_lbee5hy2fy", "optiga_trust_m",
                     "rtl8211fdi", "rv3028c7", "tmp112"):
        assert expected in slugs, f"missing expected slug: {expected}"

    # These must NOT appear.
    assert "tps628640" not in slugs, "optional assembled device must be excluded"
    assert "renesas:rzv2n:n44" not in slugs, "silicon field must be excluded"
    assert "TBD" not in slugs, "TBD values must be excluded"


def test_slugs_from_on_module_aen701() -> None:
    """AEN701 on_module: cc3501e, optiga_trust_m, rv3028c7, tmp112,
    eeprom_24c128 present; ospi_memories / hyperram storage MPNs are
    excluded (they have no chips/<part>/ driver)."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-AEN701.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_on_module(preset["on_module"])

    for expected in ("cc3501e", "eeprom_24c128", "optiga_trust_m",
                     "rv3028c7", "tmp112"):
        assert expected in slugs, f"missing expected slug: {expected}"

    assert "TBD" not in slugs, "TBD values must be excluded"
    assert "alif:ensemble:e7" not in slugs, "silicon field must be excluded"
    # Storage MPNs (OSPI NOR flash + HyperRAM) are NOT chip-driver slugs:
    # they have no chips/<part>/ driver, so emitting them as
    # CONFIG_ALP_SDK_CHIP_<X> would trip Zephyr's undefined-symbol guard.
    assert "MX25UM25645GXDI00" not in slugs, "OSPI flash MPN must not be a chip slug"
    assert "W958D8NBYA5I" not in slugs, "HyperRAM MPN must not be a chip slug"


def test_slugs_from_on_module_nx9101_tbd_filtered() -> None:
    """NX9101 on_module: TBD wifi_ble and ethernet_phy are filtered out;
    only pca9451a (the one non-TBD scalar chip) survives."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-NX9101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_on_module(preset["on_module"])

    assert "pca9451a" in slugs, "pca9451a must be present"
    assert "TBD" not in slugs, "TBD values must be excluded"
    # NX9101 has no i2c_devices or ospi_memories, so the list is short.
    for slug in slugs:
        assert slug != "TBD"


def test_slugs_from_helper_firmware_v2n101() -> None:
    """V2N101 helper_firmware: gd32g553 chip slug extracted."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-V2N101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_helper_firmware(preset.get("helper_firmware", []))
    assert "gd32g553" in slugs


def test_slugs_from_helper_firmware_aen701_tbd_filtered() -> None:
    """AEN701 helper_firmware: firmware_path is TBD but chip cc3501e
    is still a valid slug (chip: field is not TBD)."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-AEN701.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_helper_firmware(preset.get("helper_firmware", []))
    assert "cc3501e" in slugs


def test_slugs_from_helper_firmware_nx9101_empty() -> None:
    """NX9101 has no helper MCUs; helper_firmware: [] returns empty list."""
    import yaml
    with open(REPO / "metadata" / "e1m_modules" / "E1M-NX9101.yaml",
              encoding="utf-8") as f:
        preset = yaml.safe_load(f)
    slugs = _slugs_from_helper_firmware(preset.get("helper_firmware", []))
    assert slugs == []


def _make_som_only_project(tmp_path: Path, sku_yaml_content: str,
                           board_yaml_content: str,
                           sku: str = "E1M-TST001") -> "BoardProject":
    """Build a minimal BoardProject from an inline SoM preset + board.yaml.

    Creates a throwaway metadata root under tmp_path, writes the supplied
    preset YAML as the SoM file, and loads a board.yaml with no board.
    The board.schema schema copy has its ``som.sku`` pattern relaxed to
    also accept ``E1M-TST*`` names used by fixture tests.  The renesas n44
    SoC JSON is copied so presets that reference ``renesas:rzv2n:n44`` can
    resolve without the full repo metadata tree.
    """
    import json as _json
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    schemas = meta / "schemas"
    socs_v2n = meta / "socs" / "renesas" / "rzv2n"
    for d in (e1m, schemas, socs_v2n):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    # Copy the board-config schema and relax the sku pattern so synthetic
    # E1M-TST* SKUs used by fixture tests validate without error.
    bc_schema_text = (real_meta / "schemas" / "board.schema.json"
                      ).read_text(encoding="utf-8")
    bc_schema = _json.loads(bc_schema_text)
    bc_schema["properties"]["som"]["properties"]["sku"]["pattern"] = (
        r"^E1M-(AEN[3-8]01|V2N10[12]|V2M10[12]|NX9[0-9]{3}|TST[0-9]{3})$"
    )
    (schemas / "board.schema.json").write_text(
        _json.dumps(bc_schema), encoding="utf-8")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    # Copy the renesas n44 SoC JSON so silicon refs resolve in the temp root.
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs_v2n / "n44.json")

    (e1m / f"{sku}.yaml").write_text(
        textwrap.dedent(sku_yaml_content).lstrip("\n"), encoding="utf-8")
    board_path = tmp_path / "board.yaml"
    board_path.write_text(
        textwrap.dedent(board_yaml_content).lstrip("\n"), encoding="utf-8")

    return alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)


_SYNTHETIC_V2N_WITH_ON_MODULE = """\
    schema_version: 1
    sku: E1M-TST001
    family: renesas-rzv2n
    silicon: renesas:rzv2n:n44
    silicon_variant: R9A09G056N44GBG
    on_module:
      silicon:            renesas:rzv2n:n44
      pmic_main:          act8760
      rtc_external:       rv3028c7
      secure_element:     optiga_trust_m
      supervisor_mcu:     gd32g553
    helper_firmware:
      - name: gd32_bridge
        chip: gd32g553
        firmware_path: firmware/gd32-bridge/build/gd32/gd32-bridge.bin
        flash_method:  swd_probe
        flash_args:
          interface: cmsis-dap
          target: gd32g553
          base: "0x08000000"
    topology:
      a55_cluster:
        app: alp-image-edge
        machine: e1m-tst001-a55
        toolchain: poky-glibc
      m33_sm:
        app: alp-stock-shim
        board: alp_e1m_tst001_m33_sm
        toolchain: arm-zephyr-eabi
    default_hw_rev: r1
    default_board: E1M-EVK
"""

_BOARD_WITH_SOM_ONLY = """\
    som:
      sku: E1M-TST001
      hw_rev: r1
    cores:
      m33_sm:
        os: zephyr
        app: ./m33
"""


def test_slice_alp_conf_emits_som_intrinsic_chips(tmp_path: Path) -> None:
    """_slice_alp_conf must include CONFIG_ALP_SDK_CHIP_* for every chip
    derived from on_module: + helper_firmware: when no board is present."""
    project = _make_som_only_project(
        tmp_path,
        _SYNTHETIC_V2N_WITH_ON_MODULE,
        _BOARD_WITH_SOM_ONLY,
    )
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    # All four on-module chip slugs must appear.
    assert "CONFIG_ALP_SDK_CHIP_ACT8760=y" in conf
    assert "CONFIG_ALP_SDK_CHIP_RV3028C7=y" in conf
    assert "CONFIG_ALP_SDK_CHIP_OPTIGA_TRUST_M=y" in conf
    assert "CONFIG_ALP_SDK_CHIP_GD32G553=y" in conf

    # The SoM-intrinsic comment header must appear.
    assert "SoM-intrinsic chip drivers" in conf

    # Subsystems driven by on-module chips (rv3028c7, optiga_trust_m,
    # act8760 are all I2C devices).
    assert "CONFIG_I2C=y" in conf


def test_slice_alp_conf_deduplicate_som_vs_board(tmp_path: Path) -> None:
    """A chip listed in both on_module: and board populated: must appear
    exactly once in the emitted conf (no duplicate CONFIG lines)."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    schemas = meta / "schemas"
    boards = meta / "boards"
    for d in (e1m, schemas, boards):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    # Relax the sku pattern so E1M-TST002 validates.
    import json as _json2
    bc_schema_text = (real_meta / "schemas" / "board.schema.json"
                      ).read_text(encoding="utf-8")
    bc_schema = _json2.loads(bc_schema_text)
    bc_schema["properties"]["som"]["properties"]["sku"]["pattern"] = (
        r"^E1M-(AEN[3-8]01|V2N10[12]|V2M10[12]|NX9[0-9]{3}|TST[0-9]{3})$"
    )
    (schemas / "board.schema.json").write_text(
        _json2.dumps(bc_schema), encoding="utf-8")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    # Copy SoC JSON so silicon ref renesas:rzv2n:n44 resolves in temp root.
    socs_v2n = meta / "socs" / "renesas" / "rzv2n"
    socs_v2n.mkdir(parents=True, exist_ok=True)
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs_v2n / "n44.json")

    # SoM preset lists rv3028c7 as on-module.
    (e1m / "E1M-TST002.yaml").write_text(textwrap.dedent("""
        schema_version: 1
        sku: E1M-TST002
        family: renesas-rzv2n
        silicon: renesas:rzv2n:n44
        silicon_variant: R9A09G056N44GBG
        on_module:
          silicon:        renesas:rzv2n:n44
          rtc_external:   rv3028c7
        helper_firmware: []
        topology:
          a55_cluster:
            app: alp-image-edge
            machine: e1m-tst002-a55
            toolchain: poky-glibc
          m33_sm:
            app: alp-stock-shim
            board: alp_e1m_tst002_m33_sm
            toolchain: arm-zephyr-eabi
        default_hw_rev: r1
        default_board: E1M-EVK
    """).lstrip("\n"), encoding="utf-8")

    # Board preset also lists rv3028c7 in populated:.
    (boards / "e1m-evk.yaml").write_text(textwrap.dedent("""
        name: E1M-EVK
        populated:
          rv3028c7: true
          bmi323: true
    """).lstrip("\n"), encoding="utf-8")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        som:
          sku: E1M-TST002
          hw_rev: r1
        preset: e1m-evk
        cores:
          m33_sm:
            os: zephyr
            app: ./m33
    """).lstrip("\n"), encoding="utf-8")

    project = alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    # rv3028c7 must appear exactly once.
    count = conf.count("CONFIG_ALP_SDK_CHIP_RV3028C7=y")
    assert count == 1, (
        f"rv3028c7 appears {count} times; expected exactly 1 (deduplicated)")

    # bmi323 is board-only; it must still appear.
    assert "CONFIG_ALP_SDK_CHIP_BMI323=y" in conf


def test_slice_alp_conf_tbd_values_excluded(tmp_path: Path) -> None:
    """on_module entries with value TBD must NOT generate CONFIG lines."""
    project = _make_som_only_project(
        tmp_path,
        """\
            schema_version: 1
            sku: E1M-TST001
            family: renesas-rzv2n
            silicon: renesas:rzv2n:n44
            silicon_variant: R9A09G056N44GBG
            on_module:
              silicon:      renesas:rzv2n:n44
              wifi_ble:     TBD
              ethernet_phy: TBD
              pmic_main:    act8760
            helper_firmware: []
            topology:
              a55_cluster:
                app: alp-image-edge
                machine: e1m-tst001-a55
                toolchain: poky-glibc
              m33_sm:
                app: alp-stock-shim
                board: alp_e1m_tst001_m33_sm
                toolchain: arm-zephyr-eabi
            default_hw_rev: r1
            default_board: E1M-EVK
        """,
        _BOARD_WITH_SOM_ONLY,
    )
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    assert "CONFIG_ALP_SDK_CHIP_TBD" not in conf, "TBD must never be emitted"
    assert "CONFIG_ALP_SDK_CHIP_ACT8760=y" in conf


def test_slice_alp_conf_no_on_module_no_som_block(tmp_path: Path) -> None:
    """A SoM preset without on_module: must emit no SoM-intrinsic chip block
    (no regression on the synthetic presets used by other tests)."""
    project = _make_som_only_project(
        tmp_path,
        """\
            schema_version: 1
            sku: E1M-TST001
            family: renesas-rzv2n
            silicon: renesas:rzv2n:n44
            silicon_variant: R9A09G056N44GBG
            topology:
              a55_cluster:
                app: alp-image-edge
                machine: e1m-tst001-a55
                toolchain: poky-glibc
              m33_sm:
                app: alp-stock-shim
                board: alp_e1m_tst001_m33_sm
                toolchain: arm-zephyr-eabi
            default_hw_rev: r1
            default_board: E1M-EVK
        """,
        _BOARD_WITH_SOM_ONLY,
    )
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    assert "SoM-intrinsic chip drivers" not in conf


def test_slice_alp_conf_real_v2n101(tmp_path: Path) -> None:
    """End-to-end: loading real E1M-V2N101 preset produces CONFIG lines
    for its on-module chip set and does not include TBD or silicon strings."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs = meta / "socs" / "renesas" / "rzv2n"
    schemas = meta / "schemas"
    for d in (e1m, socs, schemas):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    shutil.copy(real_meta / "socs" / "renesas" / "rzv2n" / "n44.json",
                socs / "n44.json")
    shutil.copy(real_meta / "e1m_modules" / "E1M-V2N101.yaml",
                e1m / "E1M-V2N101.yaml")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        som:
          sku: E1M-V2N101
          hw_rev: r1
        cores:
          m33_sm:
            os: zephyr
            app: ./m33
    """).lstrip("\n"), encoding="utf-8")

    project = alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    m33_slice = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, m33_slice)

    # Core V2N101 on-module chips.
    for chip in ("gd32g553", "optiga_trust_m", "rv3028c7", "tmp112",
                 "eeprom_24c128", "act8760", "da9292", "murata_lbee5hy2fy"):
        assert f"CONFIG_ALP_SDK_CHIP_{chip.upper()}=y" in conf, (
            f"missing CONFIG_ALP_SDK_CHIP_{chip.upper()}=y")

    # tps628640 is assembled: optional -- must NOT appear.
    assert "CONFIG_ALP_SDK_CHIP_TPS628640" not in conf

    # I2C subsystem from the many I2C chips.
    assert "CONFIG_I2C=y" in conf

    # No raw silicon string or TBD strings should appear in chip lines.
    for line in conf.splitlines():
        if line.startswith("CONFIG_ALP_SDK_CHIP_"):
            assert "TBD" not in line
            assert "RENESAS" not in line  # silicon slug must not appear


def test_slice_alp_conf_hw_info_eeprom_feature_overrides_defaults(
    tmp_path: Path,
) -> None:
    path = _write_board(tmp_path, """
som:
  sku: E1M-V2N101

preset: e1m-x-evk
cores:
  a55_cluster:
    os: "off"
  m33_sm:
    os: zephyr
    app: ./m33
    peripherals: [i2c]

chips:
  - eeprom_24c128

features:
  hw_info:
    eeprom:
      bus: e1m_i2c0
      addr_7bit: 0x54
      offset: 32
""")
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m33_sm"])

    assert "features.hw_info.eeprom" in conf
    assert "CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID=0" in conf
    assert "CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT=0x54" in conf
    assert "CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET=32" in conf


def test_slice_alp_conf_real_aen701(tmp_path: Path) -> None:
    """End-to-end: loading real E1M-AEN701 preset emits cc3501e, optiga_trust_m,
    rv3028c7, tmp112, eeprom_24c128; TBD ospi entries absent."""
    import alp_orchestrate
    meta = tmp_path / "metadata"
    e1m = meta / "e1m_modules"
    socs_alif = meta / "socs" / "alif" / "ensemble"
    schemas = meta / "schemas"
    for d in (e1m, socs_alif, schemas):
        d.mkdir(parents=True)

    real_meta = REPO / "metadata"
    shutil.copy(real_meta / "schemas" / "board.schema.json",
                schemas / "board.schema.json")
    shutil.copy(real_meta / "schemas" / "som-preset-v1.schema.json",
                schemas / "som-preset-v1.schema.json")
    shutil.copy(real_meta / "schemas" / "soc-spec-v1.schema.json",
                schemas / "soc-spec-v1.schema.json")
    # AEN SoC JSON for capability resolution.
    real_soc_dir = real_meta / "socs" / "alif" / "ensemble"
    if real_soc_dir.is_dir():
        for soc_f in real_soc_dir.iterdir():
            shutil.copy(soc_f, socs_alif / soc_f.name)
    shutil.copy(real_meta / "e1m_modules" / "E1M-AEN701.yaml",
                e1m / "E1M-AEN701.yaml")

    board_path = tmp_path / "board.yaml"
    board_path.write_text(textwrap.dedent("""
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            os: zephyr
            app: ./m55_hp
    """).lstrip("\n"), encoding="utf-8")

    project = alp_orchestrate.load_board_yaml(board_path, metadata_root=meta)
    m55_slice = project.cores["m55_hp"]
    conf = _slice_alp_conf(project, m55_slice)

    for chip in ("cc3501e", "optiga_trust_m", "rv3028c7", "tmp112",
                 "eeprom_24c128"):
        assert f"CONFIG_ALP_SDK_CHIP_{chip.upper()}=y" in conf, (
            f"missing CONFIG_ALP_SDK_CHIP_{chip.upper()}=y for AEN701")

    assert "CONFIG_ALP_SDK_CHIP_TBD" not in conf
    assert "SoM-intrinsic chip drivers" in conf


def test_slice_alp_conf_iot_aen_uses_cc3501e_provider(tmp_path: Path) -> None:
    """cores.*.iot resolves AEN Wi-Fi/BLE to the CC3501E bridge, while
    MQTT/TLS still emit the Zephyr protocol-library gates."""
    body = """
som:
  sku: E1M-AEN701

libraries:
  - name: mbedtls
    cores: [m55_hp]

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    iot: { wifi: true, mqtt: true, tls: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])

    assert "on_module.wifi_ble: cc3501e" in conf
    assert "CONFIG_ALP_SDK_CHIP_CC3501E=y" in conf
    assert "CONFIG_ALP_SDK_WIFI_CC3501E=y" in conf
    assert "CONFIG_ALP_SDK_BLE_CC3501E=y" in conf
    assert "CONFIG_MQTT_LIB=y" in conf
    assert "CONFIG_ALP_SDK_IOT_MQTT=y" in conf
    assert "CONFIG_TLS_CREDENTIALS=y" in conf
    assert "CONFIG_MQTT_LIB_TLS=y" in conf
    assert "CONFIG_MBEDTLS=y" in conf

    # AEN uses the exact bridge backend, not the generic Zephyr wifi_mgmt
    # or HCI paths.
    assert "CONFIG_ALP_SDK_IOT_WIFI=y" not in conf
    assert "CONFIG_ALP_SDK_BLE=y" not in conf


def test_slice_alp_conf_iot_cc3501e_chip_off_no_backend_lines(
    tmp_path: Path,
) -> None:
    """issue #874 item 2: the SoM's wireless provider is CC3501E, but this
    board variant DNIs the chip (`populated: { cc3501e: false }`) -- the
    CC3501E Wi-Fi/BLE bridge lines must not be emitted (they `depend on`
    ALP_SDK_CHIP_CC3501E, which resolves off on this variant), and a clear
    comment must explain why instead of a silently-dropped `=y` line."""
    body = """
som:
  sku: E1M-AEN701

name: cc3501e-dni-variant
populated:
  cc3501e: false

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    iot: { wifi: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])

    assert "CONFIG_ALP_SDK_CHIP_CC3501E=n" in conf
    assert "CONFIG_ALP_SDK_WIFI_CC3501E=y" not in conf
    assert "CONFIG_ALP_SDK_BLE_CC3501E=y" not in conf
    assert "does not populate the chip" in conf


def test_slice_alp_conf_iot_cc3501e_triple_overlap_populated_wins(
    tmp_path: Path,
) -> None:
    """issue #874 adversarial follow-up: on_module.wifi_ble: cc3501e (SoM-
    intrinsic) + `populated: { cc3501e: false }` (board DNI) + top-level
    `chips: [cc3501e]` (project-declared) all name the same chip.
    `populated: false` is authoritative-OFF -- a project `chips:` entry
    must NOT silently re-enable a chip the board just turned off.  The
    emitted CHIP line and the WIFI/BLE gate must AGREE (both off);
    `_resolve_chip_states` and `_emit_chips` must never independently
    derive a different answer for the same chip."""
    body = """
som:
  sku: E1M-AEN701

name: cc3501e-triple-overlap
populated:
  cc3501e: false

chips:
  - cc3501e

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    iot: { wifi: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])

    # Exactly one CHIP_CC3501E line, resolving off -- no y-then-n (or
    # y-then-n-then-y) self-contradiction.
    assert conf.count("CONFIG_ALP_SDK_CHIP_CC3501E=") == 1
    assert "CONFIG_ALP_SDK_CHIP_CC3501E=n" in conf
    assert "CONFIG_ALP_SDK_CHIP_CC3501E=y" not in conf

    # The WIFI/BLE gate must agree with the chip line above.
    assert "CONFIG_ALP_SDK_WIFI_CC3501E=y" not in conf
    assert "CONFIG_ALP_SDK_BLE_CC3501E=y" not in conf


def test_slice_alp_conf_iot_tls_only_emits_network_base(tmp_path: Path) -> None:
    """issue #874 item 1: `iot.tls: true` alone (no `wifi:`/`mqtt:`) must
    still emit the networking base -- CONFIG_TLS_CREDENTIALS depends on
    NETWORKING/NET_SOCKETS, which previously only the wifi/mqtt branches
    emitted, so a TLS-only slice silently resolved TLS_CREDENTIALS to n."""
    body = """
som:
  sku: E1M-NX9101

libraries:
  - name: mbedtls
    cores: [m33]

cores:
  m33:
    os: zephyr
    app: ./m33
    iot: { tls: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m33"])

    assert "CONFIG_NETWORKING=y" in conf
    assert "CONFIG_NET_IPV4=y" in conf
    assert "CONFIG_NET_SOCKETS=y" in conf
    assert "CONFIG_TLS_CREDENTIALS=y" in conf
    # No wifi/mqtt requested -- those gates must stay off.
    assert "CONFIG_WIFI=y" not in conf
    assert "CONFIG_ALP_SDK_IOT_WIFI=y" not in conf
    assert "CONFIG_MQTT_LIB=y" not in conf


def test_slice_alp_conf_no_inference_declared_emits_nothing(
    tmp_path: Path,
) -> None:
    """issue #874 item 4: a slice that never declares `cores.<id>.inference:`
    must not get any INFERENCE_* line -- BACKEND_TFLM's `depends on
    TENSORFLOW_LITE_MICRO && CPP` previously resolved silently to n on
    every such slice (e.g. a plain blinky/tone-generator example) with a
    misleading `=y` line sitting in its alp.conf."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])

    assert "INFERENCE" not in conf
    assert "CONFIG_TENSORFLOW_LITE_MICRO" not in conf


def test_slice_alp_conf_inference_declared_emits_tflm_plus_deps(
    tmp_path: Path,
) -> None:
    """issue #874 items 4+5: `cores.<id>.inference:` (any key, e.g.
    `default_arena_kib:` -- the same signal validate.py's Rule 4 already
    keys its arena/heap OOM warning off of) turns the inference section
    back on, and BACKEND_TFLM's genuinely-external deps (CONFIG_CPP,
    CONFIG_TENSORFLOW_LITE_MICRO) are emitted alongside it since west.yml
    pulls the tflite-micro module in unconditionally."""
    body = """
som:
  sku: E1M-AEN701

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp
    inference: { default_arena_kib: 128 }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m55_hp"])

    assert "CONFIG_CPP=y" in conf
    assert "CONFIG_TENSORFLOW_LITE_MICRO=y" in conf
    assert "CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y" in conf
    assert "CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y" in conf
    assert "CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y" in conf


def test_slice_alp_conf_iot_unknown_provider_uses_generic_zephyr(
    tmp_path: Path,
) -> None:
    """A SoM whose wireless provider is still TBD emits the generic Zephyr
    networking / MQTT / TLS / BLE gates rather than a false provider."""
    body = """
som:
  sku: E1M-NX9101

libraries:
  - name: mbedtls
    cores: [m33]

cores:
  m33:
    os: zephyr
    app: ./m33
    iot: { wifi: true, mqtt: true, tls: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_alp_conf(project, project.cores["m33"])

    for expected in (
        "CONFIG_NETWORKING=y",
        "CONFIG_NET_IPV4=y",
        "CONFIG_NET_SOCKETS=y",
        "CONFIG_WIFI=y",
        "CONFIG_NET_MGMT=y",
        "CONFIG_NET_MGMT_EVENT=y",
        "CONFIG_NET_L2_WIFI_MGMT=y",
        "CONFIG_ALP_SDK_IOT_WIFI=y",
        "CONFIG_NET_TCP=y",
        "CONFIG_MQTT_LIB=y",
        "CONFIG_ALP_SDK_IOT_MQTT=y",
        "CONFIG_TLS_CREDENTIALS=y",
        "CONFIG_MQTT_LIB_TLS=y",
        "CONFIG_BT=y",
        "CONFIG_BT_PERIPHERAL=y",
        "CONFIG_BT_CENTRAL=y",
        "CONFIG_ALP_SDK_BLE=y",
    ):
        assert expected in conf

    assert "CONFIG_ALP_SDK_WIFI_CC3501E=y" not in conf
    assert "CONFIG_ALP_SDK_BLE_CC3501E=y" not in conf


def test_slice_local_conf_iot_v2n_murata_linux_handoff(tmp_path: Path) -> None:
    """V2N's Murata/CYW provider is Linux-owned: local.conf gets stable
    userland/runtime deps and leaves kernel/firmware to the BSP layer."""
    body = """
som:
  sku: E1M-V2N101

libraries:
  - name: mbedtls
    cores: [a55_cluster]

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    iot: { wifi: true, mqtt: true, tls: true, ble: true }
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    conf = _slice_local_conf(project, project.cores["a55_cluster"])

    assert "on_module.wifi_ble: murata_lbee5hy2fy" in conf
    assert "BSP/machine recipes supply kernel/firmware packages" in conf
    assert (
        'IMAGE_INSTALL:append = " wpa-supplicant iw wireless-regdb '
        'bluez5 ca-certificates"'
    ) in conf
    assert 'PACKAGECONFIG:append:pn-alp-sdk = " mqtt security"' in conf


