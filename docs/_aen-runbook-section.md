<!--
  Runbook section for docs/bring-up-aen.md — VALIDATED flashing + bring-up flows.
  All commands/values below were confirmed on real E1M-AEN801 (Alif Ensemble E8)
  silicon during the 2026-06-15 bench session. This is a section fragment intended
  to be folded into docs/bring-up-aen.md (it cross-references that file's §-numbers).
-->

## Flashing + early bring-up — the validated flows

There are **four** ways to get code onto, and observe code on, an E1M-AEN801
(Alif Ensemble E8) module, depending on what you are doing. All were
confirmed on real silicon:

| Flow | Use it for | Touches MRAM? | Tooling |
|---|---|---|---|
| **D. J-Link direct MRAM flash** | Default burn path; QA; dev iteration | **Yes** | J-Link Alif MRAM loader over SWD (part-number device profile) |
| **A. SETOOLS MRAM flash (fallback)** | Re-keying; bare-module recovery | **Yes** | SETOOLS over the SE-UART (`west flash` = `alif_flash` runner) |
| **B. Console observation** | Watching app output during bring-up | No | RAM console over SWD, or SEGGER RTT |
| **C. J-Link RAM-run** | Dev/debug iteration without burning MRAM | No | J-Link `loadbin` to ITCM + `go` |

> D is now the default burn path on the bench; A is the fallback (re-keying,
> recovery). A burn flow (D or A) decides *what runs* and **B** decides *how you
> watch it*. On this bench the only USB serial port is the FT232R wired to the
> **SE-UART** (used by flow A), so the application console is not visible over
> USB — which is exactly why flow B exists. Flow C is the fast inner loop that
> never writes flash.

---

### Flow D — J-Link direct MRAM flash (the default burn path)

J-Link **can** burn the Alif MRAM directly over SWD: its built-in Alif MRAM
loader activates when you select the **part-number device profile**
`AE822FA0E5597LS0_M55_HE` (the *generic* `Cortex-M55` profile, used by flow C,
does **not** expose the loader — which is why the old "J-Link cannot write
MRAM" claim was only ever true for the generic profile). It burns the signed
ATOC over SWD in ~0.16 s, verifies it, then a reset of type `nRESET` (RSetType
2) re-runs the SE boot ROM so the app boots from MRAM.

Requires **J-Link V9.46+ DLL** (the bench has V9.50) and a probe on matched
J-Link **V13 firmware**. Helper: `bench-builds/flash-jlink.sh`.

> **Probe firmware gotcha.** A version-mismatched probe forces a J-Link
> firmware update on first connect. That update **times out over a USB hub** —
> connect the probe to a **direct root USB port** for the firmware step.

---

### Flow A — SETOOLS MRAM flash, fallback (SETOOLS over the SE-UART)

This is the Alif-native way an image enters MRAM. There is **no host-strap
boot pin and no recovery jumper** on the E1M-AEN — SETOOLS puts the device into
maintenance mode itself (it sets the maintenance flag and resets the part),
then writes the ATOC over the SE-UART. The SES validates the ATOC and boots it
on the next reset.

> Prerequisite: the SE-UART must be wired correctly (1.8 V logic, crossed
> TX/RX, common GND) and SETOOLS configured for your part. That wiring and the
> `tools-config` step are the #1 source of "Target did not respond" — see
> [`aen-provisioning.md`](aen-provisioning.md) §2–§3 before you start. Baud is
> **57600** on E8 (the SETOOLS handshake negotiates it dynamically).

**Manual SETOOLS path (authoritative, what we validated):**

```bash
cd <setools>/app-release-exec-linux

# 1. Build the ATOC package from your app config (config under build/config/).
#    Use an app-only config so you keep the factory DEVICE config — a mismatched
#    DEVICE config is the documented crash cause (see aen-provisioning.md §4).
./app-gen-toc -f build/config/<cfg>.json

# 2. Write it to MRAM over the SE-UART. The device AUTO-enters maintenance
#    (SET_MAINTENANCE_FLAG + reset) — no jumper, no strap. Baud is dynamic.
#    <your-serial-device>: your OS's port name for the SE-UART adapter --
#    see docs/cross-platform-setup.md §7.7 for the per-OS naming convention.
./app-write-mram -c <your-serial-device> -p .
```

A clean write ends with `100% ... Done`. On the next reset the SES loads and
boots the ATOC: in our validation the stock `m55_blink` image came up at its
`loadAddress` and the HE core executed it. (`loadAddress 0x58000000` for the
M55-HE.)

**Via west (same runner under the hood):**

`west flash` on the carrier board does **not** use J-Link — it invokes the
**`alif_flash`** runner, which wraps the SETOOLS `app-gen-toc` + `app-write-mram`
sequence above (it stages the *same* signed-ATOC config, `loadAddress 0x58000000`
for the M55-HE, that the manual path uses). Use the sysbuild flow so MCUboot
signs your image into slot0:

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app> \
    --sysbuild --sysbuild-config <alp-sdk>/zephyr/sysbuild/aen/sysbuild.conf
export SETOOLS_DIR=<...>/app-release-exec-linux   # license-gated; not shipped
export SE_UART=<your-serial-device>               # the SE-UART (host-specific)
west flash      # -> alif_flash runner -> SETOOLS over the SE-UART
```

**One-off runner setup.** The `alif_flash` runner is **not** in upstream
Zephyr's `runners` package; alp-sdk ships it
([`scripts/west_commands/runners/alif_flash.py`](../scripts/west_commands/runners/alif_flash.py))
and surfaces it via `zephyr/module.yml`'s `runners:` list, so `west flash`
discovers it from the alp-sdk module with **no edit to the pinned Zephyr tree**.
Two host prerequisites:

- `pip install fdt` — `app-gen-toc` depends on the `fdt` Python package, which
  is not a Zephyr requirement; without it the toolkit fails. (The runner warns
  if `fdt` is missing.)
- Obtain the **Alif Security Toolkit** (license-gated, not redistributed by
  alp-sdk) and point the runner at it with `--setools-dir` or `$SETOOLS_DIR`,
  and at the SE-UART with `--se-uart` or `$SE_UART` (the same env vars
  `scripts/bench/aen/flash-run.sh` uses, so a host already set up for the bench
  helper needs no extra flags). A slot0-linked app that overflows ITCM (e.g. an
  NPU model; `mramAddress 0x80010000`, flags `["boot"]`) is NOT provisionable
  through this SE-UART-only runner — `--mram-xip` fails fast and routes you to
  the two-blob `scripts/bench/aen/flash-jlink-mramxip.sh` (Flow D) helper.

> **Pre-provisioned modules from Alp Lab** already carry a dev-signed MCUboot +
> self-test in slot0 (LCS=DM), so the core is already released and `west flash`
> works day-1 with no manual SETOOLS step. You only need the manual path above
> to re-key to your own production key or to recover a wiped/bare module. See
> [`aen-provisioning.md`](aen-provisioning.md) §0.5.

---

### Flow B — Seeing the console (the bench reality)

On the E8 the HE application console is a real UART, but **which** pins depend
on the carrier:

| Carrier | HE console UART | Pins |
|---|---|---|
| Alif Ensemble DevKit | **UART2** | P1_0 / P1_1 |
| E1M carrier (E1M-EVK) | **UART5** | P3_4 / P3_5 |

On a bench whose only USB serial adapter is the **FT232R SE-UART**, *neither*
of those app UARTs is wired out to USB — so early app output is invisible over
USB. Two options that need no extra UART:

**Option 1 — RAM console over SWD (what the bench uses).** Build with the RAM
console instead of the UART console; the human reads the buffer symbol over
SWD with J-Link and ASCII-decodes it. In `prj.conf`:

```ini
CONFIG_RAM_CONSOLE=y
CONFIG_RAM_CONSOLE_BUFFER_SIZE=2048
CONFIG_UART_CONSOLE=n
CONFIG_CONSOLE=y
```

Then `printk(...)` output accumulates in the `ram_console_buf` symbol. After
the app runs (flow A or flow C), read it over SWD:

```
J-Link> mem8 <addr-of ram_console_buf>, 0x800   // 2048 bytes, then ASCII-decode
```

Resolve the symbol address from the build's `zephyr.map` (or `nm zephyr.elf |
grep ram_console_buf`). Make each test emit one unambiguous line, e.g.
`RESULT PASS: ...` / `RESULT FAIL: ...`, so the readback is decisive.

**Option 2 — SEGGER RTT.** With the J-Link already attached (flow C), RTT gives
a live terminal over the same SWD link with no extra wiring
(`CONFIG_USE_SEGGER_RTT=y`, `CONFIG_RTT_CONSOLE=y`). Convenient for interactive
iteration; the RAM console is simpler for a single PASS/FAIL gate.

---

### Flow C — J-Link RAM-run (dev/debug, no MRAM write)

For the fast inner loop you can load an image straight into ITCM over SWD and
run it — nothing is written to MRAM, so you don't consume the SES/ATOC path at
all. This is how the per-driver bench tests in this repo are validated.

**1. Build with the RAM-run overlay.** The overlay retargets the ROM region to
ITCM so J-Link can `loadbin` + run. **Every** RAM-run app overlay must contain:

```dts
/ {
    chosen {
        zephyr,flash = <&itcm>;
        /delete-property/ zephyr,code-partition;
    };
};
```

Combine it with the flow-B RAM console `prj.conf` above so you can read the
result over SWD. Build with the carrier board target and the module paths:

```bash
ZEPHYR_BASE=<zephyr-base> \
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <app> -- \
    "-DEXTRA_ZEPHYR_MODULES=<alp-sdk>;<hal_alif>" \
    -DEXTRA_DTC_OVERLAY_FILE=<app.overlay>
```

**2. Attach with J-Link using the GENERIC core device.** Use the generic
`Cortex-M55` device — **not** the Alif part number. On the E8 bench the
part-specific device (`AE822FA0E5597LS0_M55_HE`) connect sequence fails
post-boot ("Could not connect to the target device"), while the generic core
device scans the APs and finds the released core directly (BENCH-VERIFIED):

```bash
JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1
```

Expected on a provisioned/running E8 (BENCH-VERIFIED 2026-06-15): SW-DP IDR
`0x4C013477`, CPUID `0x411FD220` (Cortex-M55 r1p0), `Secure debug: enabled`.
A *fresh, un-provisioned* SoM finds **no core** here ("Could not find core in
CoreSight setup") — the SES holds the M55 until an app is provisioned; that's
normal, not a fault.

**3. Load + go.** In J-Link Commander, load the raw binary to the ITCM base and
start it:

```
J-Link> loadbin build/zephyr/zephyr.bin, <ITCM-base>
J-Link> setpc <ITCM-base>
J-Link> go
```

Then read the result with flow B (mem8 of `ram_console_buf`).

> **Reset caveat.** A J-Link reset asserts **SYSRESETREQ**, which reboots the
> **SES**, not just the M55 — so a reset takes you back through the SES boot
> path (and, on a board with no ATOC, back to a gated core). For the RAM-run
> loop prefer `loadbin`/`setpc`/`go` over `reset`; if you must reset, expect to
> re-attach after the SES comes back up.

---

### Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `app-write-mram`: `Target did not respond` | SE-UART wiring or baud. Confirm **1.8 V** adapter, **crossed** TX/RX, **common GND**, port = the FT232R SE-UART, baud **57600** (E8). See [`aen-provisioning.md`](aen-provisioning.md) §2. |
| `app-write-mram` sits at `Waiting for Target..[RESET Platform]` | Hard-maintenance — **power-cycle** the board so SETOOLS catches the SES boot-ISP window. A clean write ends `100% ... Done`. |
| Image written but won't boot | ATOC built with the wrong **DEVICE** config for the part. Re-run `tools-config` for the part, or write an **app-only** ATOC keeping the factory DEVICE config. |
| `west flash` tries to use J-Link / fails to flash | On the carrier board `west flash` must use the **`alif_flash`** runner (SETOOLS, flow A); confirm the SE-UART port. (To burn over J-Link instead, use the **part-number device profile** — flow D — not the generic core profile.) |
| J-Link won't write MRAM | You selected the **generic** `Cortex-M55` device (flow C); the MRAM loader only activates with the **part-number profile** `AE822FA0E5597LS0_M55_HE`. Also needs J-Link **V9.46+** DLL and matched V13 probe firmware (flow D). |
| J-Link firmware update times out on connect | Mismatched probe firmware updates over a **USB hub** stall — connect the probe to a **direct root USB port**. |
| No app output anywhere over USB | Expected on this bench — the only USB serial is the SE-UART; the HE app console (UART2 on DevKit / UART5 on E1M) isn't wired to USB. Use the **RAM console** (flow B) or **RTT**. |
| RAM console reads as all-zeros / garbage | Wrong `ram_console_buf` address (re-resolve from `zephyr.map`), or the app never ran (check flow C `go`), or `CONFIG_UART_CONSOLE` left enabled (must be `n`). |
| J-Link: `Could not connect to the target device` | You used the **Alif part-number** device. Switch to the generic `-device Cortex-M55`. |
| J-Link: `Could not find core in CoreSight setup` | Fresh/un-provisioned SoM — the SES holds the M55. Provision an app first (flow A / [`aen-provisioning.md`](aen-provisioning.md)); then the debug-AP comes alive. |
| Wrong SW-DP IDR (not `0x4C013477`) | Wrong target or reversed SWD wiring. (`0x6BA02477` is the GD32/Cortex-M33 in this repo — that means you're on the wrong chip.) |
| After a J-Link reset the RAM-run image is gone | A reset is **SYSRESETREQ** → reboots the **SES**; ITCM contents and your `go` are lost. Re-`loadbin`/`setpc`/`go`; don't reset mid-loop. |
