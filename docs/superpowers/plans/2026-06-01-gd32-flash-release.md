# GD32 Bridge — Lean Flashable Release + Flashing SOP — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Integrate the unmerged GD32 transport firmware as the release foundation, build a v0.1.0-candidate flashable image, make the SWD flash tooling actually work via J-Link, and publish a flashing SOP in `alp-sdk`.

**Architecture:** Merge `feat/gd32-transport-bringup` into the working branch `feat/gd32-flash-release` (already created off `dev`, carries the approved spec). Rebuild the `gd32` backend to fold in the real SPI1+I2C0 transports → `gd32-bridge.hex/.bin` (full-flash @ `0x08000000`, OTA inert). Extend the `swd_v2n_host` flash backend with a SEGGER J-Link path (primary; OpenOCD/pyOCD demoted to alternative). Write `docs/gd32-flashing.md` and cross-link it. The physical flash + silicon verification is a bench step the SOP prepares but does not perform here.

**Tech Stack:** arm-none-eabi-gcc 13.3 + CMake (firmware), Python 3.11 + pytest (flash backends/tests), SEGGER J-Link (`JLinkExe -device GD32G553xE`), OpenOCD/pyOCD (alternative), Markdown (docs).

**Reference spec:** `docs/superpowers/specs/2026-06-01-gd32-flash-release-design.md`

---

## File structure

| File | Responsibility | Task |
| --- | --- | --- |
| `firmware/gd32-bridge/**` (from branch) | Real transport HAL + build (`transport_hw_gd32.c`, `bridge_board_config.h`, `transport.h`); integrated, **no logic edits** | 1 |
| `scripts/flash_backends/swd_v2n_host.py` | Add SEGGER J-Link primary path; keep OpenOCD/pyOCD as fallback | 2 |
| `tests/scripts/test_flash_backends.py` | New J-Link tests; update the interface/target-guard test | 2 |
| `scripts/openocd/cmsis-dap.cfg`, `jlink.cfg`, `gd32g553.cfg`, `README.md` | OpenOCD alternative-path configs (bench-unverified, honestly flagged) | 3 |
| `docs/gd32-flashing.md` | The public build→wire→flash→verify SOP | 4 |
| `docs/gd32-bridge.md`, `firmware/gd32-bridge/README.md`, `docs/firmware-quickstart.md` | Cross-link the SOP | 4 |

**Branch:** all work lands on `feat/gd32-flash-release`; Task 5 merges it into `dev`.

**Pre-flight (run once before Task 1):**
```bash
cd "E:/GitHub/alp-sdk"
git rev-parse --abbrev-ref HEAD     # expect: feat/gd32-flash-release
git status --short                  # expect: clean
git submodule status vendors/gd32_firmware_library/upstream   # expect: a commit + (v1.5.0)
```
Expected: on `feat/gd32-flash-release`, clean tree, submodule populated.

---

## Task 1: Integrate transport-bringup + build-verify the v0.1.0 candidate image

**Files:**
- Modify (via merge): `firmware/gd32-bridge/**`
- No source edits in this task — it is a merge + two builds.

> **Note — stale foreign build dir.** The pre-existing
> `firmware/gd32-bridge/build/` (dated May 29) was produced on a *different*
> computer and only rode along in this synced tree; its `CMakeCache.txt`
> holds that machine's absolute paths. **Do not reuse it.** This task
> configures fresh `build/stub` + `build/gd32` dirs, so the `gd32` build is
> genuinely verified *on this machine* for the first time here. Optionally
> clear the stale dir first: `rm -rf firmware/gd32-bridge/build` (it is
> gitignored; harmless either way since we use new dir names).

- [ ] **Step 1: Confirm branch + print the merge conflict surface**

Run:
```bash
cd "E:/GitHub/alp-sdk"
git fetch origin
git merge-base dev origin/feat/gd32-transport-bringup
git diff --stat 0977435..dev -- firmware/gd32-bridge/ docs/gd32-bridge.md
```
Expected: the diff is empty or only a brand-casing/comment touch. If a real overlap on `firmware/gd32-bridge/` shows up, note it — Step 2's merge will need manual resolution.

- [ ] **Step 2: Merge the transport-bringup branch**

Run:
```bash
git merge --no-ff origin/feat/gd32-transport-bringup \
  -m "merge(gd32-bridge): SPI1+I2C0 slave transport HAL (feat/gd32-transport-bringup)"
```
Expected: `Merge made by the 'ort' strategy`, files under `firmware/gd32-bridge/` added/changed (incl. `hal/transport_hw_gd32.c`, `hal/bridge_board_config.h`, `src/transport.h`).
If conflicts: they should be confined to comments/README casing — resolve by keeping `dev`'s casing + the branch's code, `git add` the resolved files, `git commit --no-edit`.

- [ ] **Step 3: Verify the transport HAL is now present**

Run:
```bash
ls firmware/gd32-bridge/hal/transport_hw_gd32.c firmware/gd32-bridge/hal/bridge_board_config.h firmware/gd32-bridge/src/transport.h
grep -n "transport_hw_gd32.c" firmware/gd32-bridge/CMakeLists.txt
```
Expected: all three files exist; `CMakeLists.txt` lists `hal/transport_hw_gd32.c` under the `gd32` backend.

- [ ] **Step 4: Build the stub backend (protocol/dispatch compile check) in a fresh dir**

Run:
```bash
cmake -B firmware/gd32-bridge/build/stub -S firmware/gd32-bridge \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/gd32-bridge/toolchain/arm-none-eabi.cmake" \
  -DBRIDGE_HAL_BACKEND=stub
cmake --build firmware/gd32-bridge/build/stub --parallel
```
Expected: configures and builds with no errors (warnings from gcc 13.3 `-Wshadow`/`-Wpedantic` are acceptable; there is no `-Werror`). Produces `build/stub/gd32-bridge` (ELF; no `.hex`/`.bin` by design for stub).

- [ ] **Step 5: Build the gd32 backend (the real release image) in a fresh dir**

Run:
```bash
cmake -B firmware/gd32-bridge/build/gd32 -S firmware/gd32-bridge \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/gd32-bridge/toolchain/arm-none-eabi.cmake" \
  -DBRIDGE_HAL_BACKEND=gd32
cmake --build firmware/gd32-bridge/build/gd32 --parallel
```
Expected: configures (prints `gd32-bridge firmware version: 0.1.0`), links the GigaDevice library + `transport_hw_gd32.c`, and emits `build/gd32/gd32-bridge.elf` + `.hex` + `.bin`. **This image is larger than the May-29 scaffold (~10 KB text)** because the real transports + vendor SPI/I2C/GPIO/DMA drivers now link in.
If the build fails (not just warns): STOP and triage — this is the gcc-13.3-vs-CI-10.3 risk (spec A1). Fix the compile error on the branch (it is a real defect) before continuing; do not work around it in tooling.

- [ ] **Step 6: Record the release artifact's size + checksum**

Run:
```bash
arm-none-eabi-size firmware/gd32-bridge/build/gd32/gd32-bridge
sha256sum firmware/gd32-bridge/build/gd32/gd32-bridge.hex firmware/gd32-bridge/build/gd32/gd32-bridge.bin
```
Expected: a size line (text+data must fit 512 KB flash; bss must fit 96 KB SRAM) and two SHA-256 hashes. **Copy these four values** (text/data/bss + the two hashes) into a scratch note — Task 4 records them in the SOP as the v0.1.0-candidate fingerprint.

- [ ] **Step 7: No code commit needed (merge already committed; `build/` is gitignored)**

Run:
```bash
git check-ignore firmware/gd32-bridge/build && echo "build/ ignored (nothing to commit)"
git log --oneline -1
```
Expected: `build/ ignored`; HEAD is the merge commit. Task 1 produces no further commit.

---

## Task 2: Add a SEGGER J-Link path to the swd_v2n_host flash backend (TDD)

**Files:**
- Modify: `scripts/flash_backends/swd_v2n_host.py`
- Test: `tests/scripts/test_flash_backends.py`

- [ ] **Step 1: Write the failing tests**

Add these to `tests/scripts/test_flash_backends.py`. First extend the existing import (around line 42-44):
```python
from flash_backends.swd_v2n_host import (          # noqa: E402
    SwdV2nHostFlash,
    _jlink_commander_script,
)
```
Then add new tests in the `SwdV2nHostFlash` section (after `test_swd_v2n_host_falls_back_to_pyocd`):
```python
def test_swd_v2n_host_prefers_jlink_when_present() -> None:
    backend = SwdV2nHostFlash()
    artefact = "/tmp/gd32-bridge.hex"
    # Both J-Link and openocd "present" -> J-Link must win.
    with patch("flash_backends.swd_v2n_host.shutil.which",
               side_effect=lambda t: f"/usr/bin/{t}"
               if t in ("JLinkExe", "openocd") else None), \
         patch("flash_backends.swd_v2n_host.subprocess.run") as run_mock:
        result = backend.flash(_ctx(
            {"base": "0x08000000"}, dry_run=True, artefact=artefact))
    assert result.ok is True
    assert run_mock.call_count == 0
    joined = " ".join(result.command)
    assert result.command[0].endswith("JLinkExe")
    assert "-device" in result.command and "GD32G553xE" in result.command
    assert "SWD" in joined
    assert "loadfile" in result.message
    assert "gd32-bridge.hex" in result.message


def test_swd_v2n_host_jlink_happy_path() -> None:
    backend = SwdV2nHostFlash()
    with patch("flash_backends.swd_v2n_host.shutil.which",
               side_effect=lambda t: "/usr/bin/JLinkExe"
               if t == "JLinkExe" else None), \
         patch("flash_backends.swd_v2n_host.subprocess.run",
               return_value=_proc(rc=0)) as run_mock:
        result = backend.flash(_ctx(
            {"jlink_device": "GD32G553xE"}, artefact="/tmp/gd32-bridge.hex"))
    assert result.ok is True
    assert run_mock.call_count == 1
    assert run_mock.call_args[0][0][0].endswith("JLinkExe")


def test_jlink_commander_script_hex_uses_loadfile() -> None:
    script = _jlink_commander_script(Path("/tmp/gd32-bridge.hex"),
                                     "0x08000000", do_reset=True)
    assert "loadfile" in script
    assert "gd32-bridge.hex" in script
    assert "g" in script.split()        # "go" after reset
    assert script.strip().endswith("qc")


def test_jlink_commander_script_bin_uses_loadbin_with_base() -> None:
    script = _jlink_commander_script(Path("/tmp/gd32-bridge.bin"),
                                     "0x08000000", do_reset=True)
    assert "loadbin" in script
    assert "0x08000000" in script
```
Also **replace** the existing `test_swd_v2n_host_requires_interface_and_target` so it forces the OpenOCD path (J-Link absent), keeping the guard meaningful:
```python
def test_swd_v2n_host_requires_interface_and_target() -> None:
    backend = SwdV2nHostFlash()
    # No J-Link present -> openocd path, which DOES require interface+target.
    with patch("flash_backends.swd_v2n_host.shutil.which",
               side_effect=lambda t: "/usr/bin/openocd"
               if t == "openocd" else None), \
         patch("flash_backends.swd_v2n_host.subprocess.run") as run_mock:
        result = backend.flash(_ctx({}))    # no interface / target
    assert result.ok is False
    assert run_mock.call_count == 0
```

- [ ] **Step 2: Run the new tests to verify they fail**

Run:
```bash
cd "E:/GitHub/alp-sdk"
python -m pytest tests/scripts/test_flash_backends.py -k "jlink or commander_script" -v
```
Expected: FAIL — `ImportError: cannot import name '_jlink_commander_script'` (and/or the J-Link assertions fail because the backend has no J-Link path yet).

- [ ] **Step 3: Implement the J-Link path in the backend**

Replace the entire contents of `scripts/flash_backends/swd_v2n_host.py` with:
```python
# SPDX-License-Identifier: Apache-2.0
"""
swd_v2n_host -- flash the GD32G553 supervisor MCU over SWD.

Primary path is **SEGGER J-Link** (`JLinkExe -device GD32G553xE`), which
has built-in GD32G553 flash support. OpenOCD / pyOCD remain as fallbacks
for CMSIS-DAP / ST-Link users (mainline OpenOCD's GD32G5 flash support is
version-dependent -- see docs/gd32-flashing.md + scripts/openocd/).

On E1M-V2N101 the external SWD header exposes the GD32's SWDIO=PA13,
SWCLK=PA14, NRST (see metadata/chips/gd32g553.yaml).

flash_args contract:
  interface    str   OpenOCD interface ID ("cmsis-dap" | "jlink" | "stlink").
                     REQUIRED for the openocd/pyocd path; ignored by J-Link.
  target       str   OpenOCD target ID ("gd32g553"). REQUIRED for the
                     openocd/pyocd path; ignored by J-Link.
  base         str?  Flash base address (default "0x08000000").
  reset        bool  Reset+run after programming (default True).
  use_pyocd    bool  Force the pyocd path (skip J-Link + openocd).
  use_openocd  bool  Force the openocd path (skip J-Link auto-pick).
  jlink_device str?  SEGGER device name (default "GD32G553xE").
  jlink_speed  int?  SWD speed in kHz (default 4000).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from . import FlashBackend, FlashContext, FlashResult, register


_DEFAULT_BASE = "0x08000000"
_DEFAULT_JLINK_DEVICE = "GD32G553xE"
_DEFAULT_JLINK_SPEED = 4000
_JLINK_BINARIES = ("JLinkExe", "JLink")     # Linux/macOS, then Windows


def _jlink_commander_script(artefact: Path, base: str, do_reset: bool) -> str:
    """Build the J-Link Commander script that programs ``artefact``.

    ``.bin`` images need an explicit load address; ``.hex`` / ``.elf``
    carry their own addresses so ``loadfile`` suffices.
    """
    path = str(artefact)
    lines = ["r", "halt"]
    if artefact.suffix.lower() == ".bin":
        lines.append(f"loadbin {path}, {base}")
    else:
        lines.append(f"loadfile {path}")
    if do_reset:
        lines += ["r", "g"]
    lines.append("qc")
    return "\n".join(lines) + "\n"


def _which_jlink() -> "str | None":
    for name in _JLINK_BINARIES:
        found = shutil.which(name)
        if found:
            return found
    return None


class SwdV2nHostFlash:
    """SEGGER J-Link (primary) / OpenOCD / pyOCD wrapper for the GD32G553."""

    name: str = "swd_v2n_host"
    requires: list[str] = list(_JLINK_BINARIES) + ["openocd", "pyocd"]

    def flash(self, ctx: FlashContext) -> FlashResult:
        start = time.monotonic()
        fa = ctx.flash_args or {}
        base = fa.get("base") or _DEFAULT_BASE
        do_reset = bool(fa.get("reset", True))
        force_pyocd = bool(fa.get("use_pyocd"))
        force_openocd = bool(fa.get("use_openocd"))

        # ---- J-Link (primary; best GD32G5 flash support) ----
        jlink = None if (force_pyocd or force_openocd) else _which_jlink()
        if jlink:
            device = fa.get("jlink_device") or _DEFAULT_JLINK_DEVICE
            speed = str(fa.get("jlink_speed") or _DEFAULT_JLINK_SPEED)
            script = _jlink_commander_script(ctx.artefact_path, base, do_reset)
            base_cmd = [
                jlink, "-device", device, "-if", "SWD", "-speed", speed,
                "-AutoConnect", "1", "-ExitOnError", "1", "-NoGui", "1",
                "-CommanderScript",
            ]
            if ctx.dry_run:
                cmd = base_cmd + ["<generated.jlink>"]
                return FlashResult(
                    ok=True,
                    elapsed_s=time.monotonic() - start,
                    message=(f"swd_v2n_host[{ctx.core_id}]: would run "
                             f"{' '.join(cmd)} (dry-run); J-Link script:\n"
                             f"{script}"),
                    command=cmd,
                )
            fd, script_path = tempfile.mkstemp(suffix=".jlink")
            try:
                with os.fdopen(fd, "w") as fh:
                    fh.write(script)
                cmd = base_cmd + [script_path]
                proc = subprocess.run(cmd, check=False,
                                      capture_output=True, text=True)
            finally:
                try:
                    os.unlink(script_path)
                except OSError:                      # pragma: no cover
                    pass
            elapsed = time.monotonic() - start
            if proc.returncode == 0:
                return FlashResult(
                    ok=True, elapsed_s=elapsed,
                    message=(f"swd_v2n_host[{ctx.core_id}]: GD32G553 flashed "
                             f"via J-Link ({device}) @ {base} in "
                             f"{elapsed:.1f}s"),
                    command=cmd)
            tail = (proc.stderr or proc.stdout or "").strip().splitlines()
            tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
            return FlashResult(
                ok=False, elapsed_s=elapsed,
                message=(f"swd_v2n_host[{ctx.core_id}]: J-Link exited "
                         f"rc={proc.returncode} -- {tail_msg}"),
                command=cmd)

        # ---- OpenOCD / pyOCD (alternative; need interface + target) ----
        interface = fa.get("interface") or ""
        target = fa.get("target") or ""
        if not interface or not target:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("swd_v2n_host: flash_args.interface and "
                         "flash_args.target are required for the openocd/pyocd "
                         "path (e.g. interface=cmsis-dap, target=gd32g553) -- "
                         "or install SEGGER J-Link for the primary path."))

        openocd = None if force_pyocd else shutil.which("openocd")
        pyocd = shutil.which("pyocd")

        if openocd:
            program_cmd = f"program {ctx.artefact_path} verify"
            if do_reset:
                program_cmd += " reset"
            program_cmd += f" exit {base}"
            cmd = [openocd, "-f", f"interface/{interface}.cfg",
                   "-f", f"target/{target}.cfg", "-c", program_cmd]
        elif pyocd:
            cmd = [pyocd, "flash", "--target", str(target),
                   "--base-address", str(base), str(ctx.artefact_path)]
        else:
            return FlashResult(
                ok=False, elapsed_s=time.monotonic() - start,
                message=("swd_v2n_host: no flash tool found -- install SEGGER "
                         "J-Link (preferred), or `openocd`, or `pyocd`."))

        if ctx.dry_run:
            return FlashResult(
                ok=True, elapsed_s=time.monotonic() - start,
                message=f"swd_v2n_host[{ctx.core_id}]: would run "
                        f"{' '.join(cmd)} (dry-run)",
                command=cmd)

        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
        elapsed = time.monotonic() - start
        if proc.returncode == 0:
            return FlashResult(
                ok=True, elapsed_s=elapsed,
                message=f"swd_v2n_host[{ctx.core_id}]: GD32G553 flashed "
                        f"@ {base} in {elapsed:.1f}s",
                command=cmd)
        tail = (proc.stderr or proc.stdout or "").strip().splitlines()
        tail_msg = " | ".join(tail[-4:]) if tail else "(no output)"
        return FlashResult(
            ok=False, elapsed_s=elapsed,
            message=(f"swd_v2n_host[{ctx.core_id}]: exited "
                     f"rc={proc.returncode} -- {tail_msg}"),
            command=cmd)


_INST = SwdV2nHostFlash()
register(_INST)

BACKEND: FlashBackend = _INST
```

- [ ] **Step 4: Run the J-Link tests to verify they pass**

Run:
```bash
python -m pytest tests/scripts/test_flash_backends.py -k "jlink or commander_script" -v
```
Expected: PASS (4 new tests).

- [ ] **Step 5: Run the full flash-backends suite to confirm no regressions**

Run:
```bash
python -m pytest tests/scripts/test_flash_backends.py -v
```
Expected: ALL pass, including the updated `test_swd_v2n_host_requires_interface_and_target` and the unchanged openocd/pyocd tests.

- [ ] **Step 6: Commit**

```bash
git add scripts/flash_backends/swd_v2n_host.py tests/scripts/test_flash_backends.py
git commit -m "feat(flash): add SEGGER J-Link primary path to swd_v2n_host (GD32G553)" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Add scripts/openocd/ alternative-probe configs

**Files:**
- Create: `scripts/openocd/cmsis-dap.cfg`, `scripts/openocd/jlink.cfg`, `scripts/openocd/gd32g553.cfg`, `scripts/openocd/README.md`

> These exist so the OpenOCD/pyOCD fallback the backend names is documented. They are **bench-unverified for GD32G5 flashing**; J-Link is the supported route. `openocd`/`pyocd` are not installed on this workstation, so correctness is confirmed at the bench.

- [ ] **Step 1: Create the CMSIS-DAP interface config**

Create `scripts/openocd/cmsis-dap.cfg`:
```tcl
# SPDX-License-Identifier: Apache-2.0
# CMSIS-DAP SWD adapter for the GD32G553 supervisor (alternative path).
# The SUPPORTED primary path is SEGGER J-Link -- see docs/gd32-flashing.md.
adapter driver cmsis-dap
transport select swd
adapter speed 2000
```

- [ ] **Step 2: Create the J-Link-via-OpenOCD interface config**

Create `scripts/openocd/jlink.cfg`:
```tcl
# SPDX-License-Identifier: Apache-2.0
# Drive a SEGGER J-Link THROUGH OpenOCD. NOTE: the supported J-Link path
# uses SEGGER's own JLinkExe (scripts/flash_backends/swd_v2n_host.py);
# this file is only for users who specifically want OpenOCD in the loop.
adapter driver jlink
transport select swd
adapter speed 2000
```

- [ ] **Step 3: Create the GD32G553 target config (core bring-up; flashing caveated)**

Create `scripts/openocd/gd32g553.cfg`:
```tcl
# SPDX-License-Identifier: Apache-2.0
#
# GD32G553MEY7TR (Cortex-M33; 512 KB main flash @ 0x08000000, 96 KB SRAM,
# 32 KB TCMSRAM). ALTERNATIVE path only.
#
# BENCH-UNVERIFIED FOR FLASH PROGRAMMING. GD32G5 is not a stock OpenOCD
# flash target; the SUPPORTED route is SEGGER J-Link
# (`JLinkExe -device GD32G553xE`, see docs/gd32-flashing.md). This config
# brings the core up as a generic Cortex-M over SWD so connect / halt /
# IDCODE work for debug; PROGRAMMING via OpenOCD needs a GD32G5-aware
# OpenOCD build (e.g. GigaDevice's GD-Link distribution).

source [find target/swj-dp.tcl]

if { [info exists CHIPNAME] } { set _CHIPNAME $CHIPNAME } else { set _CHIPNAME gd32g553 }
# ARM Cortex-M33 SW-DP IDCODE -- expected 0x6BA02477.
if { [info exists CPUTAPID] } { set _CPUTAPID $CPUTAPID } else { set _CPUTAPID 0x6ba02477 }

swj_newdap $_CHIPNAME cpu -irlen 4 -expected-id $_CPUTAPID
dap create $_CHIPNAME.dap -chain-position $_CHIPNAME.cpu
target create $_CHIPNAME.cpu cortex_m -dap $_CHIPNAME.dap

reset_config srst_only

# Internal-flash programming is intentionally NOT declared here: no
# mainline OpenOCD flash driver is confirmed for GD32G5. To program with
# OpenOCD, add a `flash bank` line for your GD32G5-aware build and VALIDATE
# the erase/program/verify cycle on a sacrificial part first. Otherwise use
# the J-Link path, which knows this device natively.
```

- [ ] **Step 4: Create the directory README**

Create `scripts/openocd/README.md`:
```markdown
# OpenOCD configs for the GD32G553 supervisor (alternative flash path)

The **supported** way to flash the GD32 bridge is **SEGGER J-Link** via
`scripts/flash_backends/swd_v2n_host.py` (`JLinkExe -device GD32G553xE`) —
see [`docs/gd32-flashing.md`](../../docs/gd32-flashing.md).

These configs exist for CMSIS-DAP / ST-Link / OpenOCD users:

| File | Purpose |
| --- | --- |
| `cmsis-dap.cfg` | CMSIS-DAP SWD adapter |
| `jlink.cfg` | drive a J-Link *through* OpenOCD (not the supported J-Link path) |
| `gd32g553.cfg` | GD32G553 Cortex-M target; core bring-up + IDCODE only |

**Status: bench-unverified for flash programming.** GD32G5 is not a stock
OpenOCD flash target; `gd32g553.cfg` deliberately declares no `flash bank`.
For OpenOCD-based *programming* you need a GD32G5-aware OpenOCD build
(e.g. GigaDevice's GD-Link) and must validate the erase/program/verify
cycle on silicon. For routine flashing, use J-Link.
```

- [ ] **Step 5: Sanity-check the files exist and commit**

Run:
```bash
ls -1 scripts/openocd/
```
Expected: `README.md  cmsis-dap.cfg  gd32g553.cfg  jlink.cfg`.
```bash
git add scripts/openocd/
git commit -m "feat(flash): add scripts/openocd/ alternative-probe configs (GD32G553, bench-unverified)" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Write docs/gd32-flashing.md SOP + cross-links

**Files:**
- Create: `docs/gd32-flashing.md`
- Modify: `docs/gd32-bridge.md`, `firmware/gd32-bridge/README.md`, `docs/firmware-quickstart.md`

- [ ] **Step 1: Write the SOP**

Create `docs/gd32-flashing.md` with the following content. Replace the four `REPLACE_WITH_TASK1_*` tokens with the size/hash values captured in Task 1 Step 6 (no other placeholders permitted):
```markdown
# Flashing the GD32 bridge firmware

The GD32G553 supervisor on E1M-X V2N / V2N-M1 SoMs ships **pre-flashed by
Alp** — for normal use you do nothing. This SOP is for **bench bring-up,
factory first-flash, and recovery**: taking a blank or stale GD32 to a
running bridge over SWD.

> **Status (2026-06-01): v0.1.0 candidate — HIL-pending.** The transport
> firmware builds clean but has **not yet run on silicon**. Flashing it is
> a first-bring-up activity; expect to validate, not just install.

## 1. Prerequisites

- `arm-none-eabi-gcc` 13.x and `cmake` 3.20+ on PATH.
- The GigaDevice firmware-library submodule:
  `git submodule update --init --recursive vendors/gd32_firmware_library/upstream`
- A SWD probe. **Primary: SEGGER J-Link** (+ the SEGGER J-Link Software
  pack, which provides `JLinkExe`/`JLink`). Alternative: CMSIS-DAP or
  ST-Link via OpenOCD (see `scripts/openocd/`, bench-unverified for GD32G5).

## 2. Build the image

```bash
cd <alp-sdk>
cmake -B firmware/gd32-bridge/build/gd32 -S firmware/gd32-bridge \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/gd32-bridge/toolchain/arm-none-eabi.cmake" \
  -DBRIDGE_HAL_BACKEND=gd32
cmake --build firmware/gd32-bridge/build/gd32 --parallel
```

Output: `firmware/gd32-bridge/build/gd32/gd32-bridge.{elf,hex,bin}` — a
single full-flash image linked at `0x08000000` (transports on; OTA inert).

v0.1.0-candidate fingerprint (this commit):

| Field | Value |
| --- | --- |
| size (text/data/bss) | REPLACE_WITH_TASK1_SIZE |
| `gd32-bridge.hex` sha256 | REPLACE_WITH_TASK1_HEX_SHA256 |
| `gd32-bridge.bin` sha256 | REPLACE_WITH_TASK1_BIN_SHA256 |

> OTA is **not** armed (`-DBRIDGE_OTA_PARTITIONED` is OFF). Do not enable it
> for bench flashing — the partitioned bootloader is HIL-pending and a bad
> bootloader bricks the part (no host-driven SWD reflash this HW rev).

## 3. Wire the SWD probe

The external programming header exposes the GD32's debug port
(`metadata/chips/gd32g553.yaml`):

| Signal | GD32 pad | Probe |
| --- | --- | --- |
| SWDIO | `PA13` | SWDIO |
| SWCLK | `PA14` | SWCLK |
| NRST | (board NRST) | nRESET (optional but recommended) |
| GND | — | GND |
| VTref | 3V3 | VTref (sense; do not back-power) |

Power the module from its normal supply; the probe senses `VTref`, it does
not power the target.

## 4. Flash

**Preferred — through the SDK orchestrator** (auto-selects J-Link when
`JLinkExe` is on PATH):
```bash
west alp-build examples/v2n/v2n-gd32-bridge-ping
west alp-flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge --dry-run  # preview
west alp-flash examples/v2n/v2n-gd32-bridge-ping --helper gd32_bridge            # flash
```

**Standalone — SEGGER J-Link** (equivalent one-liner; the orchestrator
generates the same script):
```bash
cat > /tmp/flash.jlink <<'EOF'
r
halt
loadfile <alp-sdk>/firmware/gd32-bridge/build/gd32/gd32-bridge.hex
r
g
qc
EOF
JLinkExe -device GD32G553xE -if SWD -speed 4000 -AutoConnect 1 \
         -ExitOnError 1 -NoGui 1 -CommanderScript /tmp/flash.jlink
```

**Alternative — OpenOCD** (CMSIS-DAP/ST-Link; **requires a GD32G5-aware
OpenOCD build**, see `scripts/openocd/README.md`):
```bash
openocd -f scripts/openocd/cmsis-dap.cfg -f scripts/openocd/gd32g553.cfg \
        -c "program firmware/gd32-bridge/build/gd32/gd32-bridge.hex verify reset exit 0x08000000"
```

## 5. Verify

1. **Probe-level:** on connect, J-Link reports the SW-DP IDCODE — confirm
   it reads **`0x6BA02477`** (ARM Cortex-M33 DPv2). A mismatch means wrong
   target/wiring, not a firmware problem.
2. **Bridge-level:** run the host round-trip example and confirm
   `PING` + `GET_VERSION` succeed:
   ```bash
   west alp-build examples/v2n/v2n-gd32-bridge-ping
   west flash    # onto the Renesas host that talks to the GD32
   ```
   See `examples/v2n/v2n-gd32-bridge-ping/README.md`.
   - **Validate the SPI path first** — it is a clean point-to-point link.
   - The **I2C** management path shares **BRD_I2C**; on the V2M101 bench
     board that bus is currently wedged (see the bench handoff), so an I2C
     `PING` failure there is a board issue, not firmware.

## 6. First-silicon validation points

`hal/bridge_board_config.h` calls these out as the spots most likely to need
tuning on first run (none have been seen on real hardware yet):

- SPI: the master's CS-to-first-SCK setup vs the GD32's EXTI-trigger +
  DMA-arm latency. If the first reply byte is stale, increase the host's
  inter-transaction gap (protocol §4.1).
- I2C: the slave clock-stretch window while a reply is staged.

## 7. Recovery

A GD32 left in a bad state is recovered the same way it is first-flashed:
re-run §4 with the probe. There is **no host-driven SWD reflash** on this HW
rev, so keep a probe on the bench. (The host-driven path lives behind
`chips/gd32_swd/` for a future rev; protocol §10 Path B.)

## See also

- [`docs/gd32-bridge.md`](gd32-bridge.md) — firmware-tree overview.
- [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) — wire protocol.
- [`scripts/openocd/README.md`](../scripts/openocd/README.md) — OpenOCD alternative.
- `metadata/chips/gd32g553.yaml` — pin map + SWD header.
```

- [ ] **Step 2: Cross-link from docs/gd32-bridge.md**

In `docs/gd32-bridge.md`, find the "Flashing" section's intro line and add a pointer. Replace:
```markdown
## Flashing
```
with:
```markdown
## Flashing

> **Step-by-step bench/factory/recovery SOP:** [`docs/gd32-flashing.md`](gd32-flashing.md)
> (build → wire SWD → flash via J-Link → verify IDCODE + PING).
```

- [ ] **Step 3: Cross-link from firmware/gd32-bridge/README.md**

In `firmware/gd32-bridge/README.md`, find the "## Flashing" section (the line `Flashing\nin the development case is done with an external SWD probe...`) and insert immediately under the `## Flashing` heading line — first add the heading anchor. Replace:
```markdown
The build emits `build/gd32-bridge.elf` + `.hex` + `.bin`.  Flashing
in the development case is done with an external SWD probe on
```
with:
```markdown
The build emits `build/gd32-bridge.elf` + `.hex` + `.bin`.  Full
build → flash → verify SOP: [`../docs/gd32-flashing.md`](../docs/gd32-flashing.md).
Flashing in the development case is done with an external SWD probe on
```

- [ ] **Step 4: Cross-link from docs/firmware-quickstart.md**

In `docs/firmware-quickstart.md`, find the row in the §8 "Where to look next" table for the GD32 bridge protocol and add a flashing row immediately after it. Replace:
```markdown
| GD32 bridge wire protocol                        | [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) |
```
with:
```markdown
| GD32 bridge wire protocol                        | [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) |
| Flashing the GD32 bridge firmware                | [`docs/gd32-flashing.md`](gd32-flashing.md)               |
```

- [ ] **Step 5: Check the doc links resolve**

Run:
```bash
python - <<'PY'
import re, pathlib
root = pathlib.Path(".")
doc = root/"docs/gd32-flashing.md"
text = doc.read_text(encoding="utf-8")
bad = []
for m in re.finditer(r"\]\(([^)]+)\)", text):
    rel = m.group(1).split("#")[0]
    if rel.startswith("http") or not rel:
        continue
    if not (doc.parent/rel).resolve().exists():
        bad.append(rel)
print("BROKEN LINKS:", bad or "none")
PY
```
Expected: `BROKEN LINKS: none`.

- [ ] **Step 6: Commit**

```bash
git add docs/gd32-flashing.md docs/gd32-bridge.md firmware/gd32-bridge/README.md docs/firmware-quickstart.md
git commit -m "docs(gd32-bridge): add public flashing SOP (build -> SWD -> verify) + cross-links" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Final verification + integrate to dev

**Files:** none modified (verification + merge).

- [ ] **Step 1: Protocol vectors still consistent**

Run:
```bash
python firmware/gd32-bridge/tests/gen_protocol_vectors.py --check
```
Expected: exits 0 (generated vectors match the committed `protocol_vectors.txt`).

- [ ] **Step 2: Orchestrator dry-run picks the J-Link path**

Run:
```bash
python - <<'PY'
import sys, pathlib
sys.path.insert(0, str(pathlib.Path("scripts").resolve()))
from unittest.mock import patch
import flash_backends
from flash_backends import FlashContext
be = flash_backends.lookup("swd_v2n_host")
with patch("flash_backends.swd_v2n_host.shutil.which",
           side_effect=lambda t: "/usr/bin/JLinkExe" if t == "JLinkExe" else None):
    r = be.flash(FlashContext(
        artefact_path=pathlib.Path("firmware/gd32-bridge/build/gd32/gd32-bridge.hex"),
        flash_args={"base": "0x08000000"}, core_id="gd32_bridge",
        sku="E1M-V2N101", dry_run=True))
print("ok:", r.ok)
print("cmd:", " ".join(r.command))
assert r.ok and r.command[0].endswith("JLinkExe") and "GD32G553xE" in r.command
print("J-LINK DRY-RUN OK")
PY
```
Expected: prints the `JLinkExe -device GD32G553xE ...` command and `J-LINK DRY-RUN OK`.

- [ ] **Step 3: Full flash-backends suite green**

Run:
```bash
python -m pytest tests/scripts/test_flash_backends.py -q
```
Expected: all pass.

- [ ] **Step 4: Review the branch diff**

Run:
```bash
git log --oneline dev..HEAD
git diff --stat dev..HEAD
```
Expected: the spec commits + the merge + the J-Link/openocd/docs commits; diffstat touches `firmware/gd32-bridge/**` (from merge), `scripts/flash_backends/swd_v2n_host.py`, `tests/scripts/test_flash_backends.py`, `scripts/openocd/**`, `docs/**`.

- [ ] **Step 5: Merge to dev**

```bash
git checkout dev
git merge --no-ff feat/gd32-flash-release \
  -m "merge: GD32 lean flashable release + flashing SOP (Path A)"
git log --oneline -3
```
Expected: fast, clean merge onto `dev`. **Do not push** unless the user asks.

---

## Self-review

**Spec coverage** (`2026-06-01-gd32-flash-release-design.md`):
- §1 Branch integration → Task 1 (merge + conflict-surface check).
- §2 Build-verify + release artifact → Task 1 Steps 4-6 (both backends, size, sha256, candidate fingerprint).
- §3 Flash tooling (J-Link primary) → Task 2 (J-Link path, tested); §3 alternative (`scripts/openocd/`) → Task 3.
- §4 Public SOP + cross-links + internal mirror → Task 4 (SOP §5/§7 fold in the generic verify/recovery procedure from `alp-sdk-internal`; rig-specific bits omitted).
- §5 Verification → Task 1 Step 4-6, Task 5 Steps 1-3 (build, vectors, dry-run, suite); bench boundary documented in the SOP §5-§7.
- Non-goals respected: OTA stays off (no `-DBRIDGE_OTA_PARTITIONED`); no transport-logic edits; no committed prebuilt blob (only a fingerprint table); no `chips/gd32_swd` work.

**Placeholder scan:** the only fill-ins are the three `REPLACE_WITH_TASK1_*` fingerprint values in Task 4 Step 1, sourced from Task 1 Step 6 — flagged explicitly, not open-ended.

**Type/name consistency:** `_jlink_commander_script(artefact, base, do_reset)` and `SwdV2nHostFlash` are referenced identically in the implementation (Task 2 Step 3) and the tests (Task 2 Step 1). `flash_method` name `swd_v2n_host` and `flash_args` keys (`base`, `jlink_device`, `interface`, `target`, `use_pyocd`, `use_openocd`) match between backend, tests, and the SOP.
