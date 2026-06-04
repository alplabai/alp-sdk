# RK055HDMIPI4MA0 LCD Port (V2N/V2M Display 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Light up the RK055HDMIPI4MA0 MIPI-DSI panel (display + GT911 touch + backlight) on E1M-X V2N/V2M Display 1 under Yocto Linux, with a shipping LVGL example and truthful docs.

**Architecture:** DRM/KMS on the BSP's built-in rz-du DU + DSI host; backported `panel-himax-hx8394` with a new `rocktech,rk055hdmipi4ma0` desc; a new `gpio-gd32-bridge` I2C gpiochip exposes the GD32-owned sideband (panel/touch reset) to Linux; backlight is a native GPT PWM on PA5; touch is the stock goodix driver plus a polled-mode fallback patch.

**Tech Stack:** linux-renesas 6.1.141-cip43 (kernel worktree `/home/caner/projects/rzv2n/kbuild-cip43` in WSL `Ubuntu-22.04`), meta-alp-sdk bbappend patch/cfg/dtsi delivery, LVGL 9.1 (meta-oe, DRM backend), bench = V2N on COM24 (new DCS-capable silicon).

**Spec:** `docs/superpowers/specs/2026-06-04-v2n-lcd-display1-design.md` (approved 2026-06-04).

---

## Grounded constants (verified 2026-06-04 — do not re-derive)

**GD32 bridge I2C wire format** (firmware/gd32-bridge/src/transport_i2c.c, protocol.h, protocol.c):
- Slave addr **0x70** (7-bit). Bus: BRD_I2C = RIIC8 = `&i2c8` (`i2c@11c01000`), already `okay` @400 kHz in `e1m-x-evk.dtsi:152-157`. Slave clock-stretches; combined write-then-repeated-start-read works; STOP-then-read also works.
- Write: `[reg=0x00][CMD][payload…][CRC_lo][CRC_hi]` — CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB-first, LE on wire) over `CMD|payload` only. Min write = 4 bytes.
- Read reply: `[STATUS][payload…][CRC_lo][CRC_hi]` — **no SOF on I2C** — CRC over `STATUS|payload`. Reply length is opcode-derived; master must clock exactly `1+M+2` bytes. `STATUS_NO_PENDING=0x80` if read-before-write.
- Kernel CRC: `crc_itu_t(0xFFFF, buf, len)` (lib/crc-itu-t, poly 0x1021 MSB-first) == CRC-CCITT-FALSE. `select CRC_ITU_T`.
- `CMD_PING=0x00` (empty req, empty reply → read 3), `CMD_GET_VERSION=0x01` (empty req; reply 3 bytes major,minor,patch → read 6; reject major != 0), `CMD_GPIO_READ=0x10` (req mask:u32 LE, 4 bytes; reply levels:u32 → read 7), `CMD_GPIO_WRITE=0x11` (req mask:u32 + levels:u32, 8 bytes; empty reply → read 3). Status codes: 0x00 OK, 0x01 INVAL, 0x05 IO, 0x80 NO_PENDING.
- **GPIO bit map is a logical index, NOT port/pin** (bridge_hw_gd32.c:163-183), ngpio=18:
  `0=PB10/IO8, 1=PA7/IO9(CTP1_INT — never drive!), 2=PA12/IO10, 3=PB0/IO11(CTP1_RST), 4=PC1/IO12, 5=PF1/IO13(LCD1_RST), 6=PB5/IO14, 7=PC0/IO16, 8=PC14/IO24, 9=PC15/IO25, 10=PB11/IO27, 11=PC2/IO28, 12=PD11/IO29, 13=PD10/IO30, 14=PE12/IO31, 15=PD2/IO32, 16=PD8/IO34, 17=PD1/IO35`
- Boot default all pads INPUT+PULL_UP; **GPIO_WRITE auto-promotes the pad to OUTPUT push-pull, sticky until GD32 reset; no de-promote**. READ returns output-reg bit for outputs, input level for inputs.
- The GD32 **I2C transport is not yet silicon-validated** (SPI/SCI7 was). Bench gate G0 below validates it before anything depends on it.

**Pinmux/DT** (HW manual r01uh1072ej0120 Table 1.2-3; kbuild dtsi):
- PA5 backlight PWM: **Func 9 = GTIOC10B → `&gpt1_2` (gpt@13020200)**, B-channel ⇒ gpt node needs `channel = "channel_B"`. (Func 11 = GTIOC6B/`&gpt0_6` also exists but GTIOC6B is shared with P35 — do not use.) `#pwm-cells = <2>` (cell0 = hwpwm = 0, cell1 = period_ns); driver `gpt-rzg2l.c`, polarity cells ignored.
- P34 = RIIC2 **SDA2 Func 1**, P35 = RIIC2 **SCL2 Func 1** → controller `i2c2: i2c@14400c00` (status disabled in SoC dtsi).
- Macros `RZV2N_PORT_PINMUX(b,p,f)` / `RZV2N_GPIO(b,p)` are defined in `r9a09g056.dtsi` itself (banks `RZV2N_PA`=10); nothing extra to include.
- SoC dtsi labels: `dsi0` (dsi@16430000, ports: `dsi0_in`↔`du_out_dsi0`, `dsi0_out` empty), `du` (display@16460000), `vspd`/`fcpvd` (no status ⇒ enabled). EVK enables: `&du { status="okay"; }`, `&dsi0 { status="okay"; ports{port@1{dsi0_out: endpoint{remote-endpoint=<&adv7535_in>; data-lanes=<1 2 3 4>;};};}; }` — ours uses `data-lanes = <1 2>` and points at the panel.
- pwm-backlight (6.1 `pwm_bl.c`): required props `compatible="pwm-backlight"`, `pwms`, `power-supply`; optional `brightness-levels`, `default-brightness-level`.

**Panel** (mainline v6.6 `panel-himax-hx8394.c` + NXP `fsl_hx8394.c` BSD-3 + Zephyr overlay cross-check):
- v6.6 driver compiles on 6.1-cip **as-is** (`mipi_dsi_dcs_write_seq` exists at `drm_mipi_dsi.h:312`; `drm_panel_init` 4-arg; `.remove` void; `drm_panel_of_backlight` present). Only edit: drop the vestigial `#include <linux/media-bus-format.h>`.
- RK055 mode: `clock=62346`, h: 720/732/738/762 (fp 12, sync 6, bp 24), v: 1280/1296/1298/1312 (fp 16, sync 2, bp 14), `DRM_MODE_FLAG_NHSYNC|NVSYNC`, active area 68×121 mm (5.5"), **2 lanes, RGB565**.
- NXP init table (translate from BSD-3 `fsl_hx8394.c`; byte-identical in Zephyr — do NOT copy Apache code): `0x36 02; 0xB1 48 12 72 09 32 54 71 71 57 47; 0xB2 00 80 64 0C 0D 2F; 0xB4 73 74 73 74 73 74 01 0C 86 75 00 3F 73 74 73 74 73 74 01 0C 86; 0xD3 …(33B); 0xD5 …(44B); 0xD6 …(44B); 0xB6 92 92; 0xE0 …(58B); 0xC0 1F 31; 0xCC 03; 0xD4 02; 0xBD 02; 0xD8 FF×12; 0xBD 00; 0xBD 01; 0xB1 00; 0xBD 00; 0xBF 40 81 50 00 1A FC 01; 0xC6 ED; 0x35 00` (full byte runs in Task 2 code). Then exit_sleep (120 ms) + display_on — the mainline driver's enable() already does that.
- Two verify-at-implementation flags: (a) the real `fsl_hx8394.c` may contain a `0xBA` SETMIPI write the capture clipped — fetch and check; if present, include it with the **2-lane** lane-count nibble (`0x61`, not the 4-lane `0x63`); (b) confirm `MIPI_DSI_FMT_RGB565` exists in 6.1 `drm_mipi_dsi.h` and which `MIPI_DSI_MODE_VIDEO*` flags `rzg2l_mipi_dsi.c` accepts.

**Touch** (6.1 `goodix.c`): driver present, `CONFIG_TOUCHSCREEN_GOODIX` not set. IRQ hard-required at `goodix_configure_dev()`:1257-1264 via `goodix_request_irq()`:521-526. `input_setup_polling`/`input_set_poll_interval` exist (`input.h:389/391`). Poll callback = `goodix_process_events(ts)` + `goodix_i2c_write_u8(ts->client, GOODIX_READ_COOR_ADDR, 0)`. **GT911 I2C address straps off INT at reset**: INT high→0x14, low→0x5D; INT ends at GD32 PA7 (pull-up) ⇒ expect **0x14** likely; bench `i2cdetect` decides; never drive PA7 from the bridge (sticky output would fight GT911's INT output).

**Yocto:** No live bitbake tree — kernel iteration happens in `kbuild-cip43` (`make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs`, host gcc 11.4). meta-alp-sdk delivery: `linux-renesas_%.bbappend` `SRC_URI:append` (patches + .cfg) + `do_configure:prepend` (dtsi install list) + per-machine cfg appends (`:e1m-v2n101` / `:e1m-v2m101`). House patch style: starts `From: Alp Lab AB`, no personal emails. LVGL 9.1 packaged in meta-oe (`lvgl_9.1.0.bb`, PACKAGECONFIG drm ⇒ `LV_USE_LINUX_DRM=1`, `LV_USE_EVDEV=1`). `libdrm-tests` comes free under `DISTRO=rz-vlp`; weston = `IMAGE_INSTALL:append " weston weston-init"` (wayland/opengl already in distro features). alp-* example recipes use git+AUTOREV + `S=${WORKDIR}/git/examples/<cat>/<name>` + `inherit cmake`.

**Branch/commit conventions:** work on `feat/v2n-lcd-display1` in this worktree. Stage with `git add <paths>` in one call, then a **bare** `git commit -q -m "…"` as a separate call (the ECC hook false-positives on chained/amended commits). No Claude co-author footer. Attribution: Alp Lab AB.

---

### Task 0: Bench preflight — validate the GD32 I2C transport (gate G0)

No code. Do this at the bench (or queue it for the next bench window; Tasks 1–9 don't depend on it, Task 10 does).

**Files:** none.

- [ ] **Step 0.1: Move the panel to Display 1 (J6)** and check the L1_VIO strap header **P1** selects the rail matching the panel's IOVCC (1V8/3V3/VIO options; pick per the Rocktech datasheet — GT911 and HX8394 I/O are 1.8–3.3 V tolerant; 3V3 is the NXP-EVK-like default).

- [ ] **Step 0.2: Generate the PING/GET_VERSION frames** (CRC self-computed; don't trust hand-rolled hex):

```bash
python3 - <<'EOF'
def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c
for name, cmd in (("PING", 0x00), ("GET_VERSION", 0x01)):
    c = crc16(bytes([cmd]))
    print(f"{name}: i2ctransfer -y 8 w4@0x70 0x00 {cmd:#04x} {c & 0xFF:#04x} {c >> 8:#04x} r{3 if cmd == 0 else 6}")
EOF
```

- [ ] **Step 0.3: Run the printed `i2ctransfer` commands on the board** (ssh root@192.168.1.198, or COM24 console).
Expected PING: 3 bytes, first byte `0x00` (STATUS_OK), then a valid CRC.
Expected GET_VERSION: 6 bytes, `0x00` then `00 06 00`-ish (protocol 0.6.x), then CRC.
**If the transport misbehaves** (NACKs, garbage, stretch issues): STOP — escalate to the GD32-bridge workstream before Task 10; Tasks 1–9 remain valid.

- [ ] **Step 0.4: Record results** in `docs/superpowers/plans/2026-06-04-v2n-lcd-display1.md` (append a "Bench log" section at the bottom) — date, board, silicon marking if readable, PING/VERSION bytes.

---

### Task 1: Kernel patch 0002 — backport `panel-himax-hx8394.c` with the RK055 desc

**Files:**
- Create (in WSL kernel tree): `drivers/gpu/drm/panel/panel-himax-hx8394.c`
- Modify (kernel tree): `drivers/gpu/drm/panel/Kconfig`, `drivers/gpu/drm/panel/Makefile`
- Create (worktree): `meta-alp-sdk/recipes-kernel/linux/linux-renesas/0002-drm-panel-add-himax-hx8394-with-rocktech-rk055hdmipi.patch`

All kernel-tree commands run via: `wsl -d Ubuntu-22.04 -e bash -lc 'cd /home/caner/projects/rzv2n/kbuild-cip43 && <cmd>'`. The kernel worktree is at SHA 6717c06c — keep it clean apart from our staged work (`git stash` anything unrelated first; check `git status`).

- [ ] **Step 1.1: Verify the two open flags.**

```bash
grep -n "MIPI_DSI_FMT_RGB565" include/drm/drm_mipi_dsi.h
grep -n "MIPI_DSI_MODE_VIDEO" drivers/gpu/drm/renesas/rz-du/rzg2l_mipi_dsi.c | head -20
```
Expected: `MIPI_DSI_FMT_RGB565` present in the enum; the DSI host's `attach`/`mode_valid` shows which video-mode flags it honors. Decision rule: if the host checks for/requires `MIPI_DSI_MODE_VIDEO_SYNC_PULSE`, use `MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE` in the desc; if it accepts burst, keep `MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST`. Record the choice in the patch description.

- [ ] **Step 1.2: Fetch the v6.6 driver + the NXP init source.**

```bash
curl -fsSL https://raw.githubusercontent.com/torvalds/linux/v6.6/drivers/gpu/drm/panel/panel-himax-hx8394.c -o drivers/gpu/drm/panel/panel-himax-hx8394.c
curl -fsSL https://raw.githubusercontent.com/nxp-mcuxpresso/mcux-sdk/main/components/video/display/hx8394/fsl_hx8394.c -o /tmp/fsl_hx8394.c || true
grep -n "0xBA\|setmipi\|SETMIPI" /tmp/fsl_hx8394.c
```
If the mcux-sdk path 404s, find `fsl_hx8394.c` via GitHub code search (`repo:nxp-mcuxpresso/mcux-sdk fsl_hx8394`) and fetch the real path. Confirm whether a `0xBA` (SETMIPI) entry exists; if yes note its bytes — for 2-lane the first parameter must be `0x61`.

- [ ] **Step 1.3: Edit the driver** — apply exactly these changes to the fetched file:
1. Drop the `#include <linux/media-bus-format.h>` line (path is uapi-only in 6.1; symbol unused).
2. Add the RK055 mode + init + desc after the hannstar block (byte runs verbatim from the grounded table; full long arrays below), and the of_match entry:

```c
/* Init translated from NXP fsl_hx8394.c (BSD-3-Clause, Copyright 2021 NXP)
 * for the Rocktech RK055HDMIPI4MA0 (RK055MHD091 cell, HX8394-F).
 */
static int rk055hdmipi4ma0_init_sequence(struct hx8394 *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_SET_ADDRESS_MODE, 0x02);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETPOWER,
			       0x48, 0x12, 0x72, 0x09, 0x32, 0x54, 0x71, 0x71, 0x57, 0x47);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETDISP,
			       0x00, 0x80, 0x64, 0x0c, 0x0d, 0x2f);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETCYC,
			       0x73, 0x74, 0x73, 0x74, 0x73, 0x74, 0x01, 0x0c, 0x86, 0x75,
			       0x00, 0x3f, 0x73, 0x74, 0x73, 0x74, 0x73, 0x74, 0x01, 0x0c,
			       0x86);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETGIP0,
			       0x00, 0x00, 0x07, 0x07, 0x40, 0x07, 0x0c, 0x00, 0x08, 0x10,
			       0x08, 0x00, 0x08, 0x54, 0x15, 0x0a, 0x05, 0x0a, 0x02, 0x15,
			       0x06, 0x05, 0x06, 0x47, 0x44, 0x0a, 0x0a, 0x4b, 0x10, 0x07,
			       0x07, 0x0c, 0x40);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETGIP1,
			       0x1c, 0x1c, 0x1d, 0x1d, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
			       0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x24, 0x25, 0x18, 0x18,
			       0x26, 0x27, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
			       0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x20, 0x21,
			       0x18, 0x18, 0x18, 0x18);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETGIP2,
			       0x1c, 0x1c, 0x1d, 0x1d, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
			       0x01, 0x00, 0x0b, 0x0a, 0x09, 0x08, 0x21, 0x20, 0x18, 0x18,
			       0x27, 0x26, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
			       0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x25, 0x24,
			       0x18, 0x18, 0x18, 0x18);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETVCOM, 0x92, 0x92);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETGAMMA,
			       0x00, 0x0a, 0x15, 0x1b, 0x1e, 0x21, 0x24, 0x22, 0x47, 0x56,
			       0x65, 0x66, 0x6e, 0x82, 0x88, 0x8b, 0x9a, 0x9d, 0x98, 0xa8,
			       0xb9, 0x5d, 0x5c, 0x61, 0x66, 0x6a, 0x6f, 0x7f, 0x7f, 0x00,
			       0x0a, 0x15, 0x1b, 0x1e, 0x21, 0x24, 0x22, 0x47, 0x56, 0x65,
			       0x65, 0x6e, 0x81, 0x87, 0x8b, 0x98, 0x9d, 0x99, 0xa8, 0xba,
			       0x5d, 0x5d, 0x62, 0x67, 0x6b, 0x72, 0x7f, 0x7f);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_UNKNOWN1, 0x1f, 0x31);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETPANEL, 0x03);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_UNKNOWN3, 0x02);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETREGBANK, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xd8,
			       0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
			       0xff, 0xff, 0xff, 0xff, 0xff, 0xff);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETREGBANK, 0x00);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETREGBANK, 0x01);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETPOWER, 0x00);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_SETREGBANK, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xbf,
			       0x40, 0x81, 0x50, 0x00, 0x1a, 0xfc, 0x01);
	mipi_dsi_dcs_write_seq(dsi, HX8394_CMD_UNKNOWN2, 0xed);
	/* TE on (vblank) — harmless in pure video mode; carrier TE pin is NC. */
	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_SET_TEAR_ON, 0x00);
	/* If Step 1.2 found a SETMIPI (0xBA) entry in fsl_hx8394.c, add it
	 * HERE verbatim but with the lane field set for 2 lanes (0x61). */

	return 0;
}

static const struct drm_display_mode rk055hdmipi4ma0_mode = {
	.hdisplay    = 720,
	.hsync_start = 720 + 12,
	.hsync_end   = 720 + 12 + 6,
	.htotal      = 720 + 12 + 6 + 24,
	.vdisplay    = 1280,
	.vsync_start = 1280 + 16,
	.vsync_end   = 1280 + 16 + 2,
	.vtotal      = 1280 + 16 + 2 + 14,
	.clock       = 62346,
	.flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
	.width_mm    = 68,
	.height_mm   = 121,
};

static const struct hx8394_panel_desc rk055hdmipi4ma0_desc = {
	.mode          = &rk055hdmipi4ma0_mode,
	.lanes         = 2,
	.mode_flags    = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST, /* per Step 1.1 decision */
	.format        = MIPI_DSI_FMT_RGB565,
	.init_sequence = rk055hdmipi4ma0_init_sequence,
};
```
and in `hx8394_of_match[]`:
```c
	{ .compatible = "rocktech,rk055hdmipi4ma0", .data = &rk055hdmipi4ma0_desc },
```

- [ ] **Step 1.4: Add Kconfig + Makefile entries** (kernel tree):

`drivers/gpu/drm/panel/Kconfig` (alphabetical, after `DRM_PANEL_HIMAX_HX8279D`-area entries — 6.1 has `panel-boe-himax8279d`, so place after `DRM_PANEL_FEIYANG_FY07024DI26A30D`/near other H entries):
```
config DRM_PANEL_HIMAX_HX8394
	tristate "HIMAX HX8394 MIPI-DSI LCD panels"
	depends on OF
	depends on DRM_MIPI_DSI
	depends on BACKLIGHT_CLASS_DEVICE
	help
	  Say Y if you want to enable support for panels based on the
	  Himax HX8394 controller, such as the Rocktech RK055HDMIPI4MA0
	  720x1280 MIPI-DSI panel.
```
`drivers/gpu/drm/panel/Makefile` (alphabetical):
```
obj-$(CONFIG_DRM_PANEL_HIMAX_HX8394) += panel-himax-hx8394.o
```

- [ ] **Step 1.5: Compile-test in the kernel tree** (failing-then-passing is the TDD beat here — the first build catches API drift):

```bash
wsl -d Ubuntu-22.04 -e bash -lc 'cd /home/caner/projects/rzv2n/kbuild-cip43 && \
  ./scripts/config --enable CONFIG_BACKLIGHT_CLASS_DEVICE --enable CONFIG_BACKLIGHT_PWM --enable CONFIG_DRM_PANEL_HIMAX_HX8394 --enable CONFIG_TOUCHSCREEN_GOODIX --enable CONFIG_INPUT_TOUCHSCREEN --enable CONFIG_CRC_ITU_T && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) drivers/gpu/drm/panel/panel-himax-hx8394.o'
```
Expected: clean compile (warnings = fix before proceeding).

- [ ] **Step 1.6: Export the patch in house style.** In the kernel tree: `git add` the three files, `git commit -m "drm/panel: add himax-hx8394 with rocktech,rk055hdmipi4ma0 (backport from v6.6)"`, then `git format-patch -1 --no-signature -o /tmp/`. Edit the generated file: replace the `From:` author line with `From: Alp Lab AB <sdk@alplab.se>` style used by `0001-clk-…patch` (open it and mirror its exact header form — no personal names), prepend the body with the provenance note (mainline v6.6 backport + NXP BSD-3 init translation + the Step 1.1 mode-flags decision). Copy to the worktree as `meta-alp-sdk/recipes-kernel/linux/linux-renesas/0002-drm-panel-add-himax-hx8394-with-rocktech-rk055hdmipi.patch`.

- [ ] **Step 1.7: Commit (SDK worktree).**

```bash
git add meta-alp-sdk/recipes-kernel/linux/linux-renesas/0002-drm-panel-add-himax-hx8394-with-rocktech-rk055hdmipi.patch
```
then (separate call) `git commit -q -m "feat(linux): backport himax-hx8394 panel driver with rocktech,rk055hdmipi4ma0 desc"`

---

### Task 2: Kernel patch 0003 — `gpio-gd32-bridge` gpiochip driver

**Files:**
- Create (kernel tree): `drivers/gpio/gpio-gd32-bridge.c`
- Modify (kernel tree): `drivers/gpio/Kconfig`, `drivers/gpio/Makefile`
- Create (worktree): `meta-alp-sdk/recipes-kernel/linux/linux-renesas/0003-gpio-add-gd32-bridge-expander-driver.patch`

- [ ] **Step 2.1: Write the driver** (complete file — this is the whole driver, grounded against the wire format above):

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO driver for the Alp Lab GD32 IO-MCU bridge (I2C transport).
 *
 * The E1M V2N-family SoM routes a set of E1M IO pads through a GD32G553
 * supervisor MCU. This driver exposes the bridge's logical GPIO map as a
 * Linux gpiochip over the bridge's I2C slave (BRD_I2C @ 0x70) using the
 * bridge wire protocol CMD_GPIO_READ/CMD_GPIO_WRITE.
 *
 * Wire notes (bridge protocol v0.6):
 *  - write:  [reg 0x00][cmd][payload][crc16 lo][crc16 hi]
 *  - read:   [status][payload][crc16 lo][crc16 hi]   (no SOF on I2C)
 *  - CRC-16/CCITT-FALSE over cmd|payload (write) / status|payload (read).
 *  - Reply length is opcode-derived; the slave clock-stretches until ready.
 *  - GPIO_WRITE auto-promotes a pad to push-pull output, sticky until the
 *    GD32 resets; there is no demote-to-input. direction_input() is
 *    therefore only honoured for never-written lines.
 *
 * Copyright (c) 2026 Alp Lab AB
 */

#include <linux/crc-itu-t.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define GD32_REG_CMD		0x00
#define GD32_CMD_PING		0x00
#define GD32_CMD_GET_VERSION	0x01
#define GD32_CMD_GPIO_READ	0x10
#define GD32_CMD_GPIO_WRITE	0x11
#define GD32_STATUS_OK		0x00

#define GD32_NGPIO		18

static const char * const gd32_bridge_gpio_names[GD32_NGPIO] = {
	"IO8/PB10",  "IO9/PA7",   "IO10/PA12", "IO11/PB0",  "IO12/PC1",
	"IO13/PF1",  "IO14/PB5",  "IO16/PC0",  "IO24/PC14", "IO25/PC15",
	"IO27/PB11", "IO28/PC2",  "IO29/PD11", "IO30/PD10", "IO31/PE12",
	"IO32/PD2",  "IO34/PD8",  "IO35/PD1",
};

struct gd32_bridge_gpio {
	struct i2c_client *client;
	struct gpio_chip chip;
	struct mutex lock;		/* serialize cmd/reply pairs */
	u32 output_mask;		/* lines promoted to output (sticky) */
	u32 output_vals;		/* last driven levels */
};

static int gd32_bridge_xfer(struct gd32_bridge_gpio *gd32, u8 cmd,
			    const u8 *req, unsigned int req_len,
			    u8 *rsp, unsigned int rsp_len)
{
	struct i2c_client *cl = gd32->client;
	u8 wbuf[2 + 8 + 2];	/* reg + cmd + max payload + crc */
	u8 rbuf[1 + 4 + 2];	/* status + max payload + crc */
	unsigned int wlen = 2 + req_len + 2;
	unsigned int rlen = 1 + rsp_len + 2;
	struct i2c_msg msgs[2];
	u16 crc;
	int ret;

	if (WARN_ON(req_len > 8 || rsp_len > 4))
		return -EINVAL;

	wbuf[0] = GD32_REG_CMD;
	wbuf[1] = cmd;
	memcpy(&wbuf[2], req, req_len);
	crc = crc_itu_t(0xFFFF, &wbuf[1], 1 + req_len);
	wbuf[2 + req_len] = crc & 0xFF;
	wbuf[3 + req_len] = crc >> 8;

	msgs[0].addr  = cl->addr;
	msgs[0].flags = 0;
	msgs[0].len   = wlen;
	msgs[0].buf   = wbuf;
	msgs[1].addr  = cl->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len   = rlen;
	msgs[1].buf   = rbuf;

	mutex_lock(&gd32->lock);
	ret = i2c_transfer(cl->adapter, msgs, 2);
	mutex_unlock(&gd32->lock);
	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;

	crc = crc_itu_t(0xFFFF, rbuf, 1 + rsp_len);
	if (rbuf[1 + rsp_len] != (crc & 0xFF) ||
	    rbuf[2 + rsp_len] != (crc >> 8))
		return -EBADMSG;
	if (rbuf[0] != GD32_STATUS_OK)
		return -EIO;
	if (rsp_len)
		memcpy(rsp, &rbuf[1], rsp_len);
	return 0;
}

static int gd32_bridge_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct gd32_bridge_gpio *gd32 = gpiochip_get_data(chip);
	u8 req[4], rsp[4];
	u32 mask = BIT(offset), levels;
	int ret;

	put_unaligned_le32(mask, req);
	ret = gd32_bridge_xfer(gd32, GD32_CMD_GPIO_READ, req, 4, rsp, 4);
	if (ret)
		return ret;
	levels = get_unaligned_le32(rsp);
	return !!(levels & mask);
}

static void gd32_bridge_gpio_set(struct gpio_chip *chip, unsigned int offset,
				 int value)
{
	struct gd32_bridge_gpio *gd32 = gpiochip_get_data(chip);
	u8 req[8];
	u32 mask = BIT(offset);

	put_unaligned_le32(mask, &req[0]);
	put_unaligned_le32(value ? mask : 0, &req[4]);
	if (!gd32_bridge_xfer(gd32, GD32_CMD_GPIO_WRITE, req, 8, NULL, 0)) {
		gd32->output_mask |= mask;
		if (value)
			gd32->output_vals |= mask;
		else
			gd32->output_vals &= ~mask;
	}
}

static int gd32_bridge_gpio_dir_out(struct gpio_chip *chip,
				    unsigned int offset, int value)
{
	/* The bridge promotes a pad to output on first write. */
	gd32_bridge_gpio_set(chip, offset, value);
	return 0;
}

static int gd32_bridge_gpio_dir_in(struct gpio_chip *chip, unsigned int offset)
{
	struct gd32_bridge_gpio *gd32 = gpiochip_get_data(chip);

	/* Sticky promotion: once written, a pad cannot return to input. */
	if (gd32->output_mask & BIT(offset))
		return -EPERM;
	return 0;	/* boot default is already input + pull-up */
}

static int gd32_bridge_gpio_get_dir(struct gpio_chip *chip,
				    unsigned int offset)
{
	struct gd32_bridge_gpio *gd32 = gpiochip_get_data(chip);

	return (gd32->output_mask & BIT(offset)) ? GPIO_LINE_DIRECTION_OUT
						 : GPIO_LINE_DIRECTION_IN;
}

static int gd32_bridge_gpio_probe(struct i2c_client *client)
{
	struct gd32_bridge_gpio *gd32;
	u8 ver[3];
	int ret;

	gd32 = devm_kzalloc(&client->dev, sizeof(*gd32), GFP_KERNEL);
	if (!gd32)
		return -ENOMEM;

	gd32->client = client;
	mutex_init(&gd32->lock);

	/* Probe handshake: PING, then version gate (wire major must be 0). */
	ret = gd32_bridge_xfer(gd32, GD32_CMD_PING, NULL, 0, NULL, 0);
	if (ret)
		return dev_err_probe(&client->dev, -EPROBE_DEFER,
				     "bridge not answering (%d)\n", ret);
	ret = gd32_bridge_xfer(gd32, GD32_CMD_GET_VERSION, NULL, 0, ver, 3);
	if (ret)
		return dev_err_probe(&client->dev, ret, "GET_VERSION failed\n");
	if (ver[0] != 0)
		return dev_err_probe(&client->dev, -ENODEV,
				     "unsupported bridge protocol %u.%u.%u\n",
				     ver[0], ver[1], ver[2]);
	dev_info(&client->dev, "GD32 bridge protocol %u.%u.%u\n",
		 ver[0], ver[1], ver[2]);

	gd32->chip.label	  = "gd32-bridge-gpio";
	gd32->chip.parent	  = &client->dev;
	gd32->chip.owner	  = THIS_MODULE;
	gd32->chip.base		  = -1;
	gd32->chip.ngpio	  = GD32_NGPIO;
	gd32->chip.names	  = gd32_bridge_gpio_names;
	gd32->chip.get		  = gd32_bridge_gpio_get;
	gd32->chip.set		  = gd32_bridge_gpio_set;
	gd32->chip.direction_output = gd32_bridge_gpio_dir_out;
	gd32->chip.direction_input  = gd32_bridge_gpio_dir_in;
	gd32->chip.get_direction    = gd32_bridge_gpio_get_dir;
	gd32->chip.can_sleep	  = true;

	i2c_set_clientdata(client, gd32);
	return devm_gpiochip_add_data(&client->dev, &gd32->chip, gd32);
}

static const struct of_device_id gd32_bridge_gpio_of_match[] = {
	{ .compatible = "alplab,gd32-bridge-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, gd32_bridge_gpio_of_match);

static struct i2c_driver gd32_bridge_gpio_driver = {
	.driver = {
		.name = "gpio-gd32-bridge",
		.of_match_table = gd32_bridge_gpio_of_match,
	},
	.probe_new = gd32_bridge_gpio_probe,
};
module_i2c_driver(gd32_bridge_gpio_driver);

MODULE_AUTHOR("Alp Lab AB");
MODULE_DESCRIPTION("GPIO driver for the Alp Lab GD32 IO-MCU bridge");
MODULE_LICENSE("GPL");
```

Add `#include <asm/unaligned.h>` if `put_unaligned_le32`/`get_unaligned_le32` are unresolved (6.1 location).

- [ ] **Step 2.2: Kconfig + Makefile** (kernel tree):

`drivers/gpio/Kconfig` (in the I2C GPIO expanders section, alphabetical near `GPIO_FXL6408`):
```
config GPIO_GD32_BRIDGE
	tristate "Alp Lab GD32 IO-MCU bridge GPIO expander"
	depends on I2C && OF
	select CRC_ITU_T
	help
	  GPIO support for E1M IO pads routed through the GD32G553
	  supervisor MCU on Alp Lab V2N-family SoMs, accessed over the
	  bridge's I2C slave interface.
```
`drivers/gpio/Makefile`:
```
obj-$(CONFIG_GPIO_GD32_BRIDGE) += gpio-gd32-bridge.o
```

- [ ] **Step 2.3: Compile-test:**

```bash
wsl -d Ubuntu-22.04 -e bash -lc 'cd /home/caner/projects/rzv2n/kbuild-cip43 && \
  ./scripts/config --enable CONFIG_GPIO_GD32_BRIDGE && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) drivers/gpio/gpio-gd32-bridge.o'
```
Expected: clean compile. Note: 6.1 i2c uses `.probe_new` (as written); if the build complains, switch to `.probe` with the `(client, id)` signature.

- [ ] **Step 2.4: Export patch 0003** (same flow as Step 1.6; commit message `gpio: add Alp Lab GD32 bridge expander driver`), copy to `meta-alp-sdk/recipes-kernel/linux/linux-renesas/0003-gpio-add-gd32-bridge-expander-driver.patch`.

- [ ] **Step 2.5: Commit (SDK worktree):** stage the patch file, then `git commit -q -m "feat(linux): gpio-gd32-bridge expander driver over BRD_I2C"`

---

### Task 3: Kernel patch 0004 — goodix polled-mode fallback

**Files:**
- Modify (kernel tree): `drivers/input/touchscreen/goodix.c`
- Create (worktree): `meta-alp-sdk/recipes-kernel/linux/linux-renesas/0004-input-goodix-fall-back-to-polling-without-an-irq.patch`

- [ ] **Step 3.1: Apply the poll fallback.** In `goodix.c`:

1. Next to `goodix_ts_irq_handler` (after goodix.c:514), add:
```c
static void goodix_ts_poll(struct input_dev *input)
{
	struct goodix_ts_data *ts = input_get_drvdata(input);

	goodix_process_events(ts);
	goodix_i2c_write_u8(ts->client, GOODIX_READ_COOR_ADDR, 0);
}
```
2. In `goodix_configure_dev()`, replace the unconditional IRQ request (the `ts->irq_flags = …; error = goodix_request_irq(ts); if (error) …` block at ~:1257-1264) with:
```c
	if (ts->client->irq > 0) {
		ts->irq_flags = goodix_irq_flags[ts->int_trigger_type] | IRQF_ONESHOT;
		error = goodix_request_irq(ts);
		if (error) {
			dev_err(&ts->client->dev, "request IRQ failed: %d\n", error);
			return error;
		}
	} else {
		error = input_setup_polling(ts->input_dev, goodix_ts_poll);
		if (error) {
			dev_err(&ts->client->dev, "could not set up polling: %d\n", error);
			return error;
		}
		input_set_poll_interval(ts->input_dev, 17); /* ~60 Hz */
	}
```
3. Before `input_register_device(ts->input_dev)` (~:1240), ensure `input_set_drvdata(ts->input_dev, ts);` is present — add it if missing (check first: `grep -n input_set_drvdata drivers/input/touchscreen/goodix.c`). NOTE: `input_setup_polling` must be called BEFORE `input_register_device`; if the existing code order has register before the IRQ block, restructure so the polling setup happens before registration (move the branch, keep the IRQ request after registration as in the original flow — only the polling half must precede registration; verify against the actual function order when editing).
4. Guard suspend/resume: wrap the `disable_irq(client->irq)` / `enable_irq(client->irq)` calls (~:1444 / ~:1488) with `if (client->irq > 0)`.

- [ ] **Step 3.2: Compile-test:**

```bash
wsl -d Ubuntu-22.04 -e bash -lc 'cd /home/caner/projects/rzv2n/kbuild-cip43 && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) drivers/input/touchscreen/goodix.o'
```
Expected: clean compile.

- [ ] **Step 3.3: Export patch 0004** (flow as 1.6; message `input: goodix - fall back to polling when no IRQ is wired`), copy to the worktree patch dir, stage, then `git commit -q -m "feat(linux): goodix polled-mode fallback for IRQ-less GT911 wiring"`

---

### Task 4: `display.cfg` fragment + bbappend wiring

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/linux/linux-renesas/display.cfg`
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-renesas_%.bbappend`

- [ ] **Step 4.1: Write `display.cfg`** (mirror the tas2563-audio.cfg header comment style — read it first):

```
# Display + touch for the E1M-X carrier RK055HDMIPI4MA0 panel path
# (himax HX8394-F over the V2N DSI, GT911 touch, GPT PWM backlight,
#  GD32-bridge GPIO expander for the panel/touch reset sideband).
CONFIG_DRM_PANEL_HIMAX_HX8394=y
CONFIG_BACKLIGHT_CLASS_DEVICE=y
CONFIG_BACKLIGHT_PWM=y
CONFIG_GPIO_GD32_BRIDGE=y
CONFIG_CRC_ITU_T=y
CONFIG_INPUT_TOUCHSCREEN=y
CONFIG_TOUCHSCREEN_GOODIX=y
```

- [ ] **Step 4.2: Wire into the bbappend.** In `linux-renesas_%.bbappend`:
- Add to the existing `SRC_URI:append` block (lines ~46-53), after the 0001 patch line:
```
    file://0002-drm-panel-add-himax-hx8394-with-rocktech-rk055hdmipi.patch \
    file://0003-gpio-add-gd32-bridge-expander-driver.patch \
    file://0004-input-goodix-fall-back-to-polling-without-an-irq.patch \
```
- Add per-machine cfg lines next to the tas2563 ones (~:82-83):
```
SRC_URI:append:e1m-v2n101 = " file://display.cfg"
SRC_URI:append:e1m-v2m101 = " file://display.cfg"
```

- [ ] **Step 4.3: Sanity-parse.** No live bitbake tree exists; the gate is textual: `grep -n "display.cfg\|0002-\|0003-\|0004-" meta-alp-sdk/recipes-kernel/linux/linux-renesas_%.bbappend` shows all five additions, and the three patch files + cfg exist in the layer dir.

- [ ] **Step 4.4: Commit:** stage `meta-alp-sdk/recipes-kernel/linux/linux-renesas/display.cfg` + the bbappend, then `git commit -q -m "feat(yocto): wire display patches + Kconfig fragment for v2n101/v2m101"`

---

### Task 5: Device tree — SoM dtsi (bridge gpiochip, GPT backlight)

**Files:**
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`

These are on-module facts so they live in the SoM dtsi. Read the file before editing; match its comment style (it carries STATUS/VERIFY prose).

- [ ] **Step 5.1: Add the GD32 gpiochip under `&i2c8`** (the carrier dtsi enables `&i2c8` but the GD32 is on-module — add the child node from the SoM dtsi; node ref blocks merge across dtsi files):

```dts
/* GD32G553 supervisor MCU bridge -- I2C slave transport on BRD_I2C.
 * Exposes the GD32-routed E1M IO pads (logical bridge map, 18 lines)
 * as a Linux gpiochip: line 5 = IO13/LCD1_RST, line 3 = IO11/CTP1_RST,
 * line 1 = IO9/CTP1_INT (input only -- the GT911 drives it).
 * Same physical chip as the CM33's SCI7 SPI link peer; Linux only
 * issues short GPIO frames here.
 */
&i2c8 {
	gd32_gpio: gpio@70 {
		compatible = "alplab,gd32-bridge-gpio";
		reg = <0x70>;
		gpio-controller;
		#gpio-cells = <2>;
	};
};
```

- [ ] **Step 5.2: Add the backlight PWM (PA5 → GTIOC10B → gpt1_2) + pwm-backlight:**

In the SoM dtsi `&pinctrl` block:
```dts
	backlight_pwm_pins: backlight-pwm {
		pinmux = <RZV2N_PORT_PINMUX(A, 5, 9)>; /* GTIOC10B = BL_PWM -> on-SoM KTD2801 boost */
	};
```
At file scope:
```dts
/* Display backlight: the LED boost driver lives ON the SoM (BL_LED_A/K
 * to the module edge); Renesas PA5 = BL_PWM drives its CNTRL input.
 * 5 kHz PWM (KTD2801 ExpressWire-free PWM-dimming range).
 */
&gpt1_2 {
	pinctrl-0 = <&backlight_pwm_pins>;
	pinctrl-names = "default";
	channel = "channel_B";
	status = "okay";
};

/ {
	backlight: backlight {
		compatible = "pwm-backlight";
		pwms = <&gpt1_2 0 200000>;
		power-supply = <&reg_3p3v>;
		brightness-levels = <0 4 8 16 32 64 128 255>;
		default-brightness-level = <6>;
	};
};
```
(`reg_3p3v` already exists in this dtsi — verify the exact label with `grep -n reg_3p3v meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi` and use what's there.)

- [ ] **Step 5.3: Build the dtb** (the dtsi files install into the kernel tree; replicate that manually for the compile gate):

```bash
wsl -d Ubuntu-22.04 -e bash -lc 'cp "/mnt/e/GitHub/alp-sdk/.claude/worktrees/v2n-lcd-display1/meta-alp-sdk/recipes-kernel/linux/linux-renesas/"*.dts* /home/caner/projects/rzv2n/kbuild-cip43/arch/arm64/boot/dts/renesas/ && \
  cd /home/caner/projects/rzv2n/kbuild-cip43 && \
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- renesas/e1m-v2n101-x-evk.dtb renesas/e1m-v2m101-x-evk.dtb'
```
Expected: both dtbs build with no warnings about the new nodes.

- [ ] **Step 5.4: Commit:** stage `e1m-v2n-som.dtsi`, then `git commit -q -m "feat(dts): SoM gd32-bridge gpiochip + GPT1_2/PA5 pwm-backlight"`

---

### Task 6: Device tree — carrier dtsi (DSI pipeline, panel, touch)

**Files:**
- Modify: `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi`

- [ ] **Step 6.1: Enable the display pipeline + panel.** Add (and update the header TODO list — remove the "2x MIPI-DSI displays + capacitive touch" line, replacing with a pointer to the new section):

```dts
/* Display 1 (J6): RK055HDMIPI4MA0 720x1280 MIPI-DSI panel.
 * The V2N's single DSI feeds the connector DSI0 lane set -> J6 directly
 * (no bridge/mux on this carrier). Display 2 (J28) sits on the DSI1
 * lane set, which the V2N family does not populate -- permanently
 * unavailable on V2N/V2M.
 * Panel rails (+5V/+3V3/+L1_VIO) are always-on; LCD1_PWR_EN (IO15) is
 * pulled high on the carrier and IO15 is unrouted on V2N -- no power
 * GPIO needed. Panel reset = E1M IO13 -> GD32 PF1 = bridge gpio line 5.
 */
&du {
	status = "okay";
};

&dsi0 {
	status = "okay";

	panel@0 {
		compatible = "rocktech,rk055hdmipi4ma0";
		reg = <0>;
		reset-gpios = <&gd32_gpio 5 GPIO_ACTIVE_LOW>;
		vcc-supply = <&reg_3p3v>;
		iovcc-supply = <&reg_3p3v>;
		backlight = <&backlight>;

		port {
			panel_in: endpoint {
				remote-endpoint = <&dsi0_out>;
			};
		};
	};

	ports {
		port@1 {
			dsi0_out: endpoint {
				remote-endpoint = <&panel_in>;
				data-lanes = <1 2>;
			};
		};
	};
};
```
Notes: `reg_3p3v` is the SoM dtsi fixed rail (always-on; matches the carrier's strap-selected +L1_VIO at the 3V3 position — if the bench strap is 1V8, switch `iovcc-supply` to `&reg_1p8v`). `GPIO_ACTIVE_LOW` because the carrier net is `LCD1_RST_L` and the hx8394 driver asserts reset by driving the GPIO logical-active.

- [ ] **Step 6.2: Enable RIIC2 + GT911.** Add to the carrier `&pinctrl` block:

```dts
	i2c2_pins: i2c2 {
		pinmux = <RZV2N_PORT_PINMUX(3, 4, 1)>, /* RIIC2 SDA2 -> E1M I2C2 (DSI/CSI ctrl bank 0) */
			 <RZV2N_PORT_PINMUX(3, 5, 1)>; /* RIIC2 SCL2 */
	};
```
and at file scope:
```dts
/* E1M I2C2 = display/camera control bank 0 -> Display-1 touch (GT911).
 * GT911 strap: I2C address depends on the INT level at reset release;
 * INT terminates at GD32 PA7 (pull-up by default) so the panel may
 * enumerate at 0x14 instead of 0x5d -- adjust reg after the bench
 * i2cdetect (see the port plan, Task 10). No interrupts property:
 * INT is not Linux-visible; the goodix driver polls (~60 Hz).
 */
&i2c2 {
	pinctrl-0 = <&i2c2_pins>;
	pinctrl-names = "default";
	clock-frequency = <400000>;
	status = "okay";

	touchscreen@5d {
		compatible = "goodix,gt911";
		reg = <0x5d>;
		reset-gpios = <&gd32_gpio 3 GPIO_ACTIVE_LOW>; /* CTP1_RST = IO11 -> GD32 PB0 */
		touchscreen-size-x = <720>;
		touchscreen-size-y = <1280>;
	};
};
```

- [ ] **Step 6.3: Build both dtbs** (same command as Step 5.3). Expected: clean; specifically no `Warning (graph_endpoint)` on the dsi0/panel OF-graph.

- [ ] **Step 6.4: Boot-smoke the V2N dtb in QEMU? No** — there is no QEMU model for this SoC; the compile + OF-graph check is the pre-bench gate. Verify decompile looks right:
```bash
wsl -d Ubuntu-22.04 -e bash -lc 'cd /home/caner/projects/rzv2n/kbuild-cip43 && dtc -I dtb -O dts arch/arm64/boot/dts/renesas/e1m-v2n101-x-evk.dtb 2>/dev/null | grep -A4 "panel@0\|touchscreen@5d\|gpio@70" | head -40'
```
Expected: the three nodes appear with the properties above.

- [ ] **Step 6.5: Commit:** stage `e1m-x-evk.dtsi`, then `git commit -q -m "feat(dts): X-EVK Display 1 -- dsi0/du + rk055 panel + RIIC2 GT911 (polled)"`

---

### Task 7: Image — Weston + DRM test tools

**Files:**
- Modify: `meta-alp-sdk/recipes-images/alp-image-edge.bb`

- [ ] **Step 7.1: Append graphics userspace.** After the existing `IMAGE_INSTALL` block add:

```
# Display stack (X-EVK MIPI-DSI panel): Weston on Mali/Wayland
# (wayland+opengl come from the rz-vlp distro), plus modetest for
# bench bring-up. libdrm-tests is also in rz-vlp's tools group;
# listed explicitly so rz-bsp builds get it too.
IMAGE_INSTALL += " \
    weston \
    weston-init \
    libdrm \
    libdrm-tests \
"
```

- [ ] **Step 7.2: Commit:** stage the recipe, then `git commit -q -m "feat(image): weston + libdrm-tests in alp-image-edge for the display stack"`

---

### Task 8: Example — `examples/display/lvgl-dashboard-x-evk` + recipe

**Files:**
- Create: `examples/display/lvgl-dashboard-x-evk/src/main.c`
- Create: `examples/display/lvgl-dashboard-x-evk/CMakeLists.txt`
- Create: `examples/display/lvgl-dashboard-x-evk/board.yaml`
- Create: `examples/display/lvgl-dashboard-x-evk/README.md`
- Create: `meta-alp-sdk/recipes-examples/alp-lvgl-dashboard/alp-lvgl-dashboard_0.6.bb`

This is the first Linux-side display example — examples are documentation (teaching-grade comments, ~50% comment ratio). It uses the meta-oe packaged LVGL 9.1 (DRM + evdev backends on), NOT a vendored copy.

- [ ] **Step 8.1: Read two patterns first:** `examples/display/lvgl-widgets-demo/` (comment style, board.yaml shape) and `meta-alp-sdk/recipes-examples/alp-edgeai/alp-edgeai_0.6.bb` (recipe shape). Follow both.

- [ ] **Step 8.2: Write `src/main.c`:**

```c
/*
 * lvgl-dashboard-x-evk -- LVGL 9 dashboard on the X-EVK MIPI-DSI panel.
 *
 * Teaching points:
 *  - On the V2N family the display pipeline is plain Linux DRM/KMS
 *    (rz-du driver) -- there is no alp_display_* indirection on Linux;
 *    apps talk to the platform display stack directly.
 *  - LVGL's Linux DRM backend scans out fullscreen on the panel;
 *    the GT911 touch arrives as a standard evdev pointer.
 *  - Build: linked against the distro's packaged LVGL 9.1
 *    (PACKAGECONFIG drm -> LV_USE_LINUX_DRM, LV_USE_EVDEV).
 *
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <lvgl/lvgl.h>
#include <lvgl/src/drivers/display/drm/lv_linux_drm.h>
#include <lvgl/src/drivers/evdev/lv_evdev.h>

/* The DRM card for the rz-du pipeline; card0 on a stock image. */
#define DRM_CARD     "/dev/dri/card0"
/* GT911 registers as a standard input device; event0 on a stock image
 * (check `libinput list-devices` if you have more input hardware). */
#define TOUCH_EVDEV  "/dev/input/event0"

static void make_dashboard(void)
{
	/* A minimal three-widget dashboard: title, arc gauge, button. */
	lv_obj_t *scr = lv_screen_active();

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "ALP SDK -- E1M-X V2N");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

	lv_obj_t *arc = lv_arc_create(scr);
	lv_obj_set_size(arc, 360, 360);
	lv_arc_set_value(arc, 62);
	lv_obj_center(arc);

	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -48);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, "Touch me");
}

int main(void)
{
	lv_init();

	/* Display: LVGL opens the DRM device and picks the preferred
	 * mode -- 720x1280 from the panel's EDID-less DT mode. */
	lv_display_t *disp = lv_linux_drm_create();
	lv_linux_drm_set_file(disp, DRM_CARD, -1);

	/* Touch: evdev pointer (GT911, polled by the kernel driver). */
	lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, TOUCH_EVDEV);
	lv_indev_set_display(touch, disp);

	make_dashboard();

	/* LVGL owns the loop: tick + timer handler forever. */
	while (1) {
		uint32_t idle_ms = lv_timer_handler();
		usleep(idle_ms * 1000);
	}
	return 0;
}
```

- [ ] **Step 8.3: Write `CMakeLists.txt`:**

```cmake
cmake_minimum_required(VERSION 3.20)
project(alp-lvgl-dashboard C)

# Linux-only example: links the distro's shared LVGL 9 (DRM backend).
add_executable(alp-lvgl-dashboard src/main.c)
find_library(LVGL_LIB lvgl REQUIRED)
target_link_libraries(alp-lvgl-dashboard ${LVGL_LIB})
install(TARGETS alp-lvgl-dashboard RUNTIME DESTINATION bin)
```

- [ ] **Step 8.4: Write `board.yaml`** (shape mirrors `examples/display/lvgl-widgets-demo/board.yaml`; first Linux-core display example — no `chips:`, no `libraries:` knob since LVGL comes from the distro package, not the Zephyr library loader):

```yaml
# board.yaml -- LVGL dashboard on the E1M-X V2N MIPI-DSI panel.
#
# First Linux-side display example: the RK055HDMIPI4MA0 panel is a
# PLATFORM device (DRM/KMS via the carrier device tree), not an SDK
# chip driver -- so there is no `chips:` entry.  LVGL 9 comes from
# the Yocto distro (meta-oe `lvgl` package, DRM backend), not the
# Zephyr `libraries:` loader.  Customer workflow: copy this dir,
# build the alp-image-edge image (the alp-lvgl-dashboard recipe
# packages this app), boot, run `alp-lvgl-dashboard`.
som:
  sku: E1M-V2N101            # any V2x SKU works -- same PCB, same panel path

preset: e1m-x-evk

cores:
  a55_cluster:
    app: ./src               # Linux userspace app (DRM/KMS + evdev)
  m33_sm:
    os: "off"                # display is wholly A55-owned

diagnostics:
  log_level: info
```
Gate: `py -3.14 scripts/check_example_portability.py` parses clean (and flag anything it rejects about the first a55-app example to the maintainer rather than inventing schema fields — `project_pending_hw_configs` rule).

- [ ] **Step 8.5: Write `README.md`** covering BOTH consumer paths (Yocto recipe install via `alp-lvgl-dashboard` package, and standalone cross-build against the SDK sysroot), what to expect on screen, and the touch device note.

- [ ] **Step 8.6: Write the recipe** `meta-alp-sdk/recipes-examples/alp-lvgl-dashboard/alp-lvgl-dashboard_0.6.bb` (exact pattern of alp-edgeai_0.6.bb):

```
SUMMARY = "ALP SDK LVGL dashboard example for the E1M-X MIPI-DSI panel"
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${WORKDIR}/git/LICENSE;md5=3b83ef96387f14655fc854ddc3c6bd57"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
PV = "0.6.0"

S = "${WORKDIR}/git/examples/display/lvgl-dashboard-x-evk"

inherit cmake
DEPENDS = "lvgl libdrm"
RDEPENDS:${PN} = "lvgl"

FILES:${PN} = "${bindir}/alp-lvgl-dashboard"
```
(Verify the LICENSE md5 against the repo's actual LICENSE checksum used in alp-edgeai_0.6.bb — copy that line verbatim.)

- [ ] **Step 8.7: Host-side syntax gate** (no cross toolchain needed): `cmake -S examples/display/lvgl-dashboard-x-evk -B /tmp/lvgl-dash-build` is expected to FAIL at `find_library(LVGL…)` on the host — that's fine; the gate is that CMake parses the file (error is about the missing lib, not syntax). C syntax gate: `gcc -fsyntax-only -I/usr/include examples/display/lvgl-dashboard-x-evk/src/main.c` will fail on missing lvgl headers — acceptable; real compile happens in the Yocto bake (Task 10 prerequisite) — note this in the commit message.

- [ ] **Step 8.8: Commit:** stage the example dir + recipe, then `git commit -q -m "feat(example): lvgl-dashboard-x-evk -- first Linux/DRM display example + Yocto recipe"`

---

### Task 9: Docs + metadata truth-up

**Files:**
- Modify: `docs/boards/e1m-x-evk.md`
- Modify: `metadata/boards/e1m-x-evk.yaml`
- Modify: `docs/build-yocto-v2n.md`
- Modify: `docs/os-support-matrix.md` (comment only — row flips in Task 10)
- Modify: `CHANGELOG.md`

- [ ] **Step 9.1: `docs/boards/e1m-x-evk.md`:** remove the IT6162 + PI3WVR648 rows/claims (lines ~43-44 and the ~88 pending-roles bullet) — they are NOT populated on the carrier; replace the display section with: both Display FFCs wired direct (ESD + LSF0102 level shifters only); Display 1 = DSI0 = the V2N display path (RK055HDMIPI4MA0); Display 2 = DSI1 = unavailable on V2N/V2M; sideband map (LCD1_RST=IO13, LCD1_PWR_EN=IO15 pulled-up/unrouted-on-V2N, CTP1 INT/RST=IO9/IO11, touch bank=E1M I2C2); backlight = module BL_LED_A/K from the SoM's PA5-driven boost; the `#AC0` DCS silicon note with datasheet reference. Keep schematic-sheet-level detail out (no net-by-net dumps beyond what's needed).

- [ ] **Step 9.2: `metadata/boards/e1m-x-evk.yaml`:** add a comment block to the display gpio section recording: Display-2 sideband + DSI1 lanes are dead on V2N-family SoMs (single SoC DSI feeds DSI0/Display 1); IO15/LCD1_PWR_EN is carrier-pulled-up so V2N (which doesn't route IO15) still powers the panel. No schema/field changes.

- [ ] **Step 9.3: `docs/build-yocto-v2n.md`:** fix the stale §3 dtb name (`renesas/r9a09g056n48-rzv2n-evk.dtb` → `renesas/e1m-v2n101-x-evk.dtb`), and add the kernel-version pin warning (`PREFERRED_VERSION_linux-renesas = "6.1%"` against the VLP-v5 template's 6.12 default). Mention `display.cfg` joins tas2563 in the per-machine fragment list.

- [ ] **Step 9.4: `docs/os-support-matrix.md`:** leave the V2N Display row as `stub` but add the footnote "display bring-up in progress on feat/v2n-lcd-display1; flips on HIL pass".

- [ ] **Step 9.5: `CHANGELOG.md`:** add an Unreleased entry: "E1M-X Display 1 (RK055HDMIPI4MA0) Linux bring-up: hx8394 panel backport + rocktech desc, gpio-gd32-bridge expander, goodix polled fallback, GPT/PA5 pwm-backlight, GT911 on RIIC2, weston image, LVGL example."

- [ ] **Step 9.6: Run the doc gates** (from the worktree root): `py -3.14 scripts/check_doc_drift.py` — clean; then the updating-docs skill's remaining checklist if it flags more surfaces. Expected: no drift findings against the edited docs.

- [ ] **Step 9.7: Commit:** stage all five files, then `git commit -q -m "docs(x-evk): display ground truth -- no bridge chips, Display-2 dead on V2N, build doc fixes"`

---

### Task 10: HIL bench validation ladder (gates G0–G8)

**Files:**
- Modify: `docs/superpowers/plans/2026-06-04-v2n-lcd-display1.md` (bench log)
- Modify (on pass): `docs/os-support-matrix.md`

Pre-requisites: Task 0 done (G0 = bridge I2C transport PASS), Tasks 1–6 built. Deploy = `Image` + `e1m-v2n101-x-evk.dtb` from kbuild-cip43 over SSH (`ssh root@192.168.1.198 "cat > /boot/Image" < arch/arm64/boot/Image`, same for the dtb; detached reboot per the bench convention — reboot must be its own ssh call). Coordinate COM24/board access with the GD32-bridge session (serial MCP is single-holder).

- [ ] **G1 — boot + probe:** dmesg shows `rzg2l-du`/`rzg2l-mipi-dsi` bind, `GD32 bridge protocol 0.6.x` from gpio-gd32-bridge, panel probe. **The first DCS write of the init sequence is the silicon DCS gate** — success ⇒ new-rev silicon confirmed working; failure with DSI transfer errors ⇒ record + check chip marking (#AC0?).
- [ ] **G2 — pixels:** fbcon shows on the panel (CONFIG_DRM_FBDEV_EMULATION=y) and/or `modetest -M rzg2l-du -s <conn>@<crtc>:720x1280` pattern fills. Record connector/crtc ids from `modetest -M rzg2l-du`.
- [ ] **G3 — touch enumerate:** `i2cdetect -y <riic2-busnum>` (find busnum: `ls /sys/bus/i2c/devices/ | grep 14400c00` style or `i2cdetect -l`). Expect GT911 ACK at **0x5d or 0x14** (strap via GD32 PA7 pull-up makes 0x14 likely). If 0x14: change the carrier dtsi node to `touchscreen@14` / `reg = <0x14>`, rebuild dtb, redeploy, commit the correction. This also empirically settles the spec's O1 (touch on RIIC2 at all) — if NO ACK on RIIC2: probe i2c3-capable buses, then escalate to the carrier owner (touch dead on V2N until carrier decision; display unaffected).
- [ ] **G4 — backlight:** `echo 4 > /sys/class/backlight/backlight/brightness` (and 0/7 sweep) visibly dims/brightens.
- [ ] **G5 — touch events:** `evtest /dev/input/event0` streams coordinates while touching; latency subjectively OK at 60 Hz poll.
- [ ] **G6 — DPMS cycle:** `modetest` DPMS off/on (or `echo 1 > /sys/class/graphics/fb0/blank; echo 0 > …`) — panel re-inits cleanly through the bridge reset path (watch dmesg for hx8394 re-prepare).
- [ ] **G7 — desktop + example:** full `alp-image-edge` bake (WSL, `MACHINE=e1m-v2n101-a55 bitbake alp-image-edge` with `PREFERRED_VERSION_linux-renesas = "6.1%"` forced; rebuild build dir from the VLP templates per docs/build-yocto-v2n.md), flash, Weston starts on the panel, `alp-lvgl-dashboard` runs fullscreen with working touch.
- [ ] **G8 — soak + coexistence:** 1 h idle + periodic modetest/touch; THEN re-run the GD32 HIL soak example (coordinate with the bridge session) while the display is live — BRD_I2C GPIO frames must not disturb the CM33↔GD32 SCI7 link soak. dmesg clean of DSI/I2C errors.
- [ ] **Record everything** in the bench log section; on full pass flip `docs/os-support-matrix.md` V2N Yocto Display → GA-pending-HIL → per repo convention, and commit: `git commit -q -m "test(hil): Display-1 bench ladder G1-G8 results -- <PASS/notes>"`
- [ ] **V2M101 smoke** (when a new-silicon V2M board is available): repeat G1–G5 — expected identical (same PCB).

---

### Task 11: Local CI + finish

- [ ] **Step 11.1:** Run the running-local-ci skill (full local gates: clang-format diff-only, twister native_sim scope unchanged by this work but run the required contexts, pytest scripts, doc gates) — all green before any push.
- [ ] **Step 11.2:** Run the reviewing-alp-changes skill against the branch diff (portable-API conventions, no-legacy-compat, attribution checks).
- [ ] **Step 11.3:** Use the finishing-a-development-branch skill: merge `feat/v2n-lcd-display1` → `dev` (the untested-integration branch; HIL-passed work may then ride a later dev→main PR). Coordinate the merge timing with the GD32 session (shared dev).

---

## Self-review (done at authoring)

- Spec coverage: §4.1→Task 1, §4.2→Task 2, §4.3→Task 3, §4.4→Task 4, §4.5→Tasks 5-6, §4.6→Task 7, §4.7→Task 8, §4.8→Task 9, §6→Tasks 0+10, §7 O1→G3, O2→resolved (GTIOC10B/gpt1_2 func 9), O3→resolved (compiles as-is; two named verify flags in Step 1.1/1.2), O4→Task 0 + grounded pad map (PF1/PB0/PA7 free, defaults input+pull-up), O5→out of scope (HW owner).
- Known deliberate deviations from spec: backlight PWM period/levels chosen here (5 kHz, 8 levels) — not in spec, implementation detail; spec's "weston via meta-rz-graphics" realized as IMAGE_INSTALL append because wayland/opengl are already distro features (grounded finding).
- Placeholder scan: the two Step-1 verification flags are explicit named checks with commands and decision rules, not TBDs. GT911 address is dual-pathed with an exact bench decision step.
- Type consistency: `gd32_gpio` label used in Tasks 5/6 matches; line indices 5/3/1 match the grounded pad map; `backlight` label matches; `&gpt1_2`+`channel_B`+func 9 consistent across Task 5.

## Bench log

(appended during Tasks 0 and 10)
