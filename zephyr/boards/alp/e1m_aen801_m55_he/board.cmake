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
#       `west flash` drives SETOOLS' app-gen-toc + app-write-mram over
#       the SE-UART to author and burn the application/MCUboot ATOC.
#       This is the SES->MCUboot->slot0 provisioning path and remains
#       the default: it is the authoritative MRAM commit, proven to
#       persist across a cold power-cycle.  J-Link CAN also program and
#       persist MRAM (the two-blob Flow D loader, part-number device
#       profile below, J-Link DLL V9.46+) -- this is not the only path,
#       just not the default `west flash` wires.
#
#   jlink  (debug / attach) -- generic Cortex-M55 over SWD.
#       Used for ITCM RAM-run bring-up and live RAM_CONSOLE reads.  This
#       is the right DEBUG default: the Alif part-number device profile
#       also connects fine on a J-Link DLL V9.46+ (an older DLL can fail
#       to connect with it), but it is reserved for the Flow D MRAM
#       loader below -- the generic profile has no MRAM loader, so it
#       stays the debug/attach device.  See the runbook.
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

# jlink: generic Cortex-M55 -- the debug/attach default (Flow D's MRAM
# loader needs the part-number device instead; see the header comment).
board_runner_args(jlink "--device=Cortex-M55" "--speed=4000")

board_set_flasher_ifnset(alif_flash)
board_set_debugger_ifnset(jlink)

board_finalize_runner_args(alif_flash)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
