# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""aen-power-stop tally.h: the pass/fail totals that survive STOP cold-boot wakes (#2965)."""
import shutil
import subprocess
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[2] / "examples" / "aen" / "aen-power-stop" / "src"

PROG = r"""
#include <stdio.h>
#include "tally.h"
int main(void)
{
	/* Three cold boots: each one starts from 0, reloads, adds its own verdicts, stores. */
	unsigned p, f, tp = 0, tf = 0;
	const unsigned boot_pass[3] = { 4, 5, 5 }, boot_fail[3] = { 1, 0, 0 };
	uint8_t cell = 0;
	for (int i = 0; i < 3; i++) {
		tally_unpack(cell, &p, &f);          /* reload after the "wake" */
		p += boot_pass[i]; f += boot_fail[i];
		cell = tally_pack(p, f);             /* what cycle_set() stores */
	}
	tally_unpack(cell, &tp, &tf);
	printf("%u %u %u %u\n", tp, tf, (unsigned)tally_pack(99, 99), (unsigned)tally_pack(0, 0));
	return 0;
}
"""


@pytest.mark.skipif(shutil.which("cc") is None, reason="no host C compiler")
def test_tally_accumulates_across_boots_and_saturates(tmp_path):
    (tmp_path / "t.c").write_text(PROG, encoding="utf-8")
    subprocess.run(["cc", "-Wall", "-Werror", "-I", str(SRC), str(tmp_path / "t.c"),
                    "-o", str(tmp_path / "t")], check=True)
    out = subprocess.run([str(tmp_path / "t")], capture_output=True, text=True, encoding="utf-8", check=True).stdout
    # 14 passes / 1 fail over the run (the old line would have said pass=5 fail=0);
    # 255 = both fields saturated; 0 = an empty tally.
    assert out.split() == ["14", "1", "255", "0"]
