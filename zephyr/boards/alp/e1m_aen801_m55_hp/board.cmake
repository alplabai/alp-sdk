# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Flash / debug runners for the E1M-AEN801 (Alif Ensemble E8) M55-HP
# carrier board.  See the M55-HE board.cmake for the full rationale:
# alif_flash (SETOOLS, the only agent that writes MRAM) is the flasher;
# generic Cortex-M55 J-Link is the debug / RAM-run path.

# alif_flash: --device first-5-chars must match the SETOOLS
# global-cfg.db Part# ("AE822...").  The runner compares only [:5].
board_runner_args(alif_flash "--device=AE822FA0E5597LS0_HP")

# jlink: generic Cortex-M55 -- the Alif part profile will not connect.
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
