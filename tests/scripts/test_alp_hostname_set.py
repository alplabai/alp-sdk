"""alp-hostname-set.sh turns a raw 24-byte EEPROM SKU field into a hostname.
That sanitiser is the only untrusted-input surface in the hostname unit, so
drive the real script over hostile inputs and assert the result is always a
legal DNS label."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "meta-alp-sdk" / "recipes-core" / "alp-hostname" / "files" / "alp-hostname-set.sh"
SH = shutil.which("sh")

pytestmark = pytest.mark.skipif(SH is None, reason="needs a POSIX sh")

LABEL = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def _run(tmp_path: Path, raw: bytes):
    prop = tmp_path / "alp,sku"
    prop.write_bytes(raw)
    etc = tmp_path / "hostname"
    kern = tmp_path / "kernel_hostname"
    env = {**os.environ, "ALP_SKU_PROP": str(prop), "ALP_HOSTNAME_FILE": str(etc),
           "ALP_KERNEL_HOSTNAME": str(kern)}
    proc = subprocess.run([SH, str(SCRIPT)], env=env, capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    return (kern.read_text().strip() if kern.exists() else None,
            etc.read_text().strip() if etc.exists() else None)


@pytest.mark.parametrize("raw, expected", [
    (b"E1M-V2M103\0\0\0\0\0\0\0\0\0\0\0\0\0\0", "e1m-v2m103"),
    (b"E1M/V2N101", "e1m-v2n101"),
    (b"A;B", "a-b"),
    (b"a b", "a-b"),
    (b"a\nb", "a-b"),
    (b"$(reboot)", "reboot"),
    (b"A" * 24, "a" * 24),
    ("ÀÉ-x".encode("utf-8"), "x"),
])
def test_hostile_inputs_sanitise_to_a_dns_label(tmp_path, raw, expected):
    kern, etc = _run(tmp_path, raw)
    assert kern == expected
    assert etc == expected
    assert LABEL.match(kern) and "--" not in kern


@pytest.mark.parametrize("raw", [b"", b"--", b"---", b"    ", b";;;", b"\0" * 24])
def test_inputs_with_nothing_usable_leave_hostname_alone(tmp_path, raw):
    kern, etc = _run(tmp_path, raw)
    assert kern is None and etc is None
