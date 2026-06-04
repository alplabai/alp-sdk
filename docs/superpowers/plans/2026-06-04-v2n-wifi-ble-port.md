# V2N/V2M Wi-Fi + BLE Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the Murata LBEE5HY2FY-922 (Infineon CYW55513) Wi-Fi 6 + BT 5.4 module up on V2N/V2M: `wlan0` (SDIO/brcmfmac backports) + `hci0` (HCI UART/BlueZ) on Linux/A55, GD32-owned REG_ON power path, validated on silicon.

**Architecture:** GD32 bridge gains pad-map bits 18/19 (BT/WL_REG_ON on PE14/PE15, boot default-on). Linux gets a kernel config fragment (kernel cfg80211 OUT, BT stack IN), DT patches 0014/0015 (SDHI1→WLAN on the dedicated SD1 pins; RSCI BT UART), the Murata `cyw-fmac` backports kmod, and murata-wireless firmware blobs. Bench-proof first (hand-built artifacts over ssh), then productionize (Yocto recipes in meta-alp-sdk + patch series), then V2M.

**Tech Stack:** GD32 firmware (gd32 + stub HALs), Zephyr twister (native_sim) for host-side smokes, WSL kernel build (`kbuild-cip43`, 6.1.141), backports/cyw-fmac, dtc, Yocto/kas, bench over COM24 + ssh (`root@192.168.1.198`) + J-Link.

**Spec:** `docs/superpowers/specs/2026-06-04-v2n-wifi-ble-port-design.md`

**Cross-session coordination (read first):** another session is concurrently editing `firmware/gd32-bridge/` (LCD sideband work; `bridge_hw_gd32.c`, `bridge_hw.h`, `docs/gd32-bridge-protocol.md`, `firmware-version.txt` are dirty in the main tree, fw now v0.2.3, protocol may move past 0.6.0). ALL work happens in a separate worktree branched from **latest** `dev`; before flashing the GD32 or merging, re-check `git -C E:\GitHub\alp-sdk log dev -3` and renumber the protocol bump / re-sequence J-Link use if needed.

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `firmware/gd32-bridge/hal/bridge_hw_gd32.c` | modify | pad-map +2 rows, boot REG_ON sequence |
| `firmware/gd32-bridge/hal/bridge_hw_stub.c` | modify | stub parity for bits 18/19 |
| `firmware/gd32-bridge/src/protocol.h` | modify | `PROTOCOL_VERSION_MINOR` bump |
| `docs/gd32-bridge-protocol.md` | modify | §3.1 REG_ON bit rows + boot-default note |
| `include/alp/chips/gd32g553.h` | modify | REG_ON bit/mask constants (Doxygen) |
| `include/alp/chips/murata_lbee5hy2fy.h` | modify | wiring table: UART instance, LPO, mask bits; drop `[UNTESTED]` after HIL |
| `tests/zephyr/chips/src/main.c` | modify | mask-constant + callback-wiring smoke |
| `metadata/chips/murata_lbee5hy2fy.yaml` | modify | UART/LPO facts, `verification.hil_silicon` |
| `CHANGELOG.md` | modify | feature entry |
| `docs/bring-up-v2n.md`, `docs/soms/v2n.md` | modify | Wi-Fi/BT bring-up + validation section |
| WSL `…/meta-renesas …/linux/files/0014-*.patch`, `0015-*.patch` | create | DT: SDHI1→WLAN, BT UART (staged via `V2N-Yocto/dtbwork/staging/linux-renesas_%.bbappend`) |
| `V2N-Yocto/dtbwork/staging/linux-renesas_%.bbappend` | modify | += 0014, 0015, `wifi-bt.cfg` |
| `V2N-Yocto/dtbwork/staging/wifi-bt.cfg` | create | kernel config fragment |
| `meta-alp-sdk/recipes-kernel/cyw-fmac/cyw-fmac_git.bb` | create | backports kmod recipe |
| `meta-alp-sdk/recipes-bsp/cyw-fmac-firmware/cyw-fmac-firmware_git.bb` | create | WLAN fw + 2FY NVRAM + CLM + BT `.hcd` |
| `meta-alp-sdk/conf/machine/e1m-v2n101-a55.conf` (+v2n102/v2m101/v2m102) | modify | pull modules/fw/userland into images |

Branch: `feat/v2n-wifi-ble` worktree. Bench artifacts (`.ko`, dtb, blobs) are deployed over ssh from WSL/Windows; nothing hand-deployed is left undocumented — Phase 2 reproduces all of it from recipes.

---

## Phase 0 — setup

### Task 1: Worktree + branch

**Files:** none (infrastructure)

- [ ] **Step 1:** REQUIRED SUB-SKILL `superpowers:using-git-worktrees`: create worktree `feat/v2n-wifi-ble` from **latest** `dev` (e.g. `E:\GitHub\alp-sdk-worktrees\v2n-wifi-ble`). Verify: `git -C <worktree> log --oneline -1` shows the current dev tip (≥ `47209d3`).
- [ ] **Step 2:** Confirm the spec + this plan exist on the branch (they were committed to dev): `git -C <worktree> log --oneline -3 -- docs/superpowers` shows the spec commit.

### Task 2: Pin ground truth (SDHI1 pins, BT UART instance)

Resolves spec §9 rows 2–3. No code; produces facts used by Tasks 7/8 and 13.

**Files:** read-only: WSL `~/projects/rzv2n/repos/rz_linux-cip`, `E:\GitHub\V2N-Yocto\dtbwork\cur.dts`

- [ ] **Step 1: SD1 pin names.** The EVK dts proves SDHI1 uses dedicated pinctrl pins, not GPIO mux:
  `cur.dts:307-326`: group `sd1` = `sd1-cd { pinmux = <0xe004c>; }` + `sd1-clk { pins = "SD1CLK"; }` + `sd1-dat-cmd { pins = "SD1DAT0…3","SD1CMD"; }`.
  Confirm the pinctrl driver knows these names:
  ```bash
  wsl -d Ubuntu-22.04 -e bash -lc 'grep -rn "SD1CLK\|SD1DAT0" /home/caner/projects/rzv2n/repos/rz_linux-cip/drivers/pinctrl/renesas/ | head'
  ```
  Expected: hits in the RZ/V2H(P)-family pinctrl data (`pinctrl-rzg2l.c` or `pinctrl-rzv2h.c`) listing `SD1CLK`, `SD1DAT0..3`, `SD1CMD` as dedicated pins.
- [ ] **Step 2: cross-check ball naming.** Confirm the schematic's `PB0–PB5` are the SD1CLK/CMD/DATx package balls: in the R9A09G056 HW manual pin-function table (internal docs) or the SoC pin chart in `rz_linux-cip` Documentation/bindings, find SD1CLK↔ball. Record the mapping sentence for Task 13's metadata note. If the manual contradicts (SD1 ≠ PB0–PB5 balls), STOP and re-check the schematic nets before touching the DT (the metadata + schematic agreed, so this is confirmation, not discovery).
- [ ] **Step 3: BT UART instance.** Find which `serial@…` muxes P40–P43:
  ```bash
  wsl -d Ubuntu-22.04 -e bash -lc 'grep -rn "P40\|RSCI" /home/caner/projects/rzv2n/repos/rz_linux-cip/arch/arm64/boot/dts/renesas/r9a09g056*.dtsi | head -30'
  # then decode: which sciN/rsciN node's pinmux group covers port 4 pins 0-3
  ```
  Also check the one flow-control-ready node already in the EVK dts (`patched.dts:1166 uart-has-rtscts`, alias `serial1` = `/soc/serial@12800c00`) — decode its `pinctrl-0` pinmux values (RZG2L encoding `((port*8+pin) | func<<16)`) to see if it IS the P40–P43 instance. Record: node path, label, pinmux encodings for P40–P43 with the UART function number.
  Expected outcome: one concrete `serial@128xxxxx` node + its 4-pin pinmux group. Record alongside Step 2's facts.
- [ ] **Step 4:** Write the three facts (SD1 pin set, ball cross-check, BT UART node + pinmux) into the worktree as `docs/superpowers/plans/2026-06-04-v2n-wifi-ble-port-facts.md`; commit:
  ```bash
  git add docs/superpowers/plans/2026-06-04-v2n-wifi-ble-port-facts.md && git commit -q -m "docs(v2n-wifi): record SDHI1/BT-UART pin ground truth"
  ```

---

## Phase 1 — GD32 power path

### Task 3: Bridge firmware — REG_ON pad-map bits 18/19 + boot default-on

REQUIRED SUB-SKILL: `extending-the-gd32-bridge-protocol` (covers version-bump etiquette, doc table, host-driver lockstep).

**Files:**
- Modify: `firmware/gd32-bridge/hal/bridge_hw_gd32.c` (pad map ends ~`:182`, boot loop ~`:685`, write ~`:876` — anchor on symbols, lines drift due to the LCD session)
- Modify: `firmware/gd32-bridge/hal/bridge_hw_stub.c`
- Modify: `firmware/gd32-bridge/src/protocol.h`
- Modify: `docs/gd32-bridge-protocol.md`

- [ ] **Step 1:** In `bridge_hw_gd32.c`, append to `gpio_pad_map[]` (after the `{ GPIOD, GPIO_PIN_1 },  /* bit 17 = E1M IO35 */` row):

```c
    /* Murata LBEE5HY2FY-922 sideband (NOT E1M pads -- module power
     * enables; see metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv rows
     * BT_REG_ON / WL_REG_ON and docs/gd32-bridge-protocol.md §3.1).
     * Boot-time these two are driven OUTPUT (default HIGH) instead of
     * the INPUT + PULL_UP rule -- the module has internal 50 k
     * pull-downs, so an input pad would leave its power state
     * indeterminate. */
    { GPIOE, GPIO_PIN_14 }, /* bit 18 = BT_REG_ON */
    { GPIOE, GPIO_PIN_15 }, /* bit 19 = WL_REG_ON */
```

And below `#define GPIO_PAD_MAP_COUNT`:

```c
#define GPIO_PAD_BT_REG_ON 18u
#define GPIO_PAD_WL_REG_ON 19u
_Static_assert(GPIO_PAD_MAP_COUNT == 20u,
               "REG_ON bit indices are wired into hosts (gd32g553.h) and "
               "docs/gd32-bridge-protocol.md §3.1 -- renumber all three together");
```

- [ ] **Step 2:** In `bridge_hw_init()`, change the pad-init loop to skip the REG_ON pads, then power the module:

```c
    for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
        if (i == GPIO_PAD_BT_REG_ON || i == GPIO_PAD_WL_REG_ON) continue;
        gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP,
                      gpio_pad_map[i].pin);
    }

    /* Wi-Fi/BT module power-on (Type 2FY power-on sequence): hold both
     * REG_ON lines LOW for a clean edge (they float vs the module's
     * internal 50 k pull-downs out of GD32 reset), give VBAT/VDDIO/LPO
     * a settle window, then enable.  ~10 ms spin at 240 MHz; precision
     * is irrelevant, only ">= a few ms" matters. */
    for (size_t i = GPIO_PAD_BT_REG_ON; i <= GPIO_PAD_WL_REG_ON; ++i) {
        gpio_bit_reset(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
        gpio_output_options_set(gpio_pad_map[i].periph, GPIO_OTYPE_PP,
                                GPIO_OSPEED_12MHZ, gpio_pad_map[i].pin);
        gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                      gpio_pad_map[i].pin);
        gpio_is_output[i] = true;
    }
    for (volatile uint32_t spin = 0; spin < 800000u; ++spin) { __NOP(); }
    gpio_bit_set(gpio_pad_map[GPIO_PAD_BT_REG_ON].periph,
                 gpio_pad_map[GPIO_PAD_BT_REG_ON].pin);
    gpio_bit_set(gpio_pad_map[GPIO_PAD_WL_REG_ON].periph,
                 gpio_pad_map[GPIO_PAD_WL_REG_ON].pin);
```

(GPIOE RCU clock is already enabled — bits 14–16 of the existing map live on GPIOD/GPIOE. Verify by reading the RCU enable block in `bridge_hw_init` before assuming; if GPIOE is missing there, add it.)

- [ ] **Step 3:** Mirror in `bridge_hw_stub.c`: read the file; extend its pad count to 20 and make its shadow state boot with bits 18/19 = output-high so host-side tests see default-on semantics. Keep the stub's existing modeling style.
- [ ] **Step 4:** `protocol.h`: bump `PROTOCOL_VERSION_MINOR` by one over whatever the branch-point value is (0.6.0 → 0.7.0 as of writing; if the LCD-session merge already took 0.7, take 0.8 — the `_Static_assert` comment and doc rows move with it).
- [ ] **Step 5:** `docs/gd32-bridge-protocol.md` §3.1 (GPIO masks): add the two rows and the boot-default paragraph:

```markdown
| bit 18 | `BT_REG_ON`  | GD32 `PE14` | Murata module BT power enable (sideband, not an E1M pad) |
| bit 19 | `WL_REG_ON`  | GD32 `PE15` | Murata module Wi-Fi power enable (sideband, not an E1M pad) |

Unlike E1M pads, the two `REG_ON` bits boot as **outputs driven HIGH**
(module powered by default) after a ~10 ms low hold for a clean
power-on edge.  Hosts may drive them low/high via `GPIO_WRITE` to
power-cycle a wedged radio; expect the SDIO/UART device to vanish and
re-enumerate.
```

- [ ] **Step 6:** Build both backends exactly as the bench skill's recipe does (stub via host cc, gd32 via the arm toolchain — reuse the existing `firmware/gd32-bridge` build scripts/CMake used by the LCD/functional work). Expected: both compile clean; `_Static_assert` holds.
- [ ] **Step 7:** Commit:

```bash
git add firmware/gd32-bridge/hal/bridge_hw_gd32.c firmware/gd32-bridge/hal/bridge_hw_stub.c firmware/gd32-bridge/src/protocol.h docs/gd32-bridge-protocol.md
git commit -q -m "feat(gd32-bridge): REG_ON pad-map bits 18/19, boot default-on for Murata Wi-Fi/BT"
```

### Task 4: Host-side constants + smoke (TDD)

**Files:**
- Modify: `include/alp/chips/gd32g553.h` (near `gd32g553_gpio_write`, ~`:372`)
- Modify: `include/alp/chips/murata_lbee5hy2fy.h` (wiring table)
- Test: `tests/zephyr/chips/src/main.c`

- [ ] **Step 1: failing test first.** In `tests/zephyr/chips/src/main.c`, extend the existing murata/gd32g553 smoke section:

```c
	/* V2N REG_ON sideband: mask-bit constants must match the bridge
	 * pad map (bits 18/19) and stay distinct. */
	zassert_equal(GD32G553_GPIO_BT_REG_ON_BIT, 18u, "BT_REG_ON bit");
	zassert_equal(GD32G553_GPIO_WL_REG_ON_BIT, 19u, "WL_REG_ON bit");
	zassert_equal(GD32G553_GPIO_BT_REG_ON_MASK, 1u << 18, "BT mask");
	zassert_equal(GD32G553_GPIO_WL_REG_ON_MASK, 1u << 19, "WL mask");
```

(Place it inside the existing gd32g553 test function; follow the file's zassert style.)
- [ ] **Step 2:** Run the chips suite and watch it fail to compile (constants undefined). Use the canonical local twister invocation from `reference_local_twister_invocation` (WSL, full `--testsuite-root tests/zephyr`), or the narrower chips-only filter if the suite supports it. Expected: build error naming `GD32G553_GPIO_BT_REG_ON_BIT`.
- [ ] **Step 3:** Add to `include/alp/chips/gd32g553.h` right above `gd32g553_gpio_read`:

```c
/** @name Murata LBEE5HY2FY-922 REG_ON sideband (V2N/V2M)
 *  GPIO mask bits for the module power enables, which live on the
 *  GD32 (PE14/PE15) rather than the host SoC.  The bridge boots them
 *  OUTPUT-HIGH (module powered); drive low then high via
 *  gd32g553_gpio_write() to power-cycle the radio.  Matches
 *  docs/gd32-bridge-protocol.md §3.1 and the firmware pad map.
 *  @{ */
#define GD32G553_GPIO_BT_REG_ON_BIT  18u
#define GD32G553_GPIO_WL_REG_ON_BIT  19u
#define GD32G553_GPIO_BT_REG_ON_MASK (1u << GD32G553_GPIO_BT_REG_ON_BIT)
#define GD32G553_GPIO_WL_REG_ON_MASK (1u << GD32G553_GPIO_WL_REG_ON_BIT)
/** @} */
```

- [ ] **Step 4:** Update the wiring table in `include/alp/chips/murata_lbee5hy2fy.h` (and its `@par V2N board wiring` text): REG_ON rows gain "pad-map bit 18/19, `GD32G553_GPIO_*_REG_ON_MASK`"; fix the table's broken `| BT_DEV_WAKE` row (missing leading `*`, pre-existing typo); add the LPO row (`RV-3028-C7 CLKOUT → LPO_IN`) and the BT UART instance from Task 2. Do NOT remove `[UNTESTED]` yet.
- [ ] **Step 5:** Re-run the suite. Expected: PASS.
- [ ] **Step 6:** Commit:

```bash
git add include/alp/chips/gd32g553.h include/alp/chips/murata_lbee5hy2fy.h tests/zephyr/chips/src/main.c
git commit -q -m "feat(gd32g553): REG_ON mask constants + murata wiring-table truth"
```

### Task 5: Bench — flash bridge, verify power + no regression

REQUIRED SUB-SKILL: `flashing-and-bench-debugging-v2n` (J-Link recipe, SWD-attach gotcha, COM24 etiquette). **Coordinate J-Link/GD32 use with the LCD session before flashing** — their WIP firmware may be on the chip; capture `GET_BUILD_ID` first so it can be restored.

- [ ] **Step 1:** Record current GD32 state (build id via the existing host ping/functional example, or J-Link `mem32` of the version string) so the LCD session's image can be re-flashed if needed.
- [ ] **Step 2:** Build + flash `feat/v2n-wifi-ble` bridge firmware per the skill recipe (J-Link, `GD32G553MEY7TR`, base `0x08000000`).
- [ ] **Step 3:** Verify REG_ON electrically without any host code: J-Link memory read of GPIOE output register — expected: ODR bits 14/15 set, MODER shows outputs. (Exact register addresses are in the flashing skill's burst-sample recipe.)
- [ ] **Step 4:** Verify via protocol: run the existing CM33 functional-tier example (the 26/26 suite) — it must still pass everything that passed at the branch point, and a `GPIO_READ` with mask `0xC0000` returns `0xC0000`. If the functional example needs the new mask constants, extend its gpio case; otherwise a 2-line host scratch test in the example is fine (keep it; it becomes regression coverage).
- [ ] **Step 5:** On the Linux console (COM24 or ssh): SDIO bus check is NOT possible yet (no dtb support) — instead confirm the module is alive electrically: `i2cdetect`-style is N/A (module not on I2C); skip to Task 7 which gives the real proof (SDIO enumeration). Commit nothing; this is bench verification.

---

## Phase 1 — Linux bring-up (hand-built, over ssh)

### Task 6: Kernel rebuild — wireless out, BT in

**Files:** WSL `~/projects/rzv2n/kbuild-cip43` (out-of-repo build tree; the Yocto-proper fragment is Task 11)

- [ ] **Step 1: sanity.** `make -s kernelversion` → `6.1.141`. Confirm release-string parity with the board: deployed `uname -r` = `6.1.141-cip43-yocto-standard`; set `CONFIG_LOCALVERSION="-cip43-yocto-standard"`, `CONFIG_LOCALVERSION_AUTO=n` so module vermagic matches. Check `grep CONFIG_MODVERSIONS .config` (if `y`, the backports build needs the same tree — it gets it).
- [ ] **Step 2:** Apply the config delta:

```bash
cd ~/projects/rzv2n/kbuild-cip43
scripts/config --disable CFG80211 --disable MAC80211 \
  --enable RFKILL \
  --module BT --enable BT_BREDR --enable BT_LE \
  --module BT_RFCOMM --module BT_HIDP \
  --module BT_HCIUART --enable BT_HCIUART_H4 --enable BT_HCIUART_BCM \
  --module BT_BCM \
  --set-str LOCALVERSION "-cip43-yocto-standard" --disable LOCALVERSION_AUTO
make olddefconfig
grep -E "^CONFIG_(CFG80211|BT|BT_HCIUART|BT_HCIUART_BCM|RFKILL)" .config
```

Expected: `CFG80211` absent/`is not set`, `CONFIG_BT=m`, `CONFIG_BT_HCIUART=m`, `CONFIG_BT_HCIUART_BCM=y`, `CONFIG_RFKILL=y`. (Kernel cfg80211 must be OUT — the backports package ships its own `compat.ko`+`cfg80211.ko`, which cannot stack on a built-in copy. RFKILL stays in-kernel; backports consumes it.)
- [ ] **Step 3:** Build: `make -j$(nproc) Image modules` then `make modules_install INSTALL_MOD_PATH=/tmp/kmods`. Expected: clean build; `/tmp/kmods/lib/modules/6.1.141-cip43-yocto-standard/` exists.
- [ ] **Step 4: deploy with backups.** Discover the boot flow first: `ssh root@192.168.1.198 'ls -la /boot; cat /proc/cmdline'` and check whether the dtb/Image come from a FAT partition (`mmcblk0p1`) — mount and list it. Back up the current Image + dtb on-board (`cp` to `/root/`), then scp the new `arch/arm64/boot/Image` over the deployed one and rsync the modules dir alongside the existing one (do NOT delete the old modules dir). Reboot via the detached-setsid recipe (see `reference-v2n-board-bench` — a `reboot` at the end of an `&&` chain silently dies).
- [ ] **Step 5:** Verify: `uname -r` unchanged, board boots, Ethernet still up, `zgrep` N/A — instead `test -d /sys/module/bluetooth || modprobe bluetooth && echo BT-OK`. Expected: `BT-OK`; `cfg80211` no longer present (`ls /sys/module/cfg80211` fails).

### Task 7: dtb — SDHI1→WLAN + BT UART (hand-patch via dtbwork flow)

**Files:** WSL kernel worktree (`/tmp/n48wt2` per `V2N-Yocto/dtbwork/build_dtb.sh`); deployed dtb on the board

- [ ] **Step 1: diagnose the live dtb.** Pull what the board actually boots: `ssh root@192.168.1.198 'dtc -I fs -O dts /proc/device-tree 2>/dev/null | grep -A14 "sd1 {"'` (or copy the dtb file found in Task 6 Step 4 and decompile in WSL). Compare against `cur.dts:307-326`. Record why pinctrl said "no mapping found" (expected: the deployed dtb's `sd1` group lost its `pins`/`pinmux` subnodes somewhere in the hand-patch history — the patch-series dtbs in `dtbwork/patched.dts` are intact).
- [ ] **Step 2: author the overrides** in the kernel worktree's `r9a09g056n48-rzv2n-evk.dts`, following the patch-series append style (see `finalize.sh`'s 0010 example). First read the source dts + included dtsi for the real label names (`&sdhi1`, the `sd1` pinctrl group label, the Task-2 serial node label); then append:

```dts
/*
 * ALP e1m-x carrier: SDHI1 is the on-module Murata LBEE5HY2FY-922
 * (Infineon CYW55513) Wi-Fi SDIO link -- soldered, no card-detect, no
 * power switch (REG_ON lives on the GD32 supervisor; powered by
 * default).  Replace the EVK microSD description.
 */
&pinctrl {
	sd1_wlan_pins: sd1-wlan {
		sd1-clk {
			pins = "SD1CLK";
			renesas,output-impedance = <3>;
			slew-rate = <0>;
		};
		sd1-dat-cmd {
			pins = "SD1DAT0", "SD1DAT1", "SD1DAT2", "SD1DAT3", "SD1CMD";
			input-enable;
			renesas,output-impedance = <3>;
			slew-rate = <0>;
		};
	};
};

/ {
	reg_wifi_vbat: regulator-wifi-vbat {
		compatible = "regulator-fixed";
		regulator-name = "wifi-vbat";
		regulator-min-microvolt = <3300000>;
		regulator-max-microvolt = <3300000>;
		regulator-always-on;
	};
	reg_wifi_1v8: regulator-wifi-1v8 {
		compatible = "regulator-fixed";
		regulator-name = "wifi-vddio-1v8";
		regulator-min-microvolt = <1800000>;
		regulator-max-microvolt = <1800000>;
		regulator-always-on;
	};
};

&sdhi1 {
	pinctrl-0 = <&sd1_wlan_pins>;
	pinctrl-1 = <&sd1_wlan_pins>;
	pinctrl-names = "default", "state_uhs";
	vmmc-supply = <&reg_wifi_vbat>;
	vqmmc-supply = <&reg_wifi_1v8>;
	bus-width = <4>;
	non-removable;
	keep-power-in-suspend;
	cap-power-off-card;
	sd-uhs-sdr50;
	/delete-property/ sd-uhs-sdr104;
	#address-cells = <1>;
	#size-cells = <0>;
	status = "okay";

	wifi@1 {
		compatible = "brcm,bcm4329-fmac";
		reg = <1>;
	};
};
```

Adjust to source reality: if the EVK dts sets `cd-gpios`/CD pinmux or the `sd1-pwr-en-hog` for SDHI1, `/delete-property/`+`/delete-node/` them here. For BT (using the Task-2 node label, shown as `&serialX`):

```dts
/* BT HCI UART: Murata module BT_UART on P40-P43, 4-wire flow control. */
&serialX {
	uart-has-rtscts;
	status = "okay";

	bluetooth {
		compatible = "brcm,bcm43438-bt";
		max-speed = <3000000>;
	};
};
```

(plus its rtscts pinmux group if the source only muxes RX/TX — content from Task 2 Step 3.)
- [ ] **Step 3:** Build the dtb exactly as `build_dtb.sh` does (git-am worktree at `6717c06c` + existing patches 0006–0013, then this delta), producing `yocto_built.dtb`. Expected: dtc exits clean; `dtc -I dtb -O dts yocto_built.dtb | grep -A8 'wifi@1'` shows the node.
- [ ] **Step 4:** Deploy dtb (same path discovered in Task 6 Step 4, after on-board backup), detached reboot, then verify on console:

```
dmesg | grep -E 'sdhi|mmc1'
# EXPECTED: renesas_sdhi_internal_dmac 15c10000.mmc: mmc1 base ... (NO "no mapping found")
ls /sys/bus/sdio/devices            # EXPECTED: mmc1:0001:1 (and :2)
grep . /sys/bus/sdio/devices/*/vendor 2>/dev/null   # EXPECTED: 0x02d0 (Broadcom/Infineon)
```

This is the **REG_ON + LPO + pinmux proof** (spec §9 rows 1/2/4 all collapse here). If nothing enumerates: check REG_ON via J-Link (Task 5 Step 3), then scope LPO_IN per spec §9.
- [ ] **Step 5:** Save the exact dts delta used as a draft for Task 11 (keep the worktree diff: `git -C /tmp/n48wt2 diff > /mnt/e/GitHub/V2N-Yocto/dtbwork/wifi-bt-draft.diff`).

### Task 8: cyw-fmac backports build + firmware + wlan0

**Files:** WSL `~/projects/rzv2n/` (new `cyw-fmac/` clone); board `/lib/firmware/cypress/`, `/lib/modules/.../updates/`

- [ ] **Step 1:** Clone + pick the release:

```bash
cd ~/projects/rzv2n
git clone https://github.com/murata-wireless/cyw-fmac && cd cyw-fmac
git branch -a | tail -20    # pick the newest release branch; Infineon FMAC releases are tagged like v2025_xx
grep -rn "55500\|55513" drivers/net/wireless/broadcom/brcm80211/include/brcm_hw_ids.h
```

Acceptance: the chosen branch's `brcm_hw_ids.h` contains the `55500` family IDs (`devid 0xbd3e` per the 2FY NVRAM). If not on any branch, use Infineon's FMAC backports release bundle instead (same build interface) — record which one in the facts file.
- [ ] **Step 2:** Build against the bench kernel:

```bash
export KLIB_BUILD=~/projects/rzv2n/kbuild-cip43 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make KLIB_BUILD=$KLIB_BUILD defconfig-brcmfmac
make KLIB_BUILD=$KLIB_BUILD -j$(nproc)
find . -name '*.ko' | head    # EXPECTED: compat/compat.ko, net/wireless/cfg80211.ko, .../brcmfmac.ko, brcmutil.ko
```

- [ ] **Step 3:** Firmware blobs:

```bash
cd ~/projects/rzv2n
git clone --depth 1 https://github.com/murata-wireless/cyw-fmac-fw
git clone --depth 1 https://github.com/murata-wireless/cyw-fmac-nvram
ls cyw-fmac-fw | grep 55500     # EXPECTED: cyfmac55500-sdio.trxse (+ .clm_blob variants)
ls cyw-fmac-nvram | grep 2FY    # EXPECTED: cyfmac55500-sdio.2FY.txt
```

Deploy to the board: `cyfmac55500-sdio.trxse`, the **STA** CLM blob renamed to `cyfmac55500-sdio.clm_blob`, NVRAM as `cyfmac55500-sdio.txt` → `/lib/firmware/cypress/` (follow the cyw-fmac-fw README naming exactly — adjust if it prescribes `brcmfmac55500-sdio.*` aliases; record what worked). Also scp the four `.ko`s into `/lib/modules/$(uname -r)/updates/` + `depmod -a` on the board.
- [ ] **Step 4:** Load + verify:

```
modprobe brcmfmac
dmesg | tail -20   # EXPECTED: brcmfmac: F1 signature read ... chip 55500/55513, firmware version line, no NVRAM errors
ip link show wlan0 # EXPECTED: wlan0 exists
iw dev wlan0 scan | grep SSID | head   # EXPECTED: visible networks
```

- [ ] **Step 5:** Connect (ask the user for bench AP SSID/PSK at this point):

```
wpa_passphrase '<SSID>' '<PSK>' > /etc/wpa_supplicant.conf
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
udhcpc -i wlan0   # or dhclient/dhcpcd, whatever the image carries
ping -c3 192.168.1.1
```

Throughput: `iperf3 -s` on the board, client from WSL (`iperf3 -c <wlan0-ip> -t 20`). EXPECTED: ≥ 40 Mbit/s TCP.
- [ ] **Step 6:** Append actual results (chip id line, fw version, iperf number) to the facts file; commit it.

### Task 9: BT bring-up

**Files:** board runtime; WSL `cyw-bt-patch` clone

- [ ] **Step 1:** `git clone --depth 1 https://github.com/murata-wireless/cyw-bt-patch; ls | grep -i 2FY` → pick the CYW55513/2FY `.hcd`; scp to board `/lib/firmware/brcm/` AND `/etc/firmware/` (serdev/btbcm looks in `/lib/firmware/brcm`, hciattach in `/etc/firmware` — covering both costs nothing; record which got used).
- [ ] **Step 2:** Serdev path first (dtb child node from Task 7 already present): `dmesg | grep -i 'hci\|bluetooth'` after boot. EXPECTED if it works: `hci0` registered, `.hcd` patch loaded. Then `bluetoothctl -- scan on` for ~10 s sees devices.
- [ ] **Step 3:** If serdev/btbcm rejects the part: fallback `btattach -B /dev/ttySCx -P bcm -S 3000000 &` (the tty for the Task-2 node; check `dmesg | grep ttyS` for its name). Then re-verify hci0 + scan. Record which path won (decides Task 11's image bits: serdev = nothing extra; btattach = a small systemd unit recipe added to the image install in Task 12).
- [ ] **Step 4:** Append BD address + scan evidence to the facts file; commit.

### Task 10: Robustness battery (V2N)

- [ ] **Step 1: power-cycle recovery.** From the CM33 side (functional example gpio case) or J-Link, drive `WL_REG_ON`+`BT_REG_ON` low, wait 1 s, high. On Linux: `echo 15c10000.mmc > /sys/bus/platform/drivers/renesas_sdhi_internal_dmac/unbind` + rebind (or `mmc1` rescan), `modprobe -r brcmfmac && modprobe brcmfmac`. EXPECTED: wlan0 returns, reconnects. Record the working recovery sequence verbatim in the facts file — it becomes the documented recovery recipe in Task 13.
- [ ] **Step 2: reboot persistence.** Detached reboot; EXPECTED: wlan0 + hci0 reappear without manual steps (wpa_supplicant re-run is fine at this phase).
- [ ] **Step 3: 30-min soak.** `iperf3 -t 600` × 3 + idle gaps; then `dmesg | grep -icE 'crc|timeout.*mmc1|sdio.*err'` EXPECTED: 0. If CRC errors > 0: stay at SDR50, note for spec §9 row 6; if 0, optionally re-test with `sd-uhs-sdr104` restored (separate dtb build) and record.

---

## Phase 2 — productionize

### Task 11: DT patches 0014/0015 + kernel config fragment (Yocto-proper)

**Files:**
- Create (WSL): `…/meta-renesas/meta-rz-bsp/recipes-kernel/linux/files/0014-arm64-dts-renesas-rzv2n-evk-murata-wifi-sdhi1-on-e1m-x.patch` and `0015-…-murata-bt-hci-uart-on-e1m-x.patch`
- Create: `E:\GitHub\V2N-Yocto\dtbwork\staging\wifi-bt.cfg`
- Modify: `E:\GitHub\V2N-Yocto\dtbwork\staging\linux-renesas_%.bbappend`

- [ ] **Step 1:** In the kernel worktree (patches 0006–0013 applied), commit the Task-7 dts delta as two commits (SDHI1/regulators/wifi-node; BT UART/serdev) with `Alp Lab AB` attribution (NO personal name/email — set `git config user.name "Alp Lab AB"`, `user.email` the SDK contact), then `git format-patch -2` and place as 0014/0015 in the patch dir.
- [ ] **Step 2:** `wifi-bt.cfg` (kernel fragment, mirrors Task 6 exactly):

```
# Murata LBEE5HY2FY-922 (CYW55513): wireless stack comes from the
# cyw-fmac backports kmod -- the kernel's own cfg80211 must be OFF.
# CONFIG_CFG80211 is not set
# CONFIG_MAC80211 is not set
CONFIG_RFKILL=y
CONFIG_BT=m
CONFIG_BT_BREDR=y
CONFIG_BT_LE=y
CONFIG_BT_RFCOMM=m
CONFIG_BT_HIDP=m
CONFIG_BT_HCIUART=m
CONFIG_BT_HCIUART_H4=y
CONFIG_BT_HCIUART_BCM=y
CONFIG_BT_BCM=m
```

- [ ] **Step 3:** bbappend additions:

```bitbake
SRC_URI:append:e1m-v2n101 = " \
    file://0014-arm64-dts-renesas-rzv2n-evk-murata-wifi-sdhi1-on-e1m-x.patch \
    file://0015-arm64-dts-renesas-rzv2n-evk-murata-bt-hci-uart-on-e1m-x.patch \
    file://wifi-bt.cfg \
"
```

- [ ] **Step 4:** Re-run the replay check (`build_dtb.sh` pattern extended to 0014/0015): patches `git am` clean, dtb builds, decompiled output matches the hand-deployed Task-7 dtb (`dtc -I dtb -O dts` diff — only phandle numbering may differ). Commit the V2N-Yocto staging changes (that repo's normal flow).

### Task 12: meta-alp-sdk recipes + image + bake

**Files:**
- Create: `meta-alp-sdk/recipes-kernel/cyw-fmac/cyw-fmac_git.bb`
- Create: `meta-alp-sdk/recipes-bsp/cyw-fmac-firmware/cyw-fmac-firmware_git.bb`
- Modify: `meta-alp-sdk/conf/machine/e1m-v2n101-a55.conf` (and the v2n102/v2m101/v2m102 siblings — same PCB, same lines)
- Modify: `E:\GitHub\V2N-Yocto\dtbwork\staging\e1m-v2n.yml` (kas)

- [ ] **Step 1:** `cyw-fmac_git.bb` (pin SRCREV/branch to what Task 8 validated):

```bitbake
SUMMARY = "Infineon/Murata cyw-fmac (brcmfmac backports) for CYW55513 (Murata Type 2FY)"
HOMEPAGE = "https://github.com/murata-wireless/cyw-fmac"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=<fill-from-checkout>"

inherit module

SRC_URI = "git://github.com/murata-wireless/cyw-fmac;protocol=https;branch=<task8-branch>"
SRCREV = "<task8-srcrev>"
S = "${WORKDIR}/git"

# backports build, not a plain Kbuild module
EXTRA_OEMAKE = "KLIB_BUILD=${STAGING_KERNEL_BUILDDIR} KLIB=${D}${nonarch_base_libdir}/modules/${KERNEL_VERSION}"
do_configure() {
    oe_runmake KLIB_BUILD=${STAGING_KERNEL_BUILDDIR} defconfig-brcmfmac
}
RPROVIDES:${PN} += "kernel-module-brcmfmac kernel-module-cfg80211"
```

(`<fill-from-checkout>` values are computed during this task from the actual clone — `md5sum COPYING`, `git rev-parse HEAD`; they are required recipe pins, not optional.)
- [ ] **Step 2:** `cyw-fmac-firmware_git.bb`:

```bitbake
SUMMARY = "Murata Type 2FY (CYW55513) WLAN firmware + NVRAM + CLM + BT patch"
LICENSE = "Firmware-cypress"
LIC_FILES_CHKSUM = "file://LICENCE.cypress;md5=<fill-from-checkout>"
NO_GENERIC_LICENSE[Firmware-cypress] = "LICENCE.cypress"

SRC_URI = " \
    git://github.com/murata-wireless/cyw-fmac-fw;protocol=https;branch=<b>;name=fw;destsuffix=fw \
    git://github.com/murata-wireless/cyw-fmac-nvram;protocol=https;branch=<b>;name=nvram;destsuffix=nvram \
    git://github.com/murata-wireless/cyw-bt-patch;protocol=https;branch=<b>;name=bt;destsuffix=bt \
"
SRCREV_fw = "<pin>" 
SRCREV_nvram = "<pin>"
SRCREV_bt = "<pin>"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/cypress ${D}${nonarch_base_libdir}/firmware/brcm
    install -m0644 ${WORKDIR}/fw/cyfmac55500-sdio.trxse           ${D}${nonarch_base_libdir}/firmware/cypress/
    install -m0644 ${WORKDIR}/fw/cyfmac55500-sdio.<sta-clm>.clm_blob ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.clm_blob
    install -m0644 ${WORKDIR}/nvram/cyfmac55500-sdio.2FY.txt      ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.txt
    install -m0644 ${WORKDIR}/bt/<task9-hcd>                      ${D}${nonarch_base_libdir}/firmware/brcm/
}
FILES:${PN} = "${nonarch_base_libdir}/firmware"
```

(Names/symlinks adjusted to exactly what Task 8/9 proved; the bench facts file is the authority. Add the alias symlinks if brcmfmac requested different names in the Task-8 dmesg.)
- [ ] **Step 3:** Machine confs (all four V2N/V2M, identical):

```bitbake
MACHINE_EXTRA_RRECOMMENDS += "cyw-fmac cyw-fmac-firmware wireless-regdb-static"
```

plus image userland via the kas/local config used by the bake: `wpa-supplicant bluez5 iw iperf3` (+ the btattach unit package if Task 9 needed it — if so, create `meta-alp-sdk/recipes-connectivity/murata-bt-attach/murata-bt-attach.bb` installing a 6-line systemd unit running the exact Task-9 command; content comes from the facts file).
- [ ] **Step 4:** Full bake in WSL (the validated flow from `build-yocto-v2n.md` / `project-v2n-linux-build-validation`), deploy the new Image+dtb+rootfs artifacts (or the delta: Image, dtb, modules, firmware, userland packages) and re-run the Task 8–10 battery from the clean image. EXPECTED: all green with zero hand-deployed files.
- [ ] **Step 5:** Commit meta-alp-sdk changes in the worktree; commit V2N-Yocto staging in its repo.

### Task 13: alp-sdk metadata + docs + CHANGELOG

**Files:**
- Modify: `metadata/chips/murata_lbee5hy2fy.yaml`
- Modify: `include/alp/chips/murata_lbee5hy2fy.h` (drop `[UNTESTED]`)
- Modify: `docs/bring-up-v2n.md`, `docs/soms/v2n.md`, `CHANGELOG.md`

- [ ] **Step 1:** `murata_lbee5hy2fy.yaml`: `driver_status: partial` stays (GPIO surface by design); add `bt_uart` instance + `lpo: rv3028c7 clkout` facts under a `notes:`/`side_channel_lines` update; REG_ON entries gain `pad_map_bit: 18/19`; `verification.hil_silicon: passed-2026-06-XX (V2N101 + V2M101)` after Task 15.
- [ ] **Step 2:** `docs/bring-up-v2n.md`: new "Wi-Fi + Bluetooth" section — power path (GD32 default-on, recovery sequence from Task 10 Step 1 verbatim), what the image ships (backports kmod, blobs, fragment), the validation battery commands + expected outputs (mirror Tasks 7–10), SDR50/104 status. `docs/soms/v2n.md:32` row: status verified. Run the doc gates locally (doc-lint + drift checker).
- [ ] **Step 3:** CHANGELOG entry under the next version: `V2N/V2M Wi-Fi 6 + BT 5.4 silicon bring-up (Murata LBEE5HY2FY-922 / CYW55513): GD32 REG_ON power path (protocol vX.Y), Linux SDIO+HCI-UART via cyw-fmac backports, Yocto recipes`.
- [ ] **Step 4:** Commit: `git add -A && git commit -q -m "docs+metadata(v2n): Murata Wi-Fi/BT verified on silicon; bring-up + recovery documented"` (split into two commits if metadata and docs land at different verification stages).

### Task 14: Local CI + merge to dev

REQUIRED SUB-SKILL: `running-local-ci` (full Windows + WSL gate list). 

- [ ] **Step 1:** Run ALL local gates on the worktree (twister native_sim full testsuite-root, pytest `py -3.14`, clang-format diff, doc gates, schema checks). EXPECTED: all green; fix anything that isn't before proceeding.
- [ ] **Step 2:** Rebase/merge against `dev` (the LCD session has been merging there): resolve `firmware/gd32-bridge/` overlaps — pad-map rows are append-only so conflicts are textual; **re-check the protocol version number is still the next free minor** and that their pad-map/doc changes didn't take bits 18/19. Re-run the firmware builds (stub + gd32) after merge. If the GD32 on the bench has moved past my flash, re-flash the merged firmware and re-run the functional tier + a Task-7-style SDIO enumeration check before calling it merged.
- [ ] **Step 3:** Merge to `dev` with the repo's `git merge --no-ff -m` pattern. No Claude co-author footer.

### Task 15: V2M101 validation

REQUIRED SUB-SKILL: `flashing-and-bench-debugging-v2n` + read `project-e1m-v2m-bench-handoff` memory first (board state, Ethernet-E1 caveat — use serial console + the Wi-Fi link itself for transfers if end0 is down).

- [ ] **Step 1:** Flash merged GD32 bridge to the V2M101's GD32; deploy the same Image/dtb/modules/blobs (same PCB → same dtb).
- [ ] **Step 2:** Run the Task 7–10 battery (enumeration, scan/connect/iperf3, hci0, power-cycle recovery, short soak). EXPECTED: identical results.
- [ ] **Step 3:** Update `verification.hil_silicon` (Task 13 Step 1) to include V2M101 + date; commit.

### Task 16: Internal sync + memory

REQUIRED SUB-SKILL: `syncing-internal-repo`.

- [ ] **Step 1:** Sync detail-rich mirrors in `alp-sdk-internal` (metadata deltas, any bench notes worth keeping team-side), sanitized per the claude-context rules.
- [ ] **Step 2:** Update the session memory file (`project-v2n-wifi-ble-lbee5hy2fy`) to final state: what shipped, fw/protocol versions, recovery recipe, V2M status.

---

## Self-review notes

- **Spec coverage:** §1 success criteria → Tasks 8/9/10/15 (+CI in 14); §3 power path → 3/4/5; §4 driver → 8/11/12; §5 DT → 7/11; §6 Yocto → 12; §7 alp-sdk → 3/4/13; §8 phases → task ordering; §9 risks: cfg80211 → Task 6 (resolved: kernel had no BT and built-in cfg80211, fragment handles both), SDHI1 pins → 2/7, RSCI → 2, LPO → 7 Step 4, btbcm → 9, SDR104 → 10 Step 3, licensing → 12 (build-time fetch).
- **Known unknowns are bench-resolved by design** (firmware blob exact names, serdev-vs-btattach, backports branch): each has a discovery step with acceptance criteria and feeds the facts file that later tasks consume — no dangling decisions.
- **`<fill-from-checkout>`/`<pin>` recipe fields** are computed inside Task 12 from the validated checkout; the task says exactly how.
