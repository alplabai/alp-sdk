# Copyright (c) 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
#
# Flash / debug runners for the E1M-AEN801 (Alif Ensemble E8) M55-HE
# carrier board.
#
# Two runners are wired, matching the two physical access paths proven
# on the E8 bench (see docs/aen-bench-bringup.md):
#
#   alif_flash  (DEFAULT flasher) -- Alif Secure Toolkit (SETOOLS).
#       The Secure Enclave (SES) is the ONLY agent that writes MRAM on
#       this part: J-Link cannot program MRAM.  `west flash` therefore
#       drives SETOOLS' app-gen-toc + app-write-mram over the SE-UART
#       to author and burn the application/MCUboot ATOC.  This is the
#       SES->MCUboot->slot0 provisioning path.
#
#   jlink  (debug / attach) -- generic Cortex-M55 over SWD.
#       Used for ITCM RAM-run bring-up and live RAM_CONSOLE reads.  The
#       Alif part-number J-Link device profile FAILS to connect on a
#       fresh E8 -- the GENERIC `Cortex-M55` device must be used (the
#       SES gates the part-specific AP).  See the runbook.
#
# NOTE on the alif_flash runner itself:
#   The runner class (`scripts/west_commands/runners/alif_flash.py`,
#   Apache-2.0, from alifsemi/zephyr_alif) is NOT part of upstream
#   Zephyr's `runners` package.  `board_runner_args(alif_flash ...)`
#   below is benign at CMake-configure time (it only records args);
#   the runner is resolved by west lazily at `west flash` time.  To
#   make `west flash -r alif_flash` work, the runner must be importable
#   from the active Zephyr's `runners` package -- i.e. build against the
#   Alif Zephyr fork, or drop alif_flash.py into the upstream Zephyr's
#   scripts/west_commands/runners/.  Set ALIF_SE_TOOLS_DIR to the
#   SETOOLS install dir before invoking.  The hand-run SETOOLS recipe
#   (app-gen-toc -f <atoc.json>; app-write-mram -p) does not need the
#   runner and is documented in docs/bring-up-aen.md.

# alif_flash: --device first-5-chars must match the SETOOLS
# global-cfg.db Part# ("AE822...").  The runner compares only [:5].
board_runner_args(alif_flash "--device=AE822FA0E5597LS0_HE")

# jlink: generic Cortex-M55 -- the Alif part profile will not connect.
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
