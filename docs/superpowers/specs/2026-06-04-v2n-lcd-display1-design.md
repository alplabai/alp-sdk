# RK055HDMIPI4MA0 LCD port — E1M-X V2N/V2M, Display 1 (design)

Date: 2026-06-04
Status: approved (maintainer, 2026-06-04)
Scope: E1M-V2N101/102 + E1M-V2M101/102 on the E1M-X EVK carrier, Linux/Yocto (A55)

## 1. Goal

Bring up the RK055HDMIPI4MA0 MIPI-DSI panel (Rocktech 5.5", 720x1280, Himax
HX8394-F controller, Goodix GT911 touch, 40-pin FFC — NXP's i.MX reference
panel) on the X-EVK **Display 1** connector, end to end under Linux:
pixels + touch + backlight + a shipping SDK example + docs. The full stack
runs on the A55 cluster (Yocto, kernel `linux-renesas` 6.1.141-cip43,
BSP v6.30); nothing display-related runs on the CM33.

## 2. Hardware ground truth

Verified against the carrier V2 netlist, the SoM schematic (rev 4025-P1),
the E1M-X connector spec (x-v1.0), the V2N SoM pad maps, and the Renesas
RZ/V2N datasheet R01DS0466 rev 1.20. Corrections to previously documented
assumptions are marked **(corrects docs)**.

### Display path

- RZ/V2N has exactly **one MIPI-DSI** instance (1/2/4 lanes, max
  1.5 Gbps/lane, D-PHY v1.2). It lands on the E1M-X connector **DSI0**
  lane set (4 data pairs + clock), which feeds the carrier **Display 1**
  FFC (J6) directly — ESD clamps only, no mux, no bridge.
- **There is no IT6162 and no PI3WVR648 on the X-EVK** — both display
  paths are wired point-to-point. **(corrects docs/boards/e1m-x-evk.md,
  which listed them as unconfirmed bridge/mux candidates.)**
- Display 2 (J28) is wired to the connector **DSI1** lane set, which the
  V2N family does not populate → **Display 2 can never work on V2N/V2M**.
  Single-panel (Display 1) is the only path and the permanent scope.
- Carrier Display sheets are titled "RK055HDMIPI4MA0": the carrier was
  designed for exactly this panel.

### Display-1 FFC (J6) signals that matter

| FFC pin | Net | Source/sink on V2N |
|---|---|---|
| 5..18 | DSI0 lanes (4 data + clk) | RZ/V2N DSI, direct |
| 21 | `LCD1_RST_L` | E1M IO13 → **GD32 PF1** (bridge GPIO, active-low) |
| 22 | panel LPTE | **not connected** on carrier (no TE; video mode only) |
| 25 / 30 / 39+40 | +L1_VIO / +3V3 / +5V | always-on rails; L1_VIO strap-selected (header P1: 1V8/3V3/VIO) |
| 26 / 27 | touch I2C SDA/SCL | **E1M_X_I2C3 (DSI1_CSI_I2C)** → GD32 PC8/PC9 (bridge secondary slave-transport port) — no Linux master; touch gated on a GD32 I2C-proxy (O1, resolved) |
| 28 | `CTP1_RST_L` | E1M IO11 → **GD32 PB0** (bridge GPIO, active-low) |
| 29 | `CTP1_INT_L` | E1M IO9 → **GD32 PA7** (no Linux IRQ path → polled touch) |
| 32 | `LCD1_PWR_EN_L` | E1M IO15 — unrouted on V2N, **10k pull-up holds it enabled**; no SW action needed |
| 1 / 2 | `BL_LED_K` / `BL_LED_A` | SoM on-module backlight boost (KTD2801-class), `CNTRL` driven by Renesas **PA5 (BL_PWM)** |

Sideband lines pass through LSF0102 level shifters on the carrier.
Touch I2C pull-ups 4.7k to +VIO.

### Silicon gate (DCS)

Datasheet R01DS0466 rev 1.20 (notes on pages 3 and 7): parts suffixed
**#AC0/#BC0 do not support controlling MIPI LCDs with the Display Command
Set over MIPI DSI**. The HX8394-F requires a DCS/MCS init sequence over
DSI, so the panel cannot be initialized on #AC0 silicon. The restriction
is lifted on later die-revision suffixes of the same base part
(R9A09G056N44GBG). The SoM MIPI revision moving to the DCS-capable part
is in progress; **the current bench board already carries the new
silicon** (maintainer, 2026-06-04), so full validation is unblocked.
Older #AC0 boards will fail at panel init — that is the documented
silicon restriction, not a port bug. SoM spec docs still cite #AC0 and
need a revision (out of scope here; flagged to the hardware owner).

### Software baseline (BSP kernel, already built-in)

- DU: `drivers/gpu/drm/renesas/rz-du/`, compatible `renesas,r9a09g056-du`.
- DSI host: `rzg2l_mipi_dsi.c`, compatible `renesas,r9a09g056-mipi-dsi`,
  with `host_transfer` (DCS command transmission) implemented.
- SoC dtsi `r9a09g056.dtsi` declares `dsi0`/`du`/`vspd`/`fcpvd`
  (status=disabled). VSP1/FCP compiled in.
- GPT PWM: `gpt-rzg2l.c`, compatible `renesas,gpt-r9a09g056`; `gpt0_*`
  nodes in the SoC dtsi; the Renesas EVK dts has a worked pinmux example.
- Missing: every `CONFIG_DRM_PANEL_*` (no panel driver built),
  `CONFIG_BACKLIGHT_CLASS_DEVICE`/`_PWM`, `CONFIG_TOUCHSCREEN_GOODIX`.
  `panel-himax-hx8394.c` does not exist in 6.1 (mainline added it in 6.3)
  and no tree has an RK055 compatible.

## 3. Decision

**Approach A** (maintainer-selected): Linux owns the whole display stack;
the GD32-owned sideband pins are exposed to Linux through a small
`gpio-gd32-bridge` gpiochip driver that speaks the existing bridge
protocol (`CMD_GPIO_READ`/`CMD_GPIO_WRITE`, protocol >= v0.1) over the
bridge's I2C slave transport on BRD_I2C (i2c8) at 0x70. No bridge
firmware or protocol changes are needed or permitted from this work
(the bridge firmware is owned by a parallel workstream).

Rejected alternatives: one-shot userspace/U-Boot reset (not shippable:
no DPMS, no touch reset); CM33-owned sideband (couples panel probe to
CM33 firmware lifecycle, splits display ownership across cores).

Panel link config: **2-lane, RGB565, 62.346 MHz pixel clock** — NXP's
proven reference configuration (~748 Mbps/lane, comfortably under
1.5 Gbps/lane). 4-lane/RGB888 experiments are explicitly out of scope.

## 4. Deliverables

All kernel work ships as patches/files in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/` via the existing
bbappend (same mechanism as the RSCI7/RIIC8 clk patch and the
`tas2563-audio.cfg` fragment).

1. **Panel driver patch** — backport mainline `panel-himax-hx8394.c`
   (introduced v6.3) onto 6.1-cip; add compatible
   `rocktech,rk055hdmipi4ma0` with the init table from NXP's MCUXpresso
   `fsl_hx8394` source and the NXP timing set (720x1280@60, hs 6/12/24,
   vs 2/16/14, 62.346 MHz). Reset handled by the standard
   `reset-gpios` flow.
2. **`gpio-gd32-bridge.c` driver patch** — I2C client driver registering
   a gpiochip (no irqchip). Line space = the GD32 bridge GPIO pin
   namespace; DT child of `&i2c8` at `reg = <0x70>`. Probe runs a
   best-effort version handshake (logged) but registers the gpiochip
   **unconditionally** — a `-EPROBE_DEFER` would hold every consumer of
   these lines (notably the panel reset), and thus the whole DSI/DU
   pipeline, hostage to the bridge's liveness. Get/set map 1:1 onto
   `CMD_GPIO_READ`/`CMD_GPIO_WRITE` frames and fail gracefully
   (`dev_warn`) when the bridge is down.
3. **`goodix.c` polled-mode patch** — when the GT911 node has no
   `interrupts` property, fall back to `input_setup_polling` (~60 Hz
   poll). Touch INT physically terminates at the GD32 and cannot raise
   a Linux IRQ.
4. **`display.cfg` Kconfig fragment** — `CONFIG_DRM_PANEL_HIMAX_HX8394=y`,
   `CONFIG_BACKLIGHT_CLASS_DEVICE=y`, `CONFIG_BACKLIGHT_PWM=y`,
   `CONFIG_TOUCHSCREEN_GOODIX=y` (+ any selected deps), wired
   per-machine for `e1m-v2n101` and `e1m-v2m101` (pattern: tas2563).
5. **Device tree**:
   - `e1m-v2n-som.dtsi` (on-module facts): `gd32_gpio: gpio@70` on
     `&i2c8` (`gpio-controller`, `#gpio-cells = <2>`); GPT instance +
     `RZV2N_PORT_PINMUX` for PA5 (function index from the HW manual);
     `backlight: pwm-backlight` fed by that GPT channel (the boost
     driver is on-module).
   - `e1m-x-evk.dtsi` (carrier enables): `&dsi0`, `&du`, `&vspd`,
     `&fcpvd` → okay; `panel@0` on the DSI bus (compatible
     `"rocktech,rk055hdmipi4ma0", "himax,hx8394"`, `reset-gpios` pointing
     at the `&gd32_gpio` line for GD32 PF1 (active-low), `backlight`, vcc/iovcc
     fixed-regulator stubs for the always-on rails) with OF-graph to
     `dsi0_out`, `data-lanes = <1 2>`. (GT911 DT node DROPPED from this
     port — the touch bus terminates at the GD32, see O1; the node ships
     with the future bridge I2C-proxy adapter.)
   - Both V2N and V2M board dts files inherit unchanged (carrier dtsi
     composition; pad routing identical across all four SKUs).
6. **Image**: pull Weston/Mali userspace into `alp-image-edge` via the
   Renesas graphics layer (`meta-rz-features/meta-rz-graphics`) plus
   `libdrm-tests` (`modetest`) for bench validation.
7. **Example**: `examples/display/lvgl-dashboard-x-evk` — first
   Linux-side display example (LVGL on DRM/KMS), plus its Yocto recipe
   in `meta-alp-sdk/recipes-examples/`. Examples are documentation:
   teaching-grade comments, both consumer paths (image recipe and
   standalone cross-build) covered in its README.
8. **Docs/metadata truth-up**:
   - `docs/boards/e1m-x-evk.md`: remove IT6162/PI3WVR648 speculation,
     document the direct-wired display paths, Display-2-dead-on-V2N,
     the IO15 pull-up behavior, and the #AC0 silicon gate.
   - `metadata/boards/e1m-x-evk.yaml`: no schema change required;
     display sideband entries already correct (IO13/IO15/IO9/IO11).
     Add a comment recording the Display-2/DSI1 limitation for V2N SoMs.
   - `docs/os-support-matrix.md`: V2N Yocto Display row stub → GA only
     after HIL passes.
   - GD32 pin-map TSV note: PF1/PB0/PA7 lines are display sideband
     consumed from Linux via the bridge I2C transport.

## 5. Data flow / error handling

- Boot: `gd32_gpio` registers its gpiochip on i2c8 (best-effort
  handshake, **no defer**) → panel probe finds reset GPIO → DSI host
  powers D-PHY → panel `prepare()` toggles reset, sends HX8394 init over
  DCS LP → video mode on → `pwm-backlight` ramps. GT911 probes on i2c2,
  resets via bridge GPIO, polls at ~60 Hz.
- Bridge unreachable (GD32 unflashed/held in reset): the gpiochip still
  registers; GPIO ops fail gracefully (`dev_warn`). The panel reset
  becomes a best-effort no-op, so the panel / DSI / DU **still come up**
  (the hx8394 driver tolerates a missing reset); only the bridge-routed
  lines are inert until the GD32 answers. The equivalent end-state was
  bench-validated 2026-06-08 (a no-reset dtb brought `card0` + `fb0` up
  with the GD32 down, proving the panel needs no working reset; the
  hx8394 `prepare()` already tolerates a failed reset toggle). On-silicon
  validation of *this* driver fix paired with the standard reset-gpios
  dtb is the one remaining bench step. No display defer, no panics, no
  silent skips.
- Panel absent: HX8394 init times out → probe fails gracefully; DRM
  pipeline stays idle; Weston falls back to headless.
- DPMS off/on and suspend/resume re-run the reset/init sequence through
  the same gpiochip path (device-link ordering: gpiochip before panel).
- BRD_I2C contention: bridge transfers are short register frames on a
  shared bus also carrying RTC/PMIC/temp traffic; the I2C core
  serializes — no special handling beyond normal timeouts.
- #AC0 boards: panel init fails at first DCS write; dmesg shows DSI
  transfer errors. Documented as the silicon restriction with a pointer
  to the datasheet note (not retried in a loop).

## 6. Validation plan (bench: V2N on COM24, new-rev silicon)

0. Panel re-seated to J6 (Display 1). L1_VIO strap (P1) checked against
   panel IOVCC requirement before power-on.
1. Kernel + dtb built via the WSL Yocto tree; deployed over SSH
   (Image + dtb to /boot), reboot via detached ssh.
2. **DCS gate**: panel probe succeeds (init sequence ACKed) — proves the
   new silicon's DCS path. Record outcome either way.
3. `modetest -M rzg2l-du` pattern fill → pixels on glass.
4. `i2cdetect -y <riic2>` → GT911 ACK at 0x5D (empirically settles O1).
5. Backlight: sysfs brightness sweep 0..max (KTD2801 dimming via PA5).
6. `evtest` touch coordinates; poll latency sanity.
7. Weston session + LVGL example; 1 h soak (no DSI underruns/errors in
   dmesg).
8. Regression: GD32 bridge HIL soak still passes with display traffic
   on BRD_I2C (coordination point with the bridge workstream).
9. Repeat smoke (2-4) on a V2M101 board when one with new silicon is
   available — same PCB, expected identical.

## 7. Risks / open items

- **O1 — touch bus (RESOLVED 2026-06-04, maintainer)**: Display-1 touch
  = `DSI1_CSI_I2C` = **E1M_X_I2C3** (connector A23/A24), which on V2N
  routes to **GD32 PC8/PC9** — the bridge's planned secondary I2C
  slave transport, not a host bus. The GT911 therefore has NO bus
  master on V2N today. Touch ships in a follow-up gated on the GD32
  workstream: bridge I2C-master proxy on GD32 I2C2 + a Linux
  `i2c-gd32-bridge` adapter driver; the goodix polled patch (deliverable
  3) is retained dormant for exactly that stack (INT also terminates at
  the GD32, so polling remains required). Display scope is unaffected.
- **O2 — PA5 GPT function index**: exact `RZV2N_PORT_PINMUX(A,5,n)`
  function number and GPT channel must be read from the RZ/V2N HW manual
  (r01uh1071/1072) during implementation.
- **O3 — backport friction**: 6.3→6.1 DRM API deltas in the panel driver
  (e.g. `mipi_dsi_dcs_write_seq` availability) — expected small, handled
  in the patch.
- **O4 — GD32 firmware defaults**: PF1/PB0 must be writable as outputs
  with sane reset defaults in the deployed bridge firmware (v0.2.1).
  Verify with the bridge workstream; no protocol change anticipated.
- **O5 — stale SoM specs**: SoM datasheets still cite #AC0; hardware
  owner to revise when the new-silicon BOM lands (tracked outside this
  port).

## 8. Out of scope

- Display 2 / DSI1 (physically impossible on V2N/V2M).
- IT6162/PI3WVR648 support (not populated on the carrier).
- `<alp/display.h>` real backend (remains stub; tracked as issue #23 —
  Linux apps consume DRM/Wayland directly, and the example documents
  that consumer path).
- 4-lane / RGB888 link configs; DSC; command-mode panel operation.
- GD32 bridge firmware/protocol changes (parallel workstream owns them).
