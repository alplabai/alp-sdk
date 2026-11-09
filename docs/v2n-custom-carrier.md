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
meta-mender-core, `meta-alp-sdk`), plus your layer. A known-good set is
`conf/bblayers.conf` of the V2M103 dev image; `meta-deepx-m1` is needed only
for V2M.

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

**Trap: do not base the image on `core-image-*`.** The vendor layers' `core-image-%.bbappend`
files match by recipe name and do not reach `alp-image-*`; the Mali/Weston
wiring is a conf-level override and does. Extending `alp-image-base` (or
`alp-image-edge`) avoids the problem; see the `meta-alp-sdk` README.

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
- V2N-M1: never drive PA6 (`M1_RESET`), P80 or P95 (PCIe mux PD / SEL); host
  firmware owns them ([bring-up-v2n-m1.md](bring-up-v2n-m1.md)).
- Keep the SoM management buses reachable: I2C0 (P30/P31) carries the identity
  EEPROM, I2C8 (P06/P07) carries the GD32 bridge and the DA9292 at 0x1E, which
  U-Boot programs. Leave both `okay`, with pull-ups, and bind no Linux driver
  to 0x1E.
- Stay off the pads the CM33 uses (the GD32 supervisor link on P76/P77/P96/P97;
  see [rzv2n-m33-secure-boot.md](rzv2n-m33-secure-boot.md)). The
  ownership dtsi is generated from the core-ownership metadata; do not edit it
  by hand.
- P90-P92, P2x and PBx are not 3.3 V tolerant: a carrier that puts 3.3 V on
  them needs a level shifter declared in its `board.yaml` `pad_levels:`, and
  `scripts/check_pad_voltage.py` must pass.

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
  dtb content is yours. This is the same mechanism `ALP_CAMERA_CAM0` uses.
- **Custom dtb name.** Set `KERNEL_DEVICETREE = "renesas/<name>.dtb"` for your
  MACHINE and change U-Boot: a `u-boot_%.bbappend` patch for
  the family-to-dtb table in `include/configs/rzv2n-dev.h` (provisioned
  units) plus `CONFIG_ALP_E1M_FDTFILE` through a `.cfg` (blank EEPROM). This
  carries a U-Boot patch for every release; prefer the first way unless the
  stock name is a problem.

## Verification

Layer parse and image build run on the build host; boot on a carrier needs
the carrier. The template was checked with `bitbake -p` and the dtb compile of
the example carrier (see the PR for the build log).
