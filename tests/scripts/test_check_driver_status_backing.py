"""Unit tests for scripts/check_driver_status_backing.py."""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_driver_status_backing.py"


def _load():
    spec = importlib.util.spec_from_file_location("check_driver_status_backing", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _preset(root: Path, name: str, body: str):
    presets = root / "metadata" / "e1m_modules"
    presets.mkdir(parents=True, exist_ok=True)
    (presets / f"{name}.yaml").write_text(body, encoding="utf-8")


def _mbox_driver(root: Path, filename: str, content: str = "// stub\n"):
    mbox = root / "zephyr" / "drivers" / "mbox"
    mbox.mkdir(parents=True, exist_ok=True)
    (mbox / filename).write_text(content, encoding="utf-8")


# -- mailbox.driver_status ---------------------------------------------------


def test_mailbox_backed_by_known_driver_file_passes(tmp_path):
    mod = _load()
    _mbox_driver(tmp_path, "mbox_renesas_rz_mhu_b.c")
    _preset(
        tmp_path,
        "E1M-V2N101",
        "mailbox:\n  controller: renesas_mhu\n  driver_status: partial\n",
    )
    assert mod.check(tmp_path) == []


def test_mailbox_overclaimed_controller_with_no_backing_file_fails(tmp_path):
    """Non-vacuity: a driver_status claim naming a controller nothing backs."""
    mod = _load()
    # No zephyr/drivers/mbox/ tree at all -- the overclaim this gate exists to stop.
    _preset(
        tmp_path,
        "E1M-V2N101",
        "mailbox:\n  controller: renesas_mhu\n  driver_status: complete\n",
    )
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "mailbox.driver_status" in issues[0]
    assert "renesas_mhu" in issues[0]


def test_mailbox_planned_does_not_require_backing(tmp_path):
    mod = _load()
    _preset(
        tmp_path,
        "E1M-V2N101",
        "mailbox:\n  controller: renesas_mhu\n  driver_status: planned\n",
    )
    assert mod.check(tmp_path) == []


def test_mailbox_none_does_not_require_backing(tmp_path):
    mod = _load()
    _preset(
        tmp_path,
        "E1M-V2N101",
        "mailbox:\n  controller: TBD\n  driver_status: none\n",
    )
    assert mod.check(tmp_path) == []


# -- on_module.{nor_flash,emmc}_driver_status --------------------------------


def test_nor_flash_none_passes_with_no_driver_present(tmp_path):
    mod = _load()
    _preset(
        tmp_path,
        "E1M-V2N101",
        "on_module:\n  nor_flash_driver_status: none\n",
    )
    assert mod.check(tmp_path) == []


def test_nor_flash_overclaimed_with_no_renesas_driver_fails(tmp_path):
    """Non-vacuity: the field claims a driver that does not exist (#1169)."""
    mod = _load()
    _preset(
        tmp_path,
        "E1M-V2N101",
        "on_module:\n  nor_flash_driver_status: complete\n",
    )
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "nor_flash_driver_status" in issues[0]


def test_nor_flash_backed_by_renesas_named_file_passes(tmp_path):
    mod = _load()
    flash_dir = tmp_path / "zephyr" / "drivers" / "flash"
    flash_dir.mkdir(parents=True)
    (flash_dir / "flash_renesas_rz_xspi.c").write_text("// stub\n", encoding="utf-8")
    _preset(
        tmp_path,
        "E1M-V2N101",
        "on_module:\n  nor_flash_driver_status: complete\n",
    )
    assert mod.check(tmp_path) == []


def test_nor_flash_backed_by_dt_drv_compat_renesas_file_passes(tmp_path):
    """A non-Renesas-named file whose DT_DRV_COMPAT is renesas-prefixed still backs it."""
    mod = _load()
    flash_dir = tmp_path / "zephyr" / "drivers" / "flash"
    flash_dir.mkdir(parents=True)
    (flash_dir / "flash_xspi.c").write_text(
        "#define DT_DRV_COMPAT renesas_rz_xspi\n", encoding="utf-8"
    )
    _preset(
        tmp_path,
        "E1M-V2N101",
        "on_module:\n  nor_flash_driver_status: complete\n",
    )
    assert mod.check(tmp_path) == []


def test_nor_flash_bare_renesas_comment_mention_does_not_pass(tmp_path):
    """Non-vacuity: a comment merely naming the vendor is not a driver binding."""
    mod = _load()
    flash_dir = tmp_path / "zephyr" / "drivers" / "flash"
    flash_dir.mkdir(parents=True)
    (flash_dir / "flash_dwc.c").write_text(
        "/* not Renesas-backed on this SoC, but see the renesas eval board note. */\n"
        "#define DT_DRV_COMPAT snps_dwc_flash\n",
        encoding="utf-8",
    )
    _preset(
        tmp_path,
        "E1M-V2N101",
        "on_module:\n  nor_flash_driver_status: complete\n",
    )
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "nor_flash_driver_status" in issues[0]


# -- soc_peripheral_instances[].driver_status --------------------------------


def test_soc_peripheral_instance_overclaimed_fails(tmp_path):
    """Non-vacuity: an instance claims a driver no source mentions."""
    mod = _load()
    _preset(
        tmp_path,
        "E1M-V2N101",
        "soc_peripheral_instances:\n"
        "  - { instance: rspi0, class: spi, silicon_peripheral: \"RSPI0_*\", "
        "driver_status: complete }\n",
    )
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "rspi0" in issues[0]


def test_soc_peripheral_instance_backed_passes(tmp_path):
    mod = _load()
    spi_dir = tmp_path / "zephyr" / "drivers" / "spi"
    spi_dir.mkdir(parents=True)
    (spi_dir / "spi_renesas_rz_spi_b.c").write_text(
        "#define DT_DRV_COMPAT renesas_rz_rspi0\n", encoding="utf-8"
    )
    _preset(
        tmp_path,
        "E1M-V2N101",
        "soc_peripheral_instances:\n"
        "  - { instance: rspi0, class: spi, silicon_peripheral: \"RSPI0_*\", "
        "driver_status: partial }\n",
    )
    assert mod.check(tmp_path) == []


def test_soc_peripheral_instance_prefix_of_unrelated_identifier_does_not_pass(tmp_path):
    """Non-vacuity: instance `sd1` must not match inside pin-name `SD1_CD`.

    Regression for the real-tree false positive: zephyr/drivers/spi/
    spi_renesas_rz_sci_b.c mentions the unrelated pin name SD1_CD in
    comments; that is not a driver for the `sd1` sdio instance.
    """
    mod = _load()
    spi_dir = tmp_path / "zephyr" / "drivers" / "spi"
    spi_dir.mkdir(parents=True)
    (spi_dir / "spi_renesas_rz_sci_b.c").write_text(
        "/* held against Linux's SD1_CD pin claim on port 9 */\n", encoding="utf-8"
    )
    _preset(
        tmp_path,
        "E1M-V2N101",
        "soc_peripheral_instances:\n"
        "  - { instance: sd1, class: sdio, silicon_peripheral: \"SD1CLK/SD1CMD/SD1DATx\", "
        "driver_status: complete }\n",
    )
    issues = mod.check(tmp_path)
    assert len(issues) == 1
    assert "sd1" in issues[0]


def test_soc_peripheral_instance_none_passes(tmp_path):
    mod = _load()
    _preset(
        tmp_path,
        "E1M-V2N101",
        "soc_peripheral_instances:\n"
        "  - { instance: rspi0, class: spi, silicon_peripheral: \"RSPI0_*\", "
        "driver_status: none }\n",
    )
    assert mod.check(tmp_path) == []


def test_repo_tree_is_clean():
    """The committed tree must satisfy the gate."""
    mod = _load()
    assert mod.check(REPO) == []
