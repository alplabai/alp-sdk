# V2N/V2M Wi-Fi + BLE port — Murata LBEE5HY2FY-922 (design)

Date: 2026-06-04
Status: approved (Approach A)
Scope: E1M-V2N101/V2N102/V2M101/V2M102 (one PCB, variant-populated — a fix to
one applies to all four)

## 1. Goal

Bring the on-module Murata LBEE5HY2FY-922 Wi-Fi 6/6E + BLE 5.4 module (Murata
Type 2FY, Infineon CYW55513) up on the V2N/V2M SoMs: Wi-Fi station mode and
Bluetooth on the A55/Linux side, with the GD32-owned power path made real.
BT audio (I2S) is explicitly out of scope for this slice; HOST_WAKE / OOB-IRQ
power management is deferred to a later PM slice.

**Success criteria** (all on real silicon):

- `wlan0`: scan → WPA2 connect → DHCP → `iperf3` ≥ 40 Mbit/s TCP (1×1 HE20).
- `hci0`: up with a valid BD address; `bluetoothctl` inquiry sees devices.
- REG_ON power-cycle via the GD32 bridge recovers the radio without reboot.
- Reboot persistence + ~30 min idle/traffic soak with zero SDIO CRC errors.
- Validated on **both** V2N101 and V2M101 bench boards.
- All local CI gates green; docs + metadata updated in the same slice.

## 2. Ground truth (verified 2026-06-04)

Wiring (consistent across public metadata, internal metadata, and the SoM
schematic; see `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` and
`gd32-io-mcu-map.tsv`):

| Signal | Route |
|---|---|
| WIFI_SDIO CLK/CMD/D0-D3 | Renesas PB0/PB5/PB1/PB2/PB3/PB4 → SDHI1 @ `0x15c10000` |
| BT_UART RXD/TXD/RTS/CTS | Renesas P40/P41/P42/P43 |
| WL_HOST_WAKE / BT_HOST_WAKE | Renesas P72 / P05 (deferred, PM slice) |
| WL_REG_ON / BT_REG_ON | GD32 PE15 / PE14 (module has internal 50 kΩ pull-downs) |
| BT_DEV_WAKE | intentionally not routed on V2N |
| LPO 32.768 kHz | RV-3028-C7 RTC CLKOUT → module LPO_IN |
| VBAT 3.3 V / VDDIO 1.8 V | always-on rails (not GPIO-switched) |

Software state today:

- Kernel 6.1.141-cip43: `brcmfmac` tops out at CYW43752 — **no CYW55513
  support**; `cfg80211` is **built-in**; no Wi-Fi/BT firmware on the image.
- Deployed carrier dtb: `mmc@15c10000` is `okay` but its `sd1` pinctrl group
  is empty → `pinctrl-rzg2l: no mapping found in node .../sd1` → SDHI1 never
  probes (live bug, observed on the bench).
- GD32 bridge firmware: boots every mapped pad INPUT+PULL_UP; PE14/PE15 are
  **not in the GPIO pad map**, so module power is indeterminate today
  (GD32 pull-up vs module 50 kΩ pull-down).
- alp-sdk: `chips/murata_lbee5hy2fy/` thin GPIO surface exists and is
  paper-correct/`[UNTESTED]`; all four SoM presets declare the module.

## 3. Architecture & ownership

- **A55/Linux owns both data paths.** Wi-Fi: Murata `cyw-fmac` backports
  driver (out-of-tree kmod) over SDHI1, in-band SDIO IRQ. BT: BlueZ over an
  RSCI HCI UART at 3 Mbaud with RTS/CTS, serdev (`btbcm`) preferred,
  `btattach` systemd unit as fallback.
- **GD32 bridge owns power sequencing.** PE14/PE15 become pad-map bits 18/19
  reachable through the existing `CMD_GPIO_READ`/`CMD_GPIO_WRITE` opcodes
  (no new opcode). Boot-time these two deviate from the E1M-pad
  INPUT+PULL_UP rule: configured OUTPUT push-pull, held LOW ~10 ms for a
  clean power-on edge, then driven HIGH — the module is powered by default
  and Linux needs no special sequencing to probe. `PROTOCOL_VERSION` gets a
  minor bump.
- **CM33/Zephyr is not on the data path.** The murata chip driver stays the
  thin GPIO surface; its REG_ON callbacks become real via
  `gd32g553_gpio_write()` with the new mask bits (smoke-test only).
- A GD32 reset power-glitches the radio (pins pass through reset defaults).
  Accepted: host power-cycle recovery is the designed remedy.

## 4. Kernel driver strategy (Approach A — decided)

Murata `cyw-fmac` backports as an **out-of-tree kernel-module package**; the
pinned kernel source stays untouched. Rationale: stock 6.1 lacks the chip ID
entirely; Infineon/Murata ship CYW55513 support as backports (buildable for
kernels < ~6.1.145; ours is 6.1.141); an in-kernel patch series would mean
owning a whole-driver refresh forever for no functional gain.

Firmware: `cyfmac55500-sdio` WLAN firmware + `…-sdio.2FY.txt` NVRAM + 2FY CLM
blob → `/lib/firmware/cypress/`; BT `.hcd` patch from `cyw-bt-patch`;
`wireless-regdb` for the regulatory database. All fetched at build time from
the public `murata-wireless` GitHub org — nothing redistributed by alp-sdk.

## 5. Device tree (patches 0014/0015 in the carrier bbappend series)

**0014 — SDHI1 → WLAN** (also fixes the live empty-pinctrl bug):

- Restore the `sd1` pinmux group with the SD1-on-PB0–PB5 function encodings
  (present in the EVK source dts), minus `SD1_CD`.
- `mmc@15c10000`: `bus-width <4>`, `non-removable`, `keep-power-in-suspend`,
  `cap-power-off-card`; no `cd-gpios`, no EVK `sd1-pwr-en` hog;
  `vmmc-supply`/`vqmmc-supply` → fixed-regulators (3.3 V / 1.8 V always-on
  rails); start `sd-uhs-sdr50`, promote to `sdr104` after the soak passes.
- Child `wifi@1 { compatible = "brcm,bcm4329-fmac"; }` — in-band SDIO IRQ.

**0015 — BT UART:**

- Enable the RSCI instance that muxes P40–P43 (exact `serial@…` node
  verified against the HW-manual pinmux tables before the patch is written),
  `uart-has-rtscts`, all four lines muxed.
- Serdev child `bluetooth { compatible = "brcm,bcm43438-bt"; max-speed =
  <3000000>; }` if 6.1 `btbcm` drives the CYW55513 `.hcd` load; otherwise the
  `btattach` unit. Decided empirically in Phase 1.

Kernel config fragment (only if needed, see risk table): `CONFIG_CFG80211=m`,
`CONFIG_BT` + `CONFIG_BT_HCIUART(_BCM)` if missing.

## 6. Yocto packaging

Recipes land in **`meta-alp-sdk`** (the customer-buildable layer that already
carries `conf/machine/e1m-v2n101-a55.conf`); the bench kas manifest consumes
them. DT patches stay in the established carrier bbappend series.

- `kernel-module-cyw-fmac` — out-of-tree kmod recipe, `SRC_URI` pinned to a
  `murata-wireless/cyw-fmac` tag.
- `cyw-fmac-firmware` — WLAN fw + 2FY NVRAM + CLM under
  `/lib/firmware/cypress/` with the names brcmfmac expects.
- `cyw-bt-patch` — BT `.hcd`.
- Image: `wpa-supplicant`, `bluez5`, `wireless-regdb`, `iw`, `iperf3`,
  btattach unit if the serdev path loses.

## 7. alp-sdk changes

- `firmware/gd32-bridge/`: pad-map bits 18/19 + boot power-on sequence +
  `PROTOCOL_VERSION` minor bump (via the protocol-extension workflow,
  HIL-validated before merge). Done on a separate worktree branch; merged
  against the concurrent LCD-session bridge work when that lands.
- `include/alp/chips/gd32g553.h`: named mask-bit constants
  (`GD32G553_GPIO_BT_REG_ON` / `…_WL_REG_ON`) + Doxygen.
- `chips/murata_lbee5hy2fy/`: no API change; header wiring-table updates
  (exact BT UART instance, LPO source, REG_ON mask bits); `[UNTESTED]`
  banner dropped only after HIL passes.
- `metadata/chips/murata_lbee5hy2fy.yaml` + shared v2n metadata: BT UART
  instance + LPO facts; `verification.hil_silicon` updated post-validation;
  all four SKU presets stay identical (one-PCB rule).
- Docs: `docs/bring-up-v2n.md` Wi-Fi/BT section; `docs/soms/v2n.md`;
  `docs/gd32-bridge-protocol.md` REG_ON rows; CHANGELOG.
- Internal-repo sync of the detail-rich mirrors afterward.

## 8. Validation plan

**Phase 1 — V2N bench, prove the chain before packaging:**

1. Flash the new bridge build (J-Link use sequenced with the LCD session);
   verify REG_ON via `CMD_GPIO_READ` from Linux over BRD_I2C and confirm the
   GD32 functional tier still passes (no regression).
2. Hand-patched dtb + WSL-built backports modules + blobs → SDHI1 probes,
   SDIO enumerates Broadcom `0x02d0`, `wlan0` appears.
3. Wi-Fi: scan → WPA2 connect → DHCP → `iperf3`.
4. BT: serdev-or-btattach → `hci0` up, inquiry works.
5. Robustness: reboot persistence; REG_ON power-cycle recovery; soak.

**Phase 2 — productionize:** recipes + DT patches as designed; full image
bake (WSL); re-run the battery from a clean image.

**Phase 3 — V2M101:** same smoke battery on the V2M bench board.

## 9. Open verification items (flagged, not invented)

| Item | Resolution path |
|---|---|
| cfg80211 built-in vs backports-stack clash | First thing proven on bench; fallback `CONFIG_CFG80211=m` fragment |
| SDHI1 ↔ PB0–PB5 mux assumption | Decode EVK dts pinmux values against the HW manual before patch 0014 |
| Which RSCI instance = P40–P43 | HW-manual pinmux table before patch 0015 |
| RV-3028 CLKOUT actually enabled (LPO) | SDIO enumeration is the proof; scope the pin if it fails |
| 6.1 `btbcm` vs CYW55513 `.hcd` | Try serdev first, fallback `btattach` |
| SDR104 timing on this layout | Start SDR50, promote after soak |
| Murata blob licensing | Build-time fetch from public GitHub; no redistribution |
