# Bench bring-up — E1M-N93 (NXP i.MX 93)

Step-by-step procedure for bringing a freshly-assembled E1M-N93
module up on the bench.  Assumes you have an `E1M-EVK` board
(or pin-compatible custom), a J-Link or ST-Link debug probe, a
USB-UART adapter, and a 1 Gb Ethernet link partner.

> Peer docs: [`bring-up-aen.md`](bring-up-aen.md),
> [`bring-up-v2n.md`](bring-up-v2n.md),
> [`bring-up-v2n-m1.md`](bring-up-v2n-m1.md).  This guide covers
> the **N93 family** (currently only `E1M-NX9101`, MPN TBD pending
> final hardware config).

> **Yocto-first family.**  Unlike the AEN family (Zephyr / bare-
> metal), the N93 family targets **Yocto Linux** as its primary
> OS.  The Cortex-M33 side runs Zephyr as a co-processor; the
> Cortex-A55s run a Linux kernel built from `meta-alp-sdk`.  This
> bring-up walks the A55 boot first; M33 attach lands in §5.

## 0. Pre-flight

Inventory check before powering anything:

* Module populated:
  - **NXP i.MX 93** SoC (2x Cortex-A55 @ 1.7 GHz + Cortex-M33
    @ 250 MHz + Ethos-U65 NPU).  Specific variant per the SKU's
    [`metadata/e1m_modules/E1M-NX9101.yaml`](../metadata/e1m_modules/E1M-NX9101.yaml).
  - **PCA9450** primary PMIC.
  - **PCAL9538** GPIO expander on BRD_I2C.
  - **24C128** EEPROM at `0x50` with the ALP manifest.
  - **DP83825I** Ethernet PHY (single MAC routed; ETH1 lives on
    E1M-X form factor only).
  - **OPTIGA Trust M** secure element.
* Board populated: E1M-edge passthroughs + 5 V power input +
  JTAG/SWD header + USB-UART for console + microSD card slot
  (Yocto images boot from SD by default during bring-up).

## 1. First-power smoke test

1. Insert a microSD card flashed with `alp-image-edge`
   built from `meta-alp-sdk` (see §3 below for the build).
2. Connect a current-limited bench supply (1.5 A limit) to V_IN.
3. Power on; watch the supply.  Steady-state current with
   Yocto idle at the login prompt: **~280..400 mA**.
4. Probe the on-module 3.3 V rail at the test-point.  Within
   spec: 3.30 V ±2 %.
5. Probe V_CORE (A55 core rail, ~0.85 V at low load).  Within
   spec: 0.85 V ±3 %.

If V_CORE is missing the PCA9450 didn't release the core rail.
Check the PMIC's `INT_STATUS` register over BRD_I2C; the
[`docs/troubleshooting.md`](troubleshooting.md) entry for
"V_CORE absent on N93" covers the typical causes.

## 2. Console + first boot

1. Wire USB-UART to UART1 on the board (silkscreen
   `USB_UART_TXD` / `_RXD`).  Standard 115200 8N1.
2. Open a terminal.
3. Insert the microSD with the `alp-image-edge` image and power on.

Expected output within ~5 s:

```
U-Boot 2025.04 (...)
CPU:   NXP i.MX93 rev1.0 ...
DRAM:  2 GiB ...
...
Booting Linux on physical CPU 0x0000000000 [0x412fd050]
Linux version 6.6.x (alp@alp-build)
...
Welcome to ALP Yocto (kirkstone, branch ...)
e1m-nx9101-a55 login:
```

Login as `root` (no password on the bring-up image).  If you
don't see U-Boot output: wrong UART selected, wrong baud, or
the boot ROM didn't find a valid bootloader on the SD.  The
boot ROM probes SD first, then eMMC; missing both means a
bricked SoC fuse (rare).

## 3. Build a Yocto image (the long pole, 1st time only)

```bash
# In a separate Yocto workspace.  Don't mix with the alp-sdk
# Zephyr workspace -- Yocto wants its own tree.
mkdir -p ~/work/yocto-alp && cd ~/work/yocto-alp
git clone https://git.yoctoproject.org/poky -b kirkstone
cd poky
# meta-imx depends on meta-freescale; clone both.
git clone https://github.com/Freescale/meta-freescale -b kirkstone
git clone https://github.com/nxp-imx/meta-imx -b kirkstone
git clone https://github.com/alplabai/alp-sdk    # then symlink meta-alp-sdk
ln -s alp-sdk/meta-alp-sdk ./meta-alp-sdk

source oe-init-build-env build-n93

# Add the layers (meta-freescale first; meta-imx sublayers depend on it):
bitbake-layers add-layer ../meta-freescale
bitbake-layers add-layer ../meta-imx/meta-imx-bsp
bitbake-layers add-layer ../meta-imx/meta-imx-ml
bitbake-layers add-layer ../meta-alp-sdk

# In conf/local.conf -- keep the meta-imx default DISTRO (fsl-imx-*).
# meta-alp-sdk's `alp` distro (conf/distro/alp.conf) is NOT selectable here:
# it requires Renesas rz-vlp and is RZ/V2N-only. On i.MX 93 use only the
# Mender include (conf/distro/include/mender.inc) layered onto fsl-imx-*.
echo 'MACHINE = "e1m-nx9101-a55"' >> conf/local.conf

# Build the alp-sdk edge image (alp-image-edge), which carries the
# alp-sdk payload -- chips, runtime, EdgeAI demos.  A plain
# core-image-minimal builds, but ships NONE of the alp-sdk payload.
# Build (45-90 min the first time; warm cache: 5-10 min).
bitbake alp-image-edge
```

The output image lands at
`tmp/deploy/images/e1m-nx9101-a55/alp-image-edge-e1m-nx9101-a55.wic`.
Flash to microSD:

```bash
sudo dd if=tmp/deploy/images/e1m-nx9101-a55/alp-image-edge-e1m-nx9101-a55.wic \
        of=/dev/sdX bs=4M conv=fsync
```

## 4. EEPROM manifest verify

From the Linux shell after first boot:

```bash
i2cdetect -y 1                 # confirm 0x50 ACKs on BRD_I2C
i2cdump  -y 1 0x50 b            # 128-byte hexdump
```

Decode against
[`include/alp/hw_info.h`](../include/alp/hw_info.h) -- same
layout as the AEN bring-up.  If unprogrammed, use
[`scripts/program_eeprom.py`](../scripts/program_eeprom.py)
against the SKU's preset (`E1M-NX9101.yaml`).

The runtime API (`alp_hw_info_t`) reads the same manifest from
userspace via `/sys/bus/i2c/devices/...` -- see
[`<alp/hw_info.h>`](../include/alp/hw_info.h).

## 5. M33 co-processor attach

The Cortex-M33 runs as a co-processor under the A55-side Linux.
Bring-up flow:

1. The A55 Linux kernel exposes `/dev/remoteproc0`.
2. Drop the Zephyr-built M33 firmware at
   `/lib/firmware/m33_zephyr.elf` (built via
   `west alp-build -b imx93_evk_m33 examples/peripheral-io/gpio-button-led`).
3. Boot the M33:

   ```bash
   echo m33_zephyr.elf > /sys/class/remoteproc/remoteproc0/firmware
   echo start          > /sys/class/remoteproc/remoteproc0/state
   ```

4. Confirm the M33 banner appears on UART2 (separate from the
   A55 console).

The mproc IPC (mailbox / shmem / hwsem) between A55 + M33 uses
the i.MX 93 MU (Messaging Unit).  See
[`<alp/mproc.h>`](../include/alp/mproc.h) for the portable API
and the Yocto-side backend `src/yocto/mproc_yocto.c` (lands in
v0.4-final).

## 6. Peripheral sanity checks

### 6.1 BRD_I2C

| Slave | 7-bit addr | What |
|-------|------------|------|
| PCAL9538 | `0x70` | GPIO expander |
| 24C128 | `0x50` | EEPROM |
| OPTIGA TM | `0x30` | Secure element |
| TMP112 | `0x48` | Thermometer (optional) |

Same convention as AEN.

### 6.2 Ethernet

The DP83825I is wired to the i.MX 93 ENET MAC.  Once Yocto's
`systemd-networkd` (or the `networkmanager` we ship) brings the
interface up, `ip link show eth0` reports link-up at 100/full
within ~500 ms of cable insert.

```bash
ethtool eth0       # vendor / speed / duplex
ping -c 4 8.8.8.8  # reach the world
```

### 6.3 Ethos-U65 NPU

```bash
modprobe ethosu              # NPU kernel driver
ls /dev/ethos-u*             # expect /dev/ethos-u0
```

The Vela-compiled MobileNet demo (Yocto image bundles it under
`/opt/alp/demos/mobilenet_vela.tflite`) exercises end-to-end:

```bash
/opt/alp/demos/run_mobilenet --backend ethos-u --image test.jpg
```

Output: top-5 class predictions matching the reference within
floating-point tolerance.

## 7. Going to production

Once §1..6 pass:

1. Use [`scripts/program_eeprom.py`](../scripts/program_eeprom.py)
   to write the production manifest.
2. Build a signed image with Mender support enabled in
   `meta-alp-sdk` (see [`docs/ota.md`](ota.md) and
   [`meta-alp-sdk/conf/distro/include/mender.inc`](../meta-alp-sdk/conf/distro/include/mender.inc)).
3. Flash to eMMC for production deployment (microSD is bring-up
   only).

## 8. Troubleshooting

* **No U-Boot output** -- check the BOOT_MODE strap on the
  board; i.MX 93 boot ROM probes SD first only if BOOT_MODE
  selects "external boot".
* **U-Boot OK but kernel panics** -- usually a DTB
  incompatibility.  Confirm the kernel + DTB came from the
  same `meta-alp-sdk` build.
* **PHY links but no IP** -- `systemd-networkd` config (`/etc/
  systemd/network/`) might not be installed; fallback to manual
  `ip link set eth0 up; udhcpc -i eth0`.
* **`/dev/ethos-u0` missing** -- the kernel module didn't load.
  Check `dmesg | grep ethos` for the load-time error; usually
  a DTB binding mismatch.

More patterns in [`docs/troubleshooting.md`](troubleshooting.md);
ask on [community.alplab.ai](https://community.alplab.ai/) for
anything not covered.

## See also

- [`docs/soms/imx93.md`](soms/imx93.md) -- N93 one-pager.
- [`docs/ota.md`](ota.md) -- Mender OTA setup for the Yocto side.
- [`meta-alp-sdk/`](../meta-alp-sdk/) -- the Yocto layer
  consumed in §3.
