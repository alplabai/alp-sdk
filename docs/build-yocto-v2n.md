# Building & deploying the V2N Linux image (Yocto)

How a customer builds and deploys the **kernel + root filesystem** for
the E1M-V2N101 / E1M-V2N102 / E1M-V2N103 SoM on the E1M-X-EVK (or a pin-compatible
custom carrier).  The build itself is the `bitbake-layers` flow in
[`../meta-alp-sdk/README.md`](../meta-alp-sdk/README.md); this page adds
the V2N-specific BSP, deploy, and on-board verification detail.

> **Bootloader is production-flashed by Alp.** Every SoM ships with BL2
> (alp LPDDR4X DDR init) + FIP already on the on-module xSPI, so it
> reaches U-Boot on first power-on. Your normal flow **never rebuilds
> the bootloader** — you build only kernel+rootfs below. Bootloader
> rebuild/recovery lives in `alp-sdk-internal` (see
> [`e1m-x-v2n-sdk-integration.md`](e1m-x-v2n-sdk-integration.md)).
>
> **The firmware pack is per memory tier, not shared across a family.**
> `E1M-V2N103` / `E1M-V2M103` populate a different (4 GB) DRAM tier of the
> same V2N-family PCB as `E1M-V2N101` / `E1M-V2M101`, and their BL2 needs a
> different DDR param (`L4X.R2W32X16D8S32.ADEE` vs the family's
> `L4X.R2W32X16D16S32.ADEE`) plus a different U-Boot `CONFIG_SYS_SDRAM_SIZE`
> / control-DT memory node — selected automatically by `MACHINE` in
> `meta-alp-sdk/recipes-bsp/{trusted-firmware-a,u-boot}/*_%.bbappend`. This
> only matters if you rebuild the bootloader yourself (`alp-sdk-internal`
> flow above); the on-module xSPI you receive already carries the right one
> for your SoM.
>
> **OPEN: production `E1M-V2N101`/`E1M-V2M101` DRAM part/tier undecided.**
> Their catalogue entry (`metadata/e1m_modules/E1M-V2N101.yaml`,
> `E1M-V2M101.yaml`: `dram_mbit: 32768` = 4 GB) states the same 4 GB the
> x103 tier above targets, yet their firmware still ships the family-default
> 8 GB D16S32 config — a production blocker, not resolved by this doc or by
> the x103 firmware-pack work. Firmware stays D16S32 for V2N101/V2M101 until
> this resolves.

## 1. Prerequisites

- Linux host (or WSL2 Ubuntu 22.04), ~60 GB free, 8+ GB RAM.
- The usual Yocto host deps
  (`gawk wget git diffstat unzip build-essential chrpath cpio …`).
- **The Renesas RZ/V2N AI SDK BSP** (license-gated) — **AI SDK
  platform 7.1 on BSP v6.30 (`RTK0EF0189F06300SJ`, linux-renesas
  6.1.141-cip43)**.  Fetch the **Source Code** package
  (`RTK0EF0189F06300SJ_linux-src.zip`) from your Renesas account and
  extract it; it carries `meta-renesas` + `meta-rz-features/*`.  alp-sdk
  does **not** redistribute it.

## 2. Assemble the layers

Follow [`../meta-alp-sdk/README.md`](../meta-alp-sdk/README.md): extract
the Source Code package's recipe tarball, `source oe-init-build-env`,
then `bitbake-layers add-layer` the Renesas feature sublayers +
`meta-alp-sdk` (and `meta-deepx-m1` for V2N-M1).  `meta-renesas` comes
from the extracted BSP, not a public clone.

## 3. Build

```bash
MACHINE=e1m-v2n101-a55 bitbake alp-image-edge
# (V2N102: MACHINE=e1m-v2n102-a55;  V2N103: e1m-v2n103-a55;
#  V2N-M1: e1m-v2m101-a55 / e1m-v2m102-a55 / e1m-v2m103-a55)
```

Output (under `build/tmp/deploy/images/e1m-v2n101-a55/`):
- `alp-image-edge-*.wic[.gz]` — full SD/eMMC image (bootloader excluded;
  it's already on xSPI).
- `alp-image-edge-*.wic.bmap` — block map of the wic (`wic.bmap` in
  `IMAGE_FSTYPES`); ship it as the bundle's `system_image_bmap` so the
  provisioning tool writes only the used blocks
  (see [provisioning-v2n.md](provisioning-v2n.md)).
- `Image` + `renesas/e1m-v2n101-x-evk.dtb` — kernel + the **carrier
  dtb** (composed from the SoC + SoM + E1M-X-EVK carrier dtsi and selected
  via the machine's `KERNEL_DEVICETREE`, so this is the e1m-x carrier dtb
  the shipped bootloader loads — the stock `r9a09g056n48-rzv2n-evk.dtb` is
  not built).

### Edge vs production image

Two images share one runtime (`alp-image-common.inc`: SDK, ROS 2 +
perception, GStreamer/libcamera, Mender, Weston, watchdog, networkd) and
differ only in posture:

| | `alp-image-edge` | `alp-image-prod` |
|---|---|---|
| Root login | passwordless (`debug-tweaks`) | locked; **SSH key-only** (key provisioned per unit) |
| Dev tooling | `libdrm-tests`/modetest, profilers | stripped |
| Discovery daemons | default | avahi/connman/ofono/rpcbind/tcf-agent trimmed |
| Branding | (set by `DISTRO`) | (set by `DISTRO`) |

Build the production image against the **`alp` distro** so the rootfs
carries an Alp identity (`/etc/os-release`, `/etc/issue`, the login
banner say `Alp SDK <version>`, read from `include/alp/version.h`) instead of the upstream
`Poky (Yocto Project Reference Distro)` reference-distro banner:

```bash
DISTRO=alp MACHINE=e1m-v2m101-a55 bitbake alp-image-prod
```

`DISTRO=alp` (`meta-alp-sdk/conf/distro/alp.conf`) is an identity-only
override of Renesas's `rz-vlp` — it inherits the entire BSP/graphics/Mender
feature set and changes no `DISTRO_FEATURES`, so it is equally usable for
`alp-image-edge`. The production hardening (no passwordless root, key-only
SSH via `alp-ssh-hardening`, trimmed services) lives in the image recipe,
not the distro.

> **Scope of the hardening:** it removes the remote *dev/debug daemons*
> (tcf-agent, zero-conf/RPC/telephony) and locks login. It does **not**
> constrain the ROS 2 payload: `alp-perception` + the ROS 2 stack ship in
> both images, and ROS 2's default DDS transport (FastDDS) opens
> unauthenticated discovery on **all interfaces**. On a deployed unit you
> must constrain it per deployment — `ROS_LOCALHOST_ONLY=1` for a single-host
> graph, a bound-interface FastDDS profile, or DDS-Security/SROS2 + a host
> firewall when the graph must be reachable across hosts. It is left to the
> integrator because the perception example documents a multi-host robot
> graph, so a forced loopback default would silently break it.

> **Kernel version pin:** the VLP-v5 `local.conf` template defaults to
> `PREFERRED_VERSION_linux-renesas = "6.12%"` — you **must** override this
> to `"6.1%"` (linux-renesas 6.1.141-cip43) for BSP v6.30.  Leaving it at
> the template default causes a recipe mismatch and build failure.

> **Machine fragments:** `alp-image-edge` picks up per-machine `.cfg`
> fragments from `meta-alp-sdk/recipes-kernel/linux/`.  For V2N with the
> display feature enabled, the active fragment list is `display.cfg`.
> (There is no audio fragment: the carrier TAS2563 codec has no DT node
> yet, so nothing would bind — see the audio TODO in `e1m-x-evk.dtsi`.)
> To build a minimal image without
> Weston/display, remove the `alp-lvgl-dashboard`, `weston`, and
> `weston-init` packages from `IMAGE_INSTALL` in your `local.conf` and
> drop the `display.cfg` fragment from the `SRC_URI` override.

## 4. Deploy the rootfs

The bootloader's `bootcmd` (rzv2n-dev config + the Alp 0002 patch)
loads `Image` from the ext4 rootfs `/boot` and then **reloads the
per-MACHINE board dtb** — `boot/e1m-v2n101-x-evk.dtb` on V2N101/V2N102/
V2N103, `boot/e1m-v2m101-x-evk.dtb` on V2M101/V2M102/V2M103 (each x103
MACHINE's `KERNEL_DEVICETREE` reuses its x101 sibling's dtb by design --
see the memory-tier rationale in `e1m-v2n103-a55.conf` /
`e1m-v2m103-a55.conf`) (issue #1175, closed as
#1252). The vendor env's hardcoded `boot/r9a09g056n44-dev.dtb` is a
filename **no Alp machine builds as a dtb**, on the eMMC branch as well as the
SD one, which is why the reload exists (the image links that name to the board
dtb so the vendor load succeeds harmlessly, #2637). If the dtb is missing from
`/boot`, the bootloader prints an error and **stops** — it does not
fall through and boot whatever devicetree is left in RAM.

The boot medium is auto-detected **per boot**: if an SD card is
present, root = `/dev/mmcblk1p2` (the carrier microSD, `&sdhi1` — see
`e1m-x-evk.dtsi`), otherwise eMMC `/dev/mmcblk0p2`
(`ALP_BOOT_DEVICE ?= "emmc"` names the provisioning default, not a
build split). **Bench-confirmed 2026-09-29 on E1M-V2M103
2026W38-0001:** U-Boot prefers a present microSD unconditionally --
this selection is independent of the DSW1 boot-mode switch. DSW1
(BOOT 2 = xSPI) only selects where **BL2/FIP** load from at boot ROM
time; it does not choose the Linux root device. Removing the microSD
falls through to eMMC (`root=/dev/mmcblk0p2`, HS200) with no DSW1
change required. The kernel cmdline is rebuilt by the Alp override
with `console=ttySC0,115200` pinned; dev builds keep `earlycon`.

**Production boot variant:** set `ALP_PROD_BOOT = "1"` for
release-bundle builds only — quiet cmdline (`quiet loglevel=4`, no
earlycon), `BOOTDELAY=0`, and keyed autoboot whose stop string is
injected by the internal release pipeline (an un-overridden prod build
has no stop sequence at all). Dev/bench builds keep the open 2 s
prompt. See `meta-alp-sdk/recipes-bsp/u-boot/u-boot/prod-boot.cfg`.

- **Full image:** write the `.wic` to the target device (eMMC via
  USB-gadget/`dd`, or SD via your host).
- **Fast dev iteration** (kernel/dtb only): copy `Image` +
  `e1m-v2n101-x-evk.dtb` into the running rootfs `/boot` over the
  network (`ssh root@<board> "cat > /boot/<f>" < <f>`) and reboot.
  (Plain `scp` *upload* to the board's dropbear can silently no-op; the
  `ssh cat >` redirect is reliable.)

## 5. Boot + verify

Console on E1M `UART0` @ 115200. After login (`root`), the carrier
smoke checks:

```bash
cat /proc/device-tree/model            # ALP e1m-x carrier + v2n-m1 SoM …
dmesg | grep -i over-current || echo none   # expect: none (suppressed via spurious-oc, errata E3)
grep timing /sys/kernel/debug/mmc0/ios      # expect: 9 (mmc HS200) -- HS-52 fallback means the eMMC rail fix regressed
i2cdetect -l                            # expect i2c-0/1/2/8 only
ethtool end0 | grep "Link detected"     # PHY attaches stmmac-N:02
cat /proc/version                       # expect "alp@alp-sdk", no "-dirty"/no personal host
```

The boot banners are pinned for traceability: BL2/BL31 read
`v2.10.5(release):alp`, U-Boot `2024.07-alp+`, kernel `alp@alp-sdk` —
all without a `-dirty` flag, upstream SHA, or builder `user@host`. A
drift back to `-dirty` means `BUILD_STRING` / `CONFIG_LOCALVERSION_AUTO`
regressed.

(End-to-end link needs the MDI-reversal layout fix — see
[`errata-e1m-x-v2n.md`](errata-e1m-x-v2n.md) E1 — until the respin, a
pair-mirror cable links at 100M.)

### CAN-FD bring-up (on-module TCAN1044 x2)

The two on-module CAN-FD channels come up as network interfaces but
carry **no bit timing in the device tree**: the bitrate is an operator
step, and the interfaces stay down (the controller is idle) until it is
set. Netdev names follow the E1M bus, not the driver's probe order
(`alp-canfd-udev`, #2352):

| E1M bus | Netdev | SoC channel | Transceiver |
|---|---|---|---|
| `E1M_X_CAN0` | `can_e1m0` | CANFD3 (`dev_port` 3) | U15 |
| `E1M_X_CAN1` | `can_e1m1` | CANFD2 (`dev_port` 2) | U16 |

Both transceivers share one standby line, CAN_STBY, driven low by a
`gpio-hog` on GD32 bridge line 20 (requires bridge protocol >= 0.13,
firmware >= 0.2.16; on an older bridge the write is never sent and both
transceivers stay in standby).

```bash
ip -br link | grep can_e1m                        # can_e1m0 + can_e1m1 present
cat /sys/class/net/can_e1m0/dev_port              # 3   (can_e1m1 -> 2)
ip link set can_e1m0 type can bitrate 500000 dbitrate 2000000 fd on      # repeat for can_e1m1; keep the driver's default sample points
ip link set can_e1m0 up
ip -d link show can_e1m0                          # shows the timing clock + bitrate + "fd on"
```

Nominal and data-phase timing must satisfy
`f = f_can / (BRP * (1 + TSEG1 + TSEG2))`, sample point
`(1 + TSEG1) / (1 + TSEG1 + TSEG2)`, `SJW <= min(TSEG1, TSEG2)`; the
kernel derives them from the requested rate and the controller clock
inherited from the SoC dtsi, so read the clock and limits from
`ip -d link show`, not from this repo. `fd on` is required for the
data phase (`dbitrate`); without it only classic CAN frames are sent.
`<alp/can.h>` does not set the bitrate (see `src/backends/can/yocto_drv.c`),
so configure it before opening the port.
If `can_e1m<N>` is missing and `can<N>` is an `rcar_canfd` netdev whose
`dev_port` is not the channel of E1M bus N, `alp_can_open()` returns
`ALP_ERR_NOT_READY` instead of opening the swapped port (install
`alp-canfd-udev`).

#### CAN-FD data phase: TDC and bus-off triage

Bench finding (E1M-V2M103, `can_clk` 80 MHz, `bitrate 500000 dbitrate
2000000 fd on`): a classic frame on a lone node ends ERROR-PASSIVE (ACK
errors only, as expected), but one FD frame with BRS
(`cansend can_e1m0 123##1.11`) drives it BUS-OFF with bit errors in the
data phase.

What the BSP kernel already does (no kernel patch needed): `rcar_canfd`
in the RZ/V BSP 6.1 tree implements transmitter delay compensation. When
the data BRP is 1 or 2 the CAN core selects TDC-AUTO and the driver
programs `CFDCnFDCFG.TDCE = 1`, `TDCOC = 0` (measured delay + offset) and
`TDCO = tdco - 1`, where `tdco` is the data sample point in clock periods
(80 MHz, 2 Mbit/s, 75 %: 40 clocks per bit, `tdco` 30). The register
layout matches RZ/V2N manual R01UH1071 section 7.9 (CFDCnFDCFG, CFDCnDCFG,
CFDCnNCFG) and section 7.9.4.1.5 (transmitter delay compensation). Manual
control is available: `ip link set can_e1m0 type can ... tdc-mode
auto|manual|off tdco N tdcv N` (needs an iproute2 with `tdc-mode`); in
manual mode the driver programs the offset-only variant (`TDCOC = 1`,
SSP = `tdcv + tdco`).

Is BUS-OFF on a lone node with BRS expected? No.

- A lone node gets no ACK. The ACK slot is sent at the nominal rate after
  the data phase, so BRS does not change it. An error-passive transmitter
  does not raise TEC for an ACK error (ISO 11898-1 fault confinement), so a
  lone node parks at ERROR-PASSIVE for classic and FD frames alike.
- BUS-OFF needs TEC > 255, i.e. repeated real bit errors (the node sent a
  bit and read back a different level at its sample point). Those are not
  produced by the missing ACK.
- Loop delay is not a first-order suspect at 2 Mbit/s: the TCAN1044 loop
  delay is about 100 to 175 ns (TI datasheet, loop-delay parameter) against
  a data sample point of 375 ns, and TDC is enabled anyway. TDC starts to
  matter near 5 Mbit/s and above (manual section 7.9.4.1.5). The maximum
  compensable delay is 6 data bit times minus 2 DLL clocks.

So the failure is a bench finding to localize, not something TDC alone
explains. Run the steps below and keep the outputs.

**Lone-node BRS test** (one port, nothing else on the bus, one 120 ohm
termination across CANH/CANL, run per port):

```bash
IF=can_e1m0        # then repeat everything with can_e1m1
ip link set $IF down
ip link set $IF type can bitrate 500000 dbitrate 2000000 fd on
ip link set $IF up
ip -d link show $IF        # record: brp, dbrp, dsample-point, "tdc-mode auto tdco N"
candump -e $IF &           # error frames decoded on the console
cansend $IF 123#11         # 1. classic: expect ERROR-PASSIVE, TEC stops at 128
ip -d -s link show $IF     # state ERROR-PASSIVE; restart-ms 0; note bus-error count
ip link set $IF down; ip link set $IF up
cansend $IF 123##1.11      # 2. FD + BRS: watch state, then repeat once per variant below
ip -d -s link show $IF     # record state, tdcv, berr-counter
```

Variants (down / reconfigure / up between each; record the state after one
BRS frame): (a) `dbitrate 1000000`, (b) `dbitrate 2000000 tdc-mode off`,
(c) `dbitrate 2000000 tdc-mode manual tdco 30 tdcv 12`, (d) `dbitrate
2000000 dsample-point 0.7`, (e) `cansend $IF 123##0.11` (FD format, no
rate switch).

Reading the result:

- Only BRS frames fail, and (a) or (e) pass: the fault is in the
  data-phase path. If `tdcv` reads 0 or a value near or above one bit
  (40 clocks), the measured delay is wrong; if (b)/(c) change the outcome
  the TDC setting matters and `tdco` needs adjusting.
- (e) also fails: FD format itself, not the rate switch; look at the error
  frame location bits in `candump -e`.
- Nothing changes with TDC mode or sample point, and the error frames say
  bit error at a fixed bit position: suspect the pad path (TXD/RXD pin
  drive, slew or input filter) or the transceiver mode pins, not the
  controller timing.
- Both ports fail identically: controller/pinctrl. Only one fails: that
  transceiver or its board wiring.

**Two-node test** (the two on-module ports cabled to each other): wire
`can_e1m0` CANH to `can_e1m1` CANH and CANL to CANL, with a 120 ohm
resistor across CANH/CANL at each end of the cable, so the bus has two
terminations. Configure both ports identically (same `bitrate`,
`dbitrate`, `fd on`), bring both up, then:

```bash
candump -e can_e1m1 &
cansend can_e1m0 123##1.11                 # single BRS frame, now ACKed
ip -d -s link show can_e1m0                # ERROR-ACTIVE, TX packets 1, no bus errors
cangen can_e1m0 -f -b -g 10 -I 123 -L 8 -n 1000
ip -s -d link show can_e1m0; ip -s -d link show can_e1m1   # error counters stay 0
```

Then raise `dbitrate` to 4000000 and 5000000 (TDC is mandatory there; read
`tdcv` in `ip -d link show`) and repeat `cangen`. If the two-node test is
clean at 2 Mbit/s but the lone-node BRS test still goes BUS-OFF, the lone
node case is an ACK-less bus-error corner to document rather than a
hardware fault; if the two-node test also errors, use the variant table
above on the two-node bus.

### Hand-building the kernel (outside bitbake)

The bitbake kernel banner is branded automatically. A **manual** kernel
build does **not** source the recipe, so export the same identity to
avoid leaking your own `user@host` into the banner:

<!-- cross-platform-lint:ignore -->
The Linux kernel's own build system is GNU `make` (there is no
`west`/`cmake` substitute for it) and this whole page's Prerequisites
(§1) already scope the entire Yocto/kernel-build flow to a Linux host
or WSL2 Ubuntu -- this is not a Windows-native tutorial to begin with.
```bash
export KBUILD_BUILD_USER=alp KBUILD_BUILD_HOST=alp-sdk
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= -j"$(nproc)" Image
```
<!-- cross-platform-lint:resume -->

## CA55 CPU frequency cap (1.7 GHz default, 1.8 GHz opt-in)

The image ships the conservative Renesas default: the CA55 cluster tops out
at **1.7 GHz @ 0.9 V**. The RZ/V2N silicon is datasheet-rated to **1.8 GHz @
0.9 V** (same rail — the only difference is a ~6 % clock bump, not a higher
voltage), validated on E1M-V2M101 silicon (5 min 4-core soak: no miscompute,
no throttle, 56 °C peak). It is left opt-in because 1.8 GHz is the datasheet
ceiling, so per-unit timing (Fmax) margin is thinner there.

To raise the cap to 1.8 GHz, flip one line in the SoM dtsi
(`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`):

```c
#define ALP_CA55_1P8GHZ 1   /* 0 = 1.7 GHz (default), 1 = 1.8 GHz */
```

…or pass it to the kernel dtb build without editing the file
(`-DALP_CA55_1P8GHZ=1`). The change is SoM-level, so it applies to all six
V2N-family SKUs. Validate your own silicon + thermals before enabling it
fleet-wide.

## Kernel FIT signing (opt-in scaffolding)

By default U-Boot loads a **raw** `Image` + dtb from `/boot` and boots them
with no integrity check. An opt-in builds the kernel instead as a **signed
`fitImage`** — kernel + dtb in one FIT with an **RSA-2048 / SHA-256**
signature over the configuration (so the kernel and its dtb are bound
together). Enable it in `conf/local.conf`:

```bash
ALP_FIT_SIGNED = "1"
require conf/include/alp-fit-signing.inc
```

When off (the default) the build is unchanged. When on, the kernel deploys
`fitImage` signed with a **generated dev key** under `build/alp-fit-keys/`
(gitignored build dir; dev-only — never ship it).

> **This is build-side scaffolding only — it is not yet enforced.** Making
> U-Boot *verify* the FIT before boot (`CONFIG_FIT_SIGNATURE` + the
> production public key in the FIP's U-Boot dtb + `bootcmd` → `bootm`) means
> rebuilding and reflashing the bootloader, and choosing the production key
> custody — a separate, brick-class step tracked in `alp-sdk-internal`. The
> dev-key fitImage here lets you exercise the signed-image build path now.
>
> **Caveat — not bootable as-is.** With the flag on the kernel deploys
> **only** the signed `fitImage` (the raw `Image` is no longer produced), and
> the current (phase-1) bootloader still `ext4load`s `/boot/Image` + `booti`s
> it — it does **not** `bootm` a FIT. So the flag validates the signed-*build*
> path, not a bootable board, and the §4 "Fast dev iteration" copy-`Image`
> recipe does not apply while it is on. Booting the FIT comes with the
> phase-2 U-Boot work.

## Notes

- **GigaDevice xSPI NOR (some SKUs).** Some production E1M V2N-family
  modules carry a GigaDevice LX-family octal xSPI NOR in the "NOR flash
  (variant per SKU)" slot — see [`soms/v2n.md`](soms/v2n.md).
  The production bootloader build enables it via
  `meta-alp-sdk/recipes-bsp/u-boot/u-boot/gigadevice-xspi.cfg`
  (`CONFIG_SPI_FLASH_GIGADEVICE`); U-Boot's `sf probe` then detects it
  and reports the correct capacity (bench-proven). **Known
  limit:** this U-Boot's Renesas xSPI driver fails reads that cross the
  16 MiB boundary (bench-observed `Read: ERROR 1` at `0xFFFF00+0x200`)
  — boot content must stay below 16 MiB. Writes above 16 MiB are
  untested.
- Audio is currently **disabled** in the DT (no DA7212 on the carrier);
  it returns once the TAS2563 routing lands (see the integration doc,
  gap 3).
- **Validation:** `core-image-minimal` baked clean on WSL (BSP v6.30,
  bitbake-layers) 2026-05-26 — DT patches apply, carrier dtb + `.wic.gz`
  produced.  A `drpai`-OFF `alp-image-edge` bake has since completed too
  — see [`docs/bring-up-drpai-v2n.md`](bring-up-drpai-v2n.md)'s status
  banner for the task count and artefact.  On-bench boot and a
  `drpai`-enabled bake are the remaining gates.
