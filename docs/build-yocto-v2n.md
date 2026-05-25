# Building the V2N Linux image (Yocto / kas)

How a customer builds and deploys the **kernel + root filesystem** for
the E1M-V2N101 / E1M-V2N102 SoM on the E1M-X-EVK (or a pin-compatible
custom carrier).

> **Bootloader is production-flashed by ALP.** Every SoM ships with BL2
> (alp LPDDR4X DDR init) + FIP already on the on-module xSPI, so it
> reaches U-Boot on first power-on. Your normal flow **never rebuilds
> the bootloader** — you build only kernel+rootfs below. Bootloader
> rebuild/recovery lives in `alp-sdk-internal` (see
> [`e1m-x-v2n-sdk-integration.md`](e1m-x-v2n-sdk-integration.md)).

## 1. Prerequisites

- Linux host (or WSL2 Ubuntu 22.04), ~60 GB free, 8+ GB RAM.
- `kas` (`pip install kas`) and the usual Yocto host deps
  (`gawk wget git diffstat unzip build-essential chrpath cpio …`).
- **The Renesas RZ/V2N AI-SDK BSP** (license-gated). Download the
  release that matches this SDK — **AI SDK 6.30 (`RTK0EF0189F06300SJ`),
  linux-renesas 6.1-cip43** — from your Renesas account and extract it.
  This carries `meta-renesas` + `meta-rz-features/*` and is **not**
  redistributable on public git, so kas references it from a path /
  internal mirror rather than cloning it.

## 2. Point kas at the BSP

Edit [`../kas/e1m-v2n.yml`](../kas/e1m-v2n.yml):

- Set the `meta-renesas` repo to your extracted BSP (`path:` for a
  local checkout, or your internal git mirror + `commit:`).
- Pin the `commit:` SHAs on the public layers (poky / meta-openembedded
  / meta-ros) to the revisions your BSP release was validated against.

## 3. Build

```bash
kas build kas/e1m-v2n.yml          # MACHINE=e1m-v2n101-a55, target alp-image-edge
# (V2N102: set machine: e1m-v2n102-a55 in the kas file or override)
```

Output (under `build/tmp/deploy/images/e1m-v2n101-a55/`):
- `alp-image-edge-*.wic[.gz]` — full SD/eMMC image (bootloader excluded;
  it's already on xSPI).
- `Image` + `renesas/r9a09g056n48-rzv2n-evk.dtb` — kernel + the **carrier
  dtb** (the meta-alp-sdk DT patches 0006–0013 are applied here, so this
  dtb is the e1m-x carrier dtb the shipped bootloader loads).

## 4. Deploy the rootfs

The bootloader's `bootcmd` loads `Image` + `r9a09g056n48-rzv2n-evk.dtb`
from the ext4 rootfs `/boot` (SD `mmcblk1p2` in dev, eMMC `mmcblk0p2`
in production; `ALP_BOOT_DEVICE ?= "emmc"`).

- **Full image:** write the `.wic` to the target device (eMMC via
  USB-gadget/`dd`, or SD via your host).
- **Fast dev iteration** (kernel/dtb only): copy `Image` +
  `r9a09g056n48-rzv2n-evk.dtb` into the running rootfs `/boot` over the
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
- This build path is **scaffolded, not yet CI-validated** — the kas
  manifest + bbappends need one green bitbake pass; flagged inline.
