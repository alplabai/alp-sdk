"""Pins for #2734: gd32-bridge must not be unbindable; no --unbind OTA path; panic reboots."""

from pathlib import Path

R = Path(__file__).resolve().parents[2]
KDIR = R / "meta-alp-sdk/recipes-kernel/linux"


def test_0005_suppresses_bind_attrs():
    p = (KDIR / "linux-renesas/0005-gpio-add-gd32-bridge-expander-driver.patch").read_text(
        encoding="utf-8"
    )
    drv = p[p.index("static struct i2c_driver gd32_bridge_gpio_driver") :]
    assert ".suppress_bind_attrs = true," in drv.split("};", 1)[0]


def test_gd32_ota_host_has_no_unbind_path():
    c = (R / "tools/gd32-ota-host/gd32_ota_host.c").read_text(encoding="utf-8")
    for bad in ("--unbind", "kernel_unbind", "kernel_rebind", "/unbind", "/bind"):
        assert bad not in c, bad
    assert "reboot required" in c
    assert "--unbind" not in (R / "tools/gd32-ota-host/README.md").read_text(encoding="utf-8")


def test_panic_cfg_in_src_uri():
    assert "file://panic.cfg" in (KDIR / "linux-renesas_%.bbappend").read_text(encoding="utf-8")
    cfg = (KDIR / "linux-renesas/panic.cfg").read_text(encoding="utf-8")
    assert "CONFIG_PANIC_TIMEOUT=10" in cfg.splitlines()
