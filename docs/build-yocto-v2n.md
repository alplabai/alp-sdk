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
banner say `Alp SDK 6.30`) instead of the upstream
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

## 4. Deploy the rootfs

The bootloader's `bootcmd` (rzv2n-dev config + the ALP 0002 patch)
loads `Image` + `boot/r9a09g056n44-dev.dtb` from the ext4 rootfs
`/boot`, auto-detecting the boot medium **per boot**: if an SD card is
present, root = `/dev/mmcblk2p2`, otherwise eMMC `/dev/mmcblk0p2`
(`ALP_BOOT_DEVICE ?= "emmc"` names the provisioning default, not a
build split). The kernel cmdline is rebuilt by the ALP override with
`console=ttySC0,115200` pinned; dev builds keep `earlycon`.

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

### Hand-building the kernel (outside bitbake)

The bitbake kernel banner is branded automatically. A **manual** kernel
build does **not** source the recipe, so export the same identity to
avoid leaking your own `user@host` into the banner:

```bash
export KBUILD_BUILD_USER=alp KBUILD_BUILD_HOST=alp-sdk
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= -j"$(nproc)" Image
```

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
(`-DALP_CA55_1P8GHZ=1`). The change is SoM-level, so it applies to all four
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

- Audio is currently **disabled** in the DT (no DA7212 on the carrier);
  it returns once the TAS2563 routing lands (see the integration doc,
  gap 3).
- **Validation:** `core-image-minimal` baked clean on WSL (BSP v6.30,
  bitbake-layers) 2026-05-26 — DT patches apply, carrier dtb + `.wic.gz`
  produced.  A full `alp-image-edge` bake + on-bench boot are the
  remaining gates.
