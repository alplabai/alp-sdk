# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""`--dst-module` must precede `apply`, in both bootstrap scripts.

`--dst-module` is a flag of `west patch` itself, not of its `apply`
SUBCOMMAND. west's own usage line puts every flag before the subcommand:

    usage: west patch [-h] [-b DIR] [-l FILE] [-w DIR] [-sm MODULE]
                      [-dm MODULE] <subcommand> ...

Written the other way round the command does not run at all:

    west patch: error: unexpected arguments: ['--dst-module', 'mcuboot']

exit 2. That shipped on `dev` and failed the `Bootstrap Zephyr workspace` step
of every `alp-build` matrix leg. A string check rather than an execution test
because the failure is in the ARGV the script builds, and reproducing it needs
a real west workspace this suite does not have.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

#: The two scripts that invoke `west patch` per module.
SCRIPTS = ("scripts/bootstrap.sh", "scripts/bootstrap.ps1")


@pytest.mark.parametrize("rel", SCRIPTS)
def test_dst_module_precedes_the_apply_subcommand(rel):
    text = (ROOT / rel).read_text(encoding="utf-8")
    # The broken form, in either script's quoting style.
    broken = re.findall(r"patch\s+apply\s+--dst-module", text)
    assert broken == [], (
        f"{rel} invokes `west patch apply --dst-module`, which west rejects "
        f"with `unexpected arguments`. `--dst-module` is a flag of `west "
        f"patch`, so it goes before the `apply` subcommand."
    )


@pytest.mark.parametrize("rel", SCRIPTS)
def test_the_per_module_invocation_is_present_and_correctly_ordered(rel):
    """Positive control: without it, deleting the call entirely would pass."""
    text = (ROOT / rel).read_text(encoding="utf-8")
    assert re.search(r"patch\s+--dst-module\s+\S+\s+apply", text), (
        f"{rel} no longer contains a `west patch --dst-module <mod> apply` "
        f"invocation -- the per-module apply is what makes bootstrap "
        f"idempotent on a partially-patched workspace (#1392)."
    )
