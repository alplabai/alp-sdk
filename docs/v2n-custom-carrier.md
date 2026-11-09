# Custom carrier and downstream layer for V2N / V2N-M1

How a product brings up its own carrier board on an E1M-V2N10x or
E1M-V2N-M1 (V2M10x) SoM, and how its Yocto layer sits on top of
`meta-alp-sdk`. The template layer is
[`../meta-alp-sdk/templates/meta-example-product/`](../meta-alp-sdk/templates/meta-example-product/).
The base build flow is in [build-yocto-v2n.md](build-yocto-v2n.md).

## Keep the stock MACHINE

Use `e1m-v2n10x-a55` or `e1m-v2m10x-a55` unchanged. TF-A DDR parameters, U-Boot
patches, the `rzv2n-family` override and the `wic.bmap` plumbing are all keyed
on those MACHINE names; a new MACHINE name misses every one of them. Only the
device tree changes.

## 1. Layer set and local.conf

`bblayers.conf`: the same layers as a stock build (poky, meta-oe,
meta-rz-bsp, meta-rz-distro, the meta-rz-features layers you use,
meta-mender-core, `meta-alp-sdk`), plus your layer; `meta-deepx-m1` is needed
only for V2M. The layer assembly is in section 2 of
[build-yocto-v2n.md](build-yocto-v2n.md) and in
[`../meta-alp-sdk/README.md`](../meta-alp-sdk/README.md).

```
BBLAYERS += "/path/to/meta-alp-sdk /path/to/meta-your-product"
```

`local.conf`:

```
DISTRO = "alp"                      # needs rz-vlp.conf from meta-rz-distro
MACHINE = "e1m-v2n101-a55"          # or e1m-v2m103-a55 for V2N-M1
DL_DIR = "/path/to/downloads"       # holds the Renesas packages (login-gated, not in this repo)
LICENSE_FLAGS_ACCEPTED = "commercial_gstreamer1.0-omx"   # only for packages you actually install
```

`LICENSE_FLAGS_ACCEPTED` is per package: a missing flag fails the build with an
"incompatible license" message rather than silently dropping the package.
The Renesas BSP zip, the Mali DDK and the codec package must come from your
own Renesas download; the DEEPX binaries need your DEEPX licence.

## 2. The product layer

Copy `meta-alp-sdk/templates/meta-example-product` and rename it.

- `conf/layer.conf`: `LAYERDEPENDS = "alp-sdk"`, priority above 8 so your
  files shadow the SDK's.
- `recipes-images/<product>-image.bb`: `require recipes-images/alp-image-base.bb`
  and add packages. `packagegroup-alp-multimedia` is added when
  `ALP_PRODUCT_MULTIMEDIA = "1"` and that packagegroup exists in your
  `meta-alp-sdk`.
- `recipes-kernel/linux/`: the carrier dts and dtsi.

**Trap: vendor `core-image-%.bbappend` files do not reach your image.** A
bbappend matches the exact recipe name, so an image under any other name,
including a product recipe that merely `require`s a `core-image-*.bb`, gets none
of the drpai / codecs / opencva payload those bbappends add. `alp-image-base`
(through `alp-image-common.inc`) ports that payload explicitly, so extend
`alp-image-base` or `alp-image-edge`; see the `meta-alp-sdk` README.

Build: `bitbake <product>-image`.

## 3. Carrier device tree

The stock stack is: SoC dtsi (via the SoM dtsi) -> `e1m-v2n-som.dtsi` ->
`e1m-x-evk.dtsi` (the EVK carrier) -> board dts -> `e1m-v2n-ownership.dtsi`.
A product replaces the EVK carrier dtsi with its own:

1. Copy `e1m-x-evk.dtsi` as a starting point (or start from the template's
   `example-carrier.dtsi`). Enable only what the carrier populates. Pin
   groups (`i2c0_pins`, `scif_pins`, ...) are defined in the EVK dtsi, so a
   custom carrier defines its own.
2. Board dts: include the SoM dtsi, your carrier dtsi, then (V2N-M1 only)
   `e1m-v2m-deepx.dtsi`.
3. **`#include "e1m-v2n-ownership.dtsi"` last**, so nothing can re-enable a
   peripheral the CM33 owns.
4. Install the dts and dtsi into the kernel tree from a `linux-renesas_%.bbappend`
   (`SRC_URI` plus a `do_configure:prepend` copy to
   `arch/arm64/boot/dts/renesas/`, as the template does).

### Hard rules

- Ownership dtsi last (above).
- V2N-M1: add no writer of your own for PA6 (`M1_RESET`) or P95 (PCIe mux
  SEL), and keep the P80 (mux PD) output-low gpio-hog in `e1m-v2m-deepx.dtsi`
  as shipped: it matches U-Boot, and removing it re-disables the PCIe mux about
  93 ms after link-up and loses the DX-M1
  ([bring-up-v2n-m1.md](bring-up-v2n-m1.md)).
- Keep the SoM management buses reachable: I2C0 (P30/P31) carries the identity
  EEPROM, I2C8 (P06/P07) carries the GD32 bridge and the DA9292 at 0x1E, which
  U-Boot programs. Leave both `okay`, with pull-ups, and bind no Linux driver
  to 0x1E.
- No Linux pinctrl or gpio claim on any port-9 pad, P9.5 included: it shares
  the PMC9/PM9 bytes with the CM33's P9.6/P9.7, and the non-atomic
  read-modify-write clobbers the CM33's live SCK7/CS mux.
- Stay off the pads the CM33 uses (the GD32 supervisor link on P76/P77/P96/P97;
  see [rzv2n-m33-secure-boot.md](rzv2n-m33-secure-boot.md)). The
  ownership dtsi is generated from the core-ownership metadata; do not edit it
  by hand.
- P90-P92, P2x and PBx are not 3.3 V tolerant: a carrier that puts 3.3 V on
  them needs a level shifter declared in the carrier's `pad_levels:` (in
  `metadata/boards/<carrier>.yaml`, or the project's `board.yaml`). The gate scans an alp-sdk tree:
  put the file under `metadata/boards/` of a checkout and run
  `python3 scripts/check_pad_voltage.py --root <checkout>`; it must pass.

## 4. Which dtb U-Boot loads

U-Boot does not boot an arbitrary dtb name. At every boot `alp_fdtfile`
(patch 0014) sets `fdtfile` from the EEPROM manifest family through a fixed
table (`v2n` -> `e1m-v2n101-x-evk.dtb`, `v2n-m1` -> `e1m-v2m101-x-evk.dtb`) and
`fdtfile` is not in the writable environment list. `CONFIG_ALP_E1M_FDTFILE` is
only the fallback for a blank EEPROM. A provisioned unit whose image lacks the
table's dtb stops at the prompt rather than load another SoM's tree.

Two ways to deal with that:

- **Shadow the stock basename (template default).** Ship your board dts under
  the stock file name (`e1m-v2n101-x-evk.dts` / `e1m-v2m101-x-evk.dts`) in a
  layer with higher priority. `KERNEL_DEVICETREE` and U-Boot stay stock; the
  dtb content is yours. Leave `ALP_CAMERA_CAM0` unset with a shadowed
  dts: its fragment references EVK-dtsi labels that your carrier dtsi lacks, and
  dtc fails.
- **Custom dtb name.** Set `KERNEL_DEVICETREE = "renesas/<name>.dtb"` for your
  MACHINE and change U-Boot: a `u-boot_%.bbappend` patch for
  the family-to-dtb table in `include/configs/rzv2n-dev.h` (provisioned
  units) plus `CONFIG_ALP_E1M_FDTFILE` through a `.cfg` (blank EEPROM). This
  carries a U-Boot patch for every release; prefer the first way unless the
  stock name is a problem.

## Verification

Layer parse and image build run on the build host; boot on a carrier needs
the carrier. Coverage: `bitbake -p` and the dtb compile; `example-product-image
-c rootfs` for `e1m-v2n101-a55` (RC=0); `linux-renesas -c compile` and
`example-product-image -c rootfs` for `e1m-v2m103-a55` (RC=0). Not booted.

## Caveats

- No Vulkan: the Mali stack ships GLES / EGL only
  ([v2n-mali-gpu.md](v2n-mali-gpu.md)).
- No hardware-accelerated ffmpeg: the hardware H.264/H.265 codec is reached
  only through GStreamer OMX (`gstreamer1.0-omx`, flag
  `commercial_gstreamer1.0-omx`), which the opt-in multimedia packagegroup
  adds ([v2n-multimedia.md](v2n-multimedia.md)). Anything that goes through
  ffmpeg runs on the CPU.

## Brcmfmac board name

Keep `alp,e1m-v2n101-x-evk` / `alp,e1m-v2m101-x-evk` first in the root
`compatible` (the template does): `cyw-fmac-firmware` symlinks the board-named
brcmfmac files by those strings (`CYW_BOARD_NAMES`), so a different first
string makes the on-module WLAN firmware lookup fail at every boot before it
falls back to the generic name. Extend `CYW_BOARD_NAMES` if you must change it.
