# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Flash / debug runners for the E1M-AEN401 (Alif Ensemble E4) M55-HP
# carrier board.
#
# alif_flash: --device first-5-chars must match the SETOOLS
# global-cfg.db Part# ("AE402...").  The runner compares only [:5].
# TODO(aen401-bench): confirm SETOOLS device string for AE402FA0E5597LE0_HP.

board_runner_args(alif_flash "--device=AE402FA0E5597LE0_HP")

# jlink: generic Cortex-M55 -- the Alif part profile will not connect.
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
