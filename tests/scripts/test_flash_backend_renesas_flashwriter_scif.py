"""Unit tests for the renesas_flashwriter_scif flash backend. No hardware:
it only plans, and refuses a confirmed write."""

import pytest

# tests/scripts/conftest.py puts scripts/ on sys.path, so flash_backends
# imports as a top-level package (matches tests/scripts/test_flash_backends.py).
from flash_backends import FlashContext, lookup
from flash_backends.renesas_flashwriter_scif import BACKEND


def _ctx(tmp_path, flash_args, dry_run, core_id="bl2"):
    art = tmp_path / "bl2_bp_spi-rzv2n-evk.bin"
    art.write_bytes(b"BL2")
    return FlashContext(artefact_path=art, flash_args=flash_args,
                        core_id=core_id, sku="E1M-V2N101", dry_run=dry_run)


def test_registered():
    assert lookup("renesas_flashwriter_scif") is BACKEND
    assert lookup("xspi_flashwriter") is None


def test_dry_run_plans_partition_write(tmp_path):
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": "mtd0", "port": "ttyTEST"}, True))
    assert r.ok
    assert "xSPI mtd0" in r.message
    assert "bl2_bp_spi-rzv2n-evk.bin" in r.message
    assert r.command  # a non-empty planned sequence


def test_dry_run_plans_emmc_boot1(tmp_path):
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": "emmc:boot1"}, True, core_id="fip"))
    assert r.ok
    assert "eMMC boot1" in r.message
    assert "partition=emmc:boot1" in r.command


def test_requires_partition(tmp_path):
    r = BACKEND.flash(_ctx(tmp_path, {"port": "ttyTEST"}, True))
    assert not r.ok
    assert "flash_partition" in r.message


def test_confirm_false_still_dry_runs_when_not_dry_run(tmp_path):
    # ctx.dry_run False but no confirm => must NOT touch hardware; plans instead.
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": "mtd1"}, False))
    assert r.ok
    assert "confirm" in r.message.lower()


@pytest.mark.parametrize("target", ["mtd0", "mtd1", "emmc:boot1"])
def test_real_write_is_refused(tmp_path, target):
    r = BACKEND.flash(_ctx(tmp_path, {"flash_partition": target, "confirm": True,
                                      "port": "ttyTEST", "flash_writer": "w.mot"}, False))
    assert not r.ok
    assert "not implemented" in r.message and "provision_som.py run" in r.message
