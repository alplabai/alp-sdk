# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Flash runner for the E1M-V2N101 (RZ/V2N) Cortex-M33 system-manager
# image.
#
# rzv2n_mtd_flash (DEFAULT flasher, and the ONLY runner wired here) --
# a single-stage raw write to xSPI mtd1 over SSH to the booted A55
# Linux, NOT a FIP rebuild: the alp BL2 raw-loads the CM33 firmware from
# a fixed flash offset into a fixed SRAM0 window. Same PCB / BL2 / mtd
# layout as E1M-V2M101, so the identical args are wired there too.
#
# NOTE: the runner class is shipped IN-TREE by alp-sdk at
# `scripts/west_commands/runners/rzv2n_mtd_flash.py` and surfaced via
# `zephyr/module.yml`'s `runners:` list (ADR-0017) -- it is NOT in
# upstream Zephyr's `runners` package.
#
# No debugger is wired here: the RZ/V2N CM33 SWD attach is UNPROVEN on
# this SoC (do not add `jlink`).
#
# `--host`/`ALP_V2N_SSH_HOST` is deliberately NOT set here -- it is
# host/bench-specific and has no default (see the runner's module
# docstring); pass it at `west flash` time.
board_runner_args(rzv2n_mtd_flash
  "--mtd=/dev/mtd1"
  "--offset=0x1a0000"
  "--pad=0x3000"
  "--max-size=0x30000"
)

board_set_flasher_ifnset(rzv2n_mtd_flash)

board_finalize_runner_args(rzv2n_mtd_flash)
