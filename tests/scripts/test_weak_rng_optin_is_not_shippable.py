# SPDX-License-Identifier: Apache-2.0
"""`ALP_SDK_ALLOW_TEST_ENTROPY` must never reach a configuration a customer ships.

Turning mbedTLS' PSA crypto core on (`ALP_SDK_MBEDTLS_PSA_CRYPTO`, #2173)
selects `CSPRNG_NEEDED`, and Zephyr satisfies that with a generator it
documents as not random whenever no true entropy driver is bound -- which is
every Alif Ensemble target today, because no Ensemble entropy driver exists
in this tree or upstream (#2192). `zephyr/CMakeLists.txt` therefore REFUSES
such a build unless `CONFIG_ALP_SDK_ALLOW_TEST_ENTROPY=y` acknowledges it.

The predictable way to defeat that is to make the red build green by putting
the opt-in somewhere it is inherited: an example's `prj.conf` (which
`tan init --from-example` copies verbatim into a customer's tree), a board
file, or a Kconfig default. Then every scaffolded product silently ships TLS
on a predictable RNG, which is the exact outcome the refusal exists to stop.

So the opt-in is only legal where it names a build that never ships: a
twister scenario's `extra_configs`, a `native_sim.conf` (never applied to a
real-silicon build), or anything under `tests/`.
"""

from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SYMBOL = "ALP_SDK_ALLOW_TEST_ENTROPY"

#: Files that may enable it, as regexes over repo-relative POSIX paths.
_ALLOWED = (
    re.compile(r"^examples/[^/]+/[^/]+/testcase\.yaml$"),
    re.compile(r"^examples/[^/]+/[^/]+/native_sim\.conf$"),
    re.compile(r"^tests/.*$"),
)

#: The declaration itself, its CMake guard, and the docs that explain them are
#: allowed to NAME the symbol -- this test is about ENABLING it.
_DECLARATION_SITES = (
    "zephyr/Kconfig.alp-libraries",
    "zephyr/CMakeLists.txt",
)


def _tracked_files() -> list[str]:
    git = shutil.which("git")
    if git is None:
        pytest.skip("git is not on PATH, so the tracked-file list is unavailable")
    out = subprocess.run(
        [git, "-C", str(_REPO_ROOT), "ls-files"],
        capture_output=True, text=True, encoding="utf-8", check=True,
    )
    return out.stdout.splitlines()


def _enables_it(path: Path) -> bool:
    """True when the file turns the symbol ON, not merely mentions it."""
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return False
    return bool(re.search(rf"^\s*-?\s*\"?CONFIG_{_SYMBOL}=y", text, re.MULTILINE))


def test_optin_is_only_enabled_where_the_build_never_ships() -> None:
    offenders = []
    for rel in _tracked_files():
        if rel in _DECLARATION_SITES:
            continue
        if not _enables_it(_REPO_ROOT / rel):
            continue
        if not any(pattern.match(rel) for pattern in _ALLOWED):
            offenders.append(rel)

    assert not offenders, (
        f"CONFIG_{_SYMBOL}=y is set in {offenders}, which is neither a twister "
        f"scenario, a native_sim.conf, nor a test. Setting it there makes every "
        f"build inheriting that file -- including a customer's `tan init "
        f"--from-example` copy -- ship TLS on a non-cryptographic RNG (#2192). "
        f"If a build genuinely cannot bind real entropy, the answer is an "
        f"entropy driver, not widening this opt-in."
    )


def test_no_example_prj_conf_enables_it() -> None:
    """The scaffolding-copy case, stated on its own so a failure names it.

    `tan init --from-example` copies one example directory; `prj.conf` goes
    with it and `testcase.yaml` does not, which is exactly why the opt-in
    lives on the scenario.
    """
    offenders = [
        rel for rel in _tracked_files()
        if rel.startswith("examples/") and rel.endswith("/prj.conf")
        and _enables_it(_REPO_ROOT / rel)
    ]
    assert not offenders, (
        f"{offenders} enable CONFIG_{_SYMBOL}. prj.conf is copied into a "
        f"customer's tree by `tan init --from-example`, so the acknowledgement "
        f"would travel with the template into a shipped product."
    )


def test_the_refusal_guard_is_still_wired() -> None:
    """Guard the guard: the opt-in only means anything while CMake enforces it."""
    cmakelists = (_REPO_ROOT / "zephyr" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert re.search(
        r"if\s*\(\s*CONFIG_ALP_SDK_MBEDTLS_WEAK_RNG\s+AND\s+NOT\s+"
        rf"CONFIG_{_SYMBOL}\s*\)",
        cmakelists,
    ), (
        "zephyr/CMakeLists.txt no longer refuses a weak-RNG build. Without that "
        f"guard CONFIG_{_SYMBOL} is decorative and the AEN TLS examples go back "
        "to building silently on a predictable generator (#2192)."
    )
    assert "FATAL_ERROR" in cmakelists.split("CONFIG_ALP_SDK_MBEDTLS_WEAK_RNG", 1)[1][:800], (
        "the weak-RNG guard must be a FATAL_ERROR -- a warning is what this "
        "change replaced."
    )
