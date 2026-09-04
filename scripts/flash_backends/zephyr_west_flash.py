# SPDX-License-Identifier: Apache-2.0
"""
zephyr_west_flash -- invoke ``west flash`` on a Zephyr slice's build
directory.

Backend invoked by ``west alp-flash`` for every slice whose
``flash_method`` is ``zephyr_west_flash``.  The orchestrator does not
force a runner: not every in-tree board registers one (e.g. AEN's
board.cmake sets ``flash-runner: alif_flash``), so we defer to
whatever the board.cmake default resolves to unless the caller
explicitly asks for one.

flash_args contract:
  runner     str?   Zephyr runner ID -- "openocd" | "jlink" | "pyocd" |
                    "nrfjprog" | "stm32cubeprogrammer" | ...
                    OPTIONAL.  Passed through to ``--runner`` when
                    set; when absent, ``west flash`` uses the
                    board.cmake default runner.
  build_dir  str?   Path to the Zephyr build directory.  When absent,
                    the dispatcher passes the slice's ``build_dir``
                    from the manifest.
  erase      bool?  When True, append ``--erase``.
  hex_file   str?   Override the binary west picks (rare; useful when
                    a slice produces both zephyr.elf + a signed
                    zephyr.signed.hex).

  `jlink_flash_device` (the AEN SoC-variant's part-number J-Link MRAM-loader
  profile, when the SoM preset publishes one) is a CONSUMER-SIDE-ONLY key:
  this backend doesn't read it and takes no `--runner` from it, so a
  west-flash run through this in-tree backend still defers to the
  board.cmake default runner (AEN's is `flash-runner: alif_flash`, i.e. the
  SETOOLS path) even when it's present in flash_args -- that's expected,
  not a sign the key is broken.

  `expect_dpidr` + `jlink_device` (#1355) are CONSUMER-SIDE-ONLY in the same
  way, and are one fact in two keys: the SW-DP IDR the board's debug port
  must answer, and the live-core attach profile that read is performed with.
  A consumer runs them as a read-only preflight BEFORE any write, so it can
  abort on the wrong board while the session is still harmless -- necessary
  because a J-Link is selected only by serial, and a cloned serial across
  two adapters silently resolves to whichever one is attached. They arrive
  together or not at all; a half-armed pair is a refusal downstream, not a
  degraded check. This backend goes through `west`, never touches a J-Link
  directly, and so reads neither.

  `slot0_load_address` (tan-cli#353; the AEN MRAM slot0-XIP address this
  slice's application blob is linked at) is CONSUMER-SIDE-ONLY in the same
  way: a downstream flasher that writes MRAM directly over J-Link needs it
  to know where to `loadbin`; `west flash` derives its own load address
  from the build's own linked ELF, so this backend ignores the key too.

  NOTE: `jlink_device` is backend-local, not a single global key -- here it
  names the read-only SEGGER *attach profile* used only for the
  `expect_dpidr` preflight read (e.g. `Cortex-M55`, explicitly not
  flash-capable). The `swd_probe` backend (`scripts/flash_backends/
  swd_probe.py`) emits a DIFFERENT `flash_args.jlink_device` for a DIFFERENT
  helper entry: the SEGGER device it passes to `JLinkExe -device` to
  actually PROGRAM a part. Same key name, two backends, two meanings --
  there's no collision today because no single manifest entry is read by
  both backends, but don't assume one backend's `jlink_device` describes
  what the other would do with it.

Tool requirement: ``west`` on PATH.  We rely on west's own runner
plumbing (``west flash --runner X``) so each underlying runner's
prerequisite (openocd / J-Link / pyocd / nrfjprog) is implicitly
required at runtime; we don't shutil.which() those because west's
own error path is more informative.
"""

from __future__ import annotations

import shutil
import subprocess
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


class ZephyrWestFlash:
    """`west flash --build-dir <dir> [--runner <runner>]` wrapper (runner
    optional — omitted defers to the board.cmake default)."""

    name: str = "zephyr_west_flash"
    requires: list[str] = ["west"]

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()
        west = shutil.which("west")
        if west is None:
            return FlashResult(
                ok=False,
                elapsed_s=time.monotonic() - start,
                message=("zephyr_west_flash: `west` is not on PATH; "
                         "activate the Zephyr venv (`. ../.venv/"
                         "bin/activate`) or `pip install west`."),
            )

        runner = (ctx.flash_args or {}).get("runner")

        # Resolve build_dir from flash_args, falling back to the
        # artefact's parent dir (every slice's output_artefact lives
        # under <build_dir>/zephyr/zephyr.elf).
        build_dir = (ctx.flash_args or {}).get("build_dir")
        if not build_dir:
            artefact = Path(ctx.artefact_path)
            # Walk up two levels: zephyr/zephyr.elf -> zephyr/ -> build_dir.
            if artefact.name in ("zephyr.elf", "zephyr.bin",
                                 "zephyr.hex", "zephyr.uf2"):
                build_dir = str(artefact.parent.parent)
            else:
                build_dir = str(artefact.parent)

        cmd: list[str] = [
            west, "flash",
            "--build-dir", str(build_dir),
        ]
        if runner:
            cmd.extend(["--runner", str(runner)])
        if (ctx.flash_args or {}).get("erase"):
            cmd.append("--erase")
        hex_file = (ctx.flash_args or {}).get("hex_file")
        if hex_file:
            cmd.extend(["--hex-file", str(hex_file)])

        if ctx.dry_run:
            return FlashResult(
                ok=True,
                elapsed_s=time.monotonic() - start,
                message=f"zephyr_west_flash[{ctx.core_id}]: would run "
                        f"{' '.join(cmd)} (dry-run)",
                command=list(cmd),
            )

        proc = subprocess.run(cmd, check=False,
                              capture_output=True, text=True)
        elapsed = time.monotonic() - start
        if proc.returncode == 0:
            return FlashResult(
                ok=True,
                elapsed_s=elapsed,
                message=f"zephyr_west_flash[{ctx.core_id}]: programmed via "
                        f"{runner or 'board-default runner'} in "
                        f"{elapsed:.1f}s",
                command=list(cmd),
            )
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False,
            elapsed_s=elapsed,
            message=(f"zephyr_west_flash[{ctx.core_id}]: exited "
                     f"rc={proc.returncode} -- {tail_msg}"),
            command=list(cmd),
        )


_INST = ZephyrWestFlash()
register(_INST)

BACKEND: FlashBackend = _INST
