# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Flash / debug runners for the E1M-AEN801 (Alif Ensemble E8) M55-HP
# carrier board.  See the M55-HE board.cmake for the full rationale:
# alif_flash (SETOOLS) is the flasher; generic Cortex-M55 J-Link is the
# debug / RAM-run path.  J-Link CAN also program and persist MRAM (the
# two-blob Flow D loader, part-number device profile, J-Link DLL
# V9.46+) -- it is not the only agent that writes MRAM, just not the
# default `west flash` wires.  Either path (Flow D or SETOOLS) still
# requires SETOOLS' app-gen-toc to sign the ATOC before it is written.

# alif_flash: --device first-5-chars must match the SETOOLS
# global-cfg.db Part# ("AE822...").  The runner compares only [:5].
board_runner_args(alif_flash "--device=AE822FA0E5597LS0_HP")

# jlink: generic Cortex-M55 -- the debug/attach default (the Alif
# part-number device profile also connects fine on a J-Link DLL
# V9.46+ -- only old DLLs failed -- but it's reserved for the Flow D
# MRAM loader; the generic profile has no MRAM loader).
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
