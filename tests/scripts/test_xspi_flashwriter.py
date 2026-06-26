"""Unit tests for the xspi_flashwriter flash backend (dry-run only; real
SCIF serial write is HW-gated)."""

from pathlib import Path

# tests/scripts/conftest.py puts scripts/ on sys.path, so flash_backends
# imports as a top-level package (matches tests/scripts/test_flash_backends.py).
from flash_backends import FlashContext, lookup
from flash_backends.xspi_flashwriter import BACKEND


def _ctx(tmp_path, flash_args, dry_run):
    art = tmp_path / "bl2_bp_spi-rzv2n-evk.bin"
    art.write_bytes(b"BL2")
    return FlashContext(artefact_path=art, flash_args=flash_args,
                        core_id="bl2", sku="E1M-V2N101", dry_run=dry_run)


def test_registered_under_name():
    assert lookup("xspi_flashwriter") is BACKEND


def test_dry_run_plans_partition_write(tmp_path):
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": "mtd0", "port": "COM24"}, True))
    assert r.ok
    assert "mtd0" in r.message
    assert "bl2_bp_spi-rzv2n-evk.bin" in r.message
    assert r.command  # a non-empty planned sequence


def test_requires_partition(tmp_path):
    r = BACKEND.flash(_ctx(tmp_path, {"port": "COM24"}, True))
    assert not r.ok
    assert "flash_partition" in r.message


def test_confirm_false_still_dry_runs_when_not_dry_run(tmp_path):
    # ctx.dry_run False but no confirm => must NOT touch hardware; plans instead.
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": "mtd1"}, False))
    assert r.ok
    assert "confirm" in r.message.lower()


def test_real_write_is_hw_gated(tmp_path):
    # confirm True + real intent => the real path is HW-gated, returns ok=False
    # with a clear message (regardless of whether pyserial is installed).
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": "mtd0", "confirm": True,
                                      "port": "COM24", "flash_writer": "missing.mot"}, False))
    assert not r.ok
