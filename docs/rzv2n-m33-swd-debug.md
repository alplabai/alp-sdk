# RZ/V2N Cortex-M33 SWD debug checklist — "why does the M33 firmware never reach its console?"

Bench procedure for halting the on-SoC **Cortex-M33** over SWD and naming, in one
session, exactly where/why it faults. Use this when the symptom is: **BL2 loads +
releases the CM33 (`CPG_RSTMON_0 & 0xE0000 == 0`), but no SCI is ever configured —
not even the `sci0`/`uart0` console** (verified from A55 Linux `/dev/mem`).

## What we already know (so you don't re-chase it)
Established from the A55 side + source — see `docs/rzv2n-m33-secure-boot.md`:
- CM33 **is out of reset / running**; boot vector `SYS_MCPU_CFG2 = 0x08003000` ✓.
- FW image loaded correctly to SRAM0 (vector SP + reset-PC point into real code, not the 0x3000 pad).
- Reproduces on **stock `hello_world` for `rzv2n_evk`** ⇒ it's the base M33 port, not the SPI/app work.
- TF-A gives the CM33 everything: all peripheral clocks released in the main boot path
  (RSCI0/RSCI7/RIIC8/RSPI), CM33 core clock on, standard master/slave access-control.
- `CONFIG_ARM_MPU/CACHE/FPU` are all **off**; arch HW init (`z_arm_init_arch_hw_at_boot`)
  is core-NVIC-only and was empirically ruled out.

⇒ The fault is a **runtime fault in the first instructions the CM33 executes** (Zephyr
RZ/V2N M33 port + FSP BSP) on the TF-A-released **secure** state. Only the SWD can see it
(SRAM0 + core regs are off-limits to Linux).

## 0. Connect & halt
The CM33 is reached through the **SoC debug port** (the RZ/V2N JTAG/SWD — **not** the GD32's
J-Link port), selecting the CM33 access-port.
- J-Link device: try the RZ/V2N CM33 part (`R9A09G056xx_CM33` / closest); else generic
  `Cortex-M33` (may need a JLinkScript to select the CM33 AP). `-if SWD`.
- **Attach to the running core — do NOT "connect under reset"** (we want its live faulted state).
- `J-Link> halt`
- `J-Link> regs`   → note **PC, MSP, PSP, LR, xPSR**, and the **security state** if shown.

If attach fails: the core may be in lockup/WFI — connecting without reset and retrying
`halt` usually still latches it.

## 1. Core fault registers  (`mem32 <addr> 1`)
| Register | Addr | Meaning |
|---|---|---|
| **CFSR**  | `0xE000ED28` | fault type: MMFSR[7:0] / **BFSR[15:8]** / **UFSR[31:16]** |
| **HFSR**  | `0xE000ED2C` | **FORCED** (bit30 = escalated config fault → read CFSR), **VECTTBL** (bit1 = bad vector fetch) |
| **BFAR**  | `0xE000ED38` | faulting **bus address** — valid iff **CFSR bit15 (BFARVALID)** |
| **MMFAR** | `0xE000ED34` | faulting MPU address — valid iff CFSR bit7 (MMARVALID) |
| **VTOR**  | `0xE000ED08` | vector base — **must read `0x08003000`** |

If `PC` is inside a fault / Default_Handler, the **real fault site = stacked PC**:
`mem32 (MSP + 0x18) 1`  (exception frame = R0,R1,R2,R3,R12,LR,**PC**,xPSR).

## 2. Interpretation matrix
| Reading | Conclusion → fix direction |
|---|---|
| **BFAR = `0x4280_xxxx`** + BusFault | CM33 can't reach the **SCI** → per-peripheral **security attribution / TZC-PC** not granting the CM33 master (clocks are on, access ≠ security) |
| **BFAR = `0x1042_xxxx`** + BusFault | can't reach the **CPG** (`R_BSP_MODULE_START`) — same root, on the clock controller |
| **BFAR = `0x4041_xxxx`** + BusFault | can't reach the **PFC** (pinctrl) — same root |
| **CFSR UFSR.INVSTATE** (bit17) | Thumb-bit / **secure-state** mismatch (reset-PC LSB, or a bad `EXC_RETURN`) |
| **CFSR UFSR.UNDEFINSTR** (bit16) | wrong code at PC, or a **secure-only instruction** in the wrong state |
| **CFSR UFSR.NOCP** (bit19) | FPU/coproc used before enable (FPU is OFF in our build → would be a surprise) |
| **HFSR.VECTTBL** (bit1) | vector at VTOR **unreadable** (VTOR wrong, or SRAM not visible to the CM33 in this state) |
| PC = `0xFFFFFFFE` / core shows **LOCKUP** | double fault (fault inside the fault handler) |
| PC in `0x0800_xxxx` app loop, sci0 configured | CM33 fine — contradicts the symptom, so not expected |

## 3. Two confirming reads
1. `mem32 0x08003000 2` — the SWD **can** read secure SRAM0 (Linux can't). Confirms the live
   **SP (word0)** + **reset-PC (word1)** match the flashed image.
2. **Single-step / breakpoint:** break at the `sci0` UART init / first `R_BSP_MODULE_START`
   (or just `step` from the reset vector) and watch the exact instruction/access that faults.

## 4. Leading hypotheses (most → least likely, given everything upstream is clean)
1. **BusFault, BFAR in peripheral space (0x4280/0x1042/0x4041)** — the CM33 master's
   **peripheral security attribution** isn't configured. TF-A releases the *clocks* but the
   secure CM33's per-peripheral access (TZC peripheral-protection / the RZ security
   attribution unit) is left at a default that blocks it. Fix is in the BL2 M33-boot path.
2. **UsageFault INVSTATE/UNDEFINSTR very early** — the Zephyr image must run in the
   TF-A-released **secure** state but isn't built secure-aware (`CONFIG_ARM_SECURE_FIRMWARE`
   / TrustZone-M not set on the `rzv2n_evk` board). Fix is in the Zephyr board/SoC config.
3. **HFSR.VECTTBL** — the CM33 can't fetch its own vector table at 0x08003000 in this state.

Whichever it is, the **CFSR + BFAR pair from step 1 names it outright** — that is the whole
point of this read.
