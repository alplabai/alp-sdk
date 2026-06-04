# Building & deploying the V2N Linux image (Yocto)

How a customer builds and deploys the **kernel + root filesystem** for
the E1M-V2N101 / E1M-V2N102 SoM on the E1M-X-EVK (or a pin-compatible
custom carrier).  The build itself is the `bitbake-layers` flow in
[`../meta-alp-sdk/README.md`](../meta-alp-sdk/README.md); this page adds
the V2N-specific BSP, deploy, and on-board verification detail.

> **Bootloader is production-flashed by ALP.** Every SoM ships with BL2
> (alp LPDDR4X DDR init) + FIP already on the on-module xSPI, so it
> reaches U-Boot on first power-on. Your normal flow **never rebuilds
> the bootloader** — you build only kernel+rootfs below. Bootloader
> rebuild/recovery lives in `alp-sdk-internal` (see
> [`e1m-x-v2n-sdk-integration.md`](e1m-x-v2n-sdk-integration.md)).

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
# (V2N102: MACHINE=e1m-v2n102-a55;  V2N-M1: e1m-v2m101-a55 / e1m-v2m102-a55)
```

Output (under `build/tmp/deploy/images/e1m-v2n101-a55/`):
- `alp-image-edge-*.wic[.gz]` — full SD/eMMC image (bootloader excluded;
  it's already on xSPI).
- `Image` + `renesas/e1m-v2n101-x-evk.dtb` — kernel + the **carrier
  dtb** (the meta-alp-sdk DT patches 0006–0013 are applied here, so this
  dtb is the e1m-x carrier dtb the shipped bootloader loads).

> **Kernel version pin:** the VLP-v5 `local.conf` template defaults to
> `PREFERRED_VERSION_linux-renesas = "6.12%"` — you **must** override this
> to `"6.1%"` (linux-renesas 6.1.141-cip43) for BSP v6.30.  Leaving it at
> the template default causes a recipe mismatch and build failure.

> **Machine fragments:** `alp-image-edge` picks up per-machine `.cfg`
> fragments from `meta-alp-sdk/recipes-kernel/linux/`.  For V2N with the
> display and audio features enabled, the active fragment list includes
> `display.cfg` + `tas2563-audio.cfg`.  To build a minimal image without
> Weston/display, remove the `alp-lvgl-dashboard`, `weston`, and
> `weston-init` packages from `IMAGE_INSTALL` in your `local.conf` and
> drop the `display.cfg` fragment from the `SRC_URI` override.

## 4. Deploy the rootfs

The bootloader's `bootcmd` loads `Image` + `e1m-v2n101-x-evk.dtb`
from the ext4 rootfs `/boot` (SD `mmcblk1p2` in dev, eMMC `mmcblk0p2`
in production; `ALP_BOOT_DEVICE ?= "emmc"`).

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
dmesg | grep -i over-current || echo none   # expect: none
i2cdetect -l                            # expect i2c-0/1/2/8 only
ethtool end0 | grep "Link detected"     # PHY attaches stmmac-N:02
```

(End-to-end link needs the MDI-reversal layout fix — see
[`errata-e1m-x-v2n.md`](errata-e1m-x-v2n.md) E1 — until the respin, a
pair-mirror cable links at 100M.)

## Notes

- Audio is currently **disabled** in the DT (no DA7212 on the carrier);
  it returns once the TAS2563 routing lands (see the integration doc,
  gap 3).
- **Validation:** `core-image-minimal` baked clean on WSL (BSP v6.30,
  bitbake-layers) 2026-05-26 — DT patches apply, carrier dtb + `.wic.gz`
  produced.  A full `alp-image-edge` bake + on-bench boot are the
  remaining gates.
