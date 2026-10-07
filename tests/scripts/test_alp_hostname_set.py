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


def _run(tmp_path: Path, raw: bytes, serial: bytes | None = None):
    prop = tmp_path / "alp,sku"
    prop.write_bytes(raw)
    sprop = tmp_path / "alp,serial"
    if serial is not None:
        sprop.write_bytes(serial)
    etc = tmp_path / "hostname"
    kern = tmp_path / "kernel_hostname"
    env = {**os.environ, "ALP_SKU_PROP": str(prop), "ALP_SERIAL_PROP": str(sprop),
           "ALP_HOSTNAME_FILE": str(etc), "ALP_KERNEL_HOSTNAME": str(kern)}
    proc = subprocess.run([SH, str(SCRIPT)], env=env, capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    return (kern.read_text(encoding="utf-8").strip() if kern.exists() else None,
            etc.read_text(encoding="utf-8").strip() if etc.exists() else None)


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


@pytest.mark.parametrize("serial, expected", [
    (b"2026W38-0001\0", "e1m-v2m103-2026w38-0001"),
    (b"2026W39-0001", "e1m-v2m103-2026w39-0001"),   # same index, other week
    (b"$(x);\n", "e1m-v2m103-x"),
    (b"---", "e1m-v2m103"),                          # nothing usable: SKU only
    (b"", "e1m-v2m103"),
])
def test_serial_suffix(tmp_path, serial, expected):
    kern, etc = _run(tmp_path, b"E1M-V2M103\0", serial)
    assert kern == expected and etc == expected
    assert LABEL.match(kern) and len(kern) <= 63 and "--" not in kern


def test_no_serial_property_keeps_sku_name(tmp_path):
    kern, _ = _run(tmp_path, b"E1M-V2M103\0", None)
    assert kern == "e1m-v2m103"


PROMPT = SCRIPT.parent / "alp-prompt.sh"


def _banner(tmp_path: Path, raw: bytes, serial: bytes | None) -> str | None:
    issue = tmp_path / "alp-module.issue"
    prop = tmp_path / "alp,sku"
    prop.write_bytes(raw)
    sprop = tmp_path / "alp,serial"
    if serial is not None:
        sprop.write_bytes(serial)
    env = {**os.environ, "ALP_SKU_PROP": str(prop), "ALP_SERIAL_PROP": str(sprop),
           "ALP_HOSTNAME_FILE": str(tmp_path / "hostname"),
           "ALP_KERNEL_HOSTNAME": str(tmp_path / "kernel_hostname"), "ALP_ISSUE_FILE": str(issue)}
    proc = subprocess.run([SH, str(SCRIPT)], env=env, capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    return issue.read_text(encoding="utf-8") if issue.exists() else None


def test_banner_shows_module_and_serial_in_original_case(tmp_path):
    assert _banner(tmp_path, b"E1M-V2M103\0", b"2026W38-0006\0") == "Module: E1M-V2M103  Serial: 2026W38-0006\n"


def test_banner_without_serial_shows_module_only(tmp_path):
    assert _banner(tmp_path, b"E1M-V2M103\0", None) == "Module: E1M-V2M103\n"


def test_blank_manifest_writes_no_banner(tmp_path):
    assert _banner(tmp_path, b"\0" * 24, b"\0" * 12) is None


DISTRO_PS1 = r"\u@\h:\w\$ "     # what the distro profile sets


def _prompt(tmp_path: Path, hostname: str, ps1: str | None) -> str:
    hf = tmp_path / "host"
    hf.write_text(hostname + "\n", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if k != "PS1"}
    env["ALP_PROMPT_HOSTNAME_FILE"] = str(hf)
    # A non-interactive shell drops PS1 from its environment, so seed it in the script.
    # dash gives even a non-interactive shell a default PS1, so the "no PS1" case unsets it.
    seed = 'PS1=$T_PS1; ' if ps1 is not None else "unset PS1; "
    env["T_PS1"] = ps1 or ""
    proc = subprocess.run([SH, "-c", f'{seed}. "{PROMPT}"; printf "%s" "${{PS1-unset}}"'],
                          env=env, capture_output=True, text=True, encoding="utf-8")
    assert proc.returncode == 0, proc.stderr
    return proc.stdout


def test_prompt_drops_the_serial_suffix(tmp_path):
    assert _prompt(tmp_path, "e1m-v2m103-2026w38-0006", DISTRO_PS1) == r"\u@e1m-v2m103:\w\$ "


@pytest.mark.parametrize("hostname", ["e1m-v2m103", "e1m-v2m103-a55", "alp-e1m", "e1m-v2m103-2026w38", "",
                                      "foo-1234w56-7anything", "e1m-v2m103-2026w38-00061"])
def test_prompt_untouched_without_a_serial_suffix(tmp_path, hostname):
    assert _prompt(tmp_path, hostname, DISTRO_PS1) == DISTRO_PS1


def test_prompt_strips_a_letter_first_index(tmp_path):
    assert _prompt(tmp_path, "e1m-v2m103-2026w38-a006", DISTRO_PS1) == r"\u@e1m-v2m103:\w\$ "


def test_prompt_untouched_in_a_non_interactive_shell(tmp_path):
    assert _prompt(tmp_path, "e1m-v2m103-2026w38-0006", None) == "unset"


def test_banner_is_reachable_by_agetty():
    """util-linux 2.39.3 agetty reads /etc/issue.d/*.issue only when /etc/issue exists
    (the image ships one) and ignores /run/issue.d then, so the recipe must link a
    *.issue file in /etc/issue.d at the path the script writes."""
    recipe = (SCRIPT.parents[1] / "alp-hostname_0.1.bb").read_text(encoding="utf-8")
    m = re.search(r"ln -sf (\S+) \$\{D\}\$\{sysconfdir\}/issue\.d/(\S+)", recipe)
    assert m, "recipe does not link into /etc/issue.d"
    target, name = m.groups()
    assert name.endswith(".issue") and not name.startswith(".")
    default = re.search(r"ALP_ISSUE_FILE:-([^}]+)\}", SCRIPT.read_text(encoding="utf-8"))
    assert default and default.group(1) == target
