# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Flash / debug runners for the E1M-AEN601 (Alif Ensemble E6) M55-HP
# carrier board.
#
# alif_flash: --device first-5-chars must match the SETOOLS
# global-cfg.db Part# ("AE612").  The runner compares only [:5].
# TODO(aen601-bench): confirm SETOOLS device string for AE612FA0E5597LS0_HP.

board_runner_args(alif_flash "--device=AE612FA0E5597LS0_HP")

# jlink: generic Cortex-M55 -- the debug/attach default (the Alif
# part-number device profile also connects fine on a J-Link DLL
# V9.46+ -- only old DLLs failed -- but it's reserved for the Flow D
# MRAM loader; the generic profile has no MRAM loader).
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
