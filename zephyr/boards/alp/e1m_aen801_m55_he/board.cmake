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
#   The runner class is shipped IN-TREE by alp-sdk at
#   `scripts/west_commands/runners/alif_flash.py` (Apache-2.0) and is
#   surfaced to `west flash` via `zephyr/module.yml`'s `runners:` list,
#   so it resolves WITHOUT any edit to the pinned upstream Zephyr tree
#   (it is NOT in upstream Zephyr's `runners` package).  It encodes the
#   proven bench recipe (== scripts/bench/aen/flash-run.sh): stage a
#   per-app signed-ATOC config (loadAddress 0x58000000, M55-HE),
#   app-gen-toc, then app-write-mram -p over the SE-UART.
#   `board_runner_args(alif_flash ...)` below only records args at
#   CMake-configure time; the runner is created lazily at `west flash`.
#   Setup: `pip install fdt` (an app-gen-toc dependency, not a Zephyr
#   requirement) and export SETOOLS_DIR (license-gated; not shipped) +
#   SE_UART before invoking, or pass --setools-dir/--se-uart.  See
#   scripts/bench/aen/README.md and docs/_aen-runbook-section.md.  The
#   hand-run SETOOLS recipe is documented in docs/aen-provisioning.md.

# alif_flash: --device first-5-chars must match the SETOOLS
# global-cfg.db Part# ("AE822...").  The runner compares only [:5].
board_runner_args(alif_flash "--device=AE822FA0E5597LS0_HE")

# jlink: generic Cortex-M55 -- the Alif part profile will not connect.
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
