# SP3 Task 3 Report: Carrier DTS — carveout (generated) + MHUv2 + remoteproc(M55-HP)

**Status:** DONE  
**Date:** 2026-06-26

---

## Step 1 — Generated carveout dtsi

Command (Windows):
```
py -3.14 scripts/alp_orchestrate.py \
  --input examples/multicore/rpmsg-aen/board.yaml \
  --emit dts-reservations \
  > meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi
```

**Note on path:** The brief specified `linux-alif/files/aen801-dts-reservations.dtsi`.
The `files/` subdirectory does NOT appear in BitBake's FILESEXTRAPATHS search path when
the prepend is `${THISDIR}/${PN}:` — BitBake only searches the top-level directory added
by the prepend.  File was placed at `linux-alif/aen801-dts-reservations.dtsi` (same
level as `e1m-aen801-evk.dts` and `e1m_dct_defines.h`).

Generated content:
```dts
/ {
    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;

        alp_default_rpmsg: alp_default_rpmsg@23c0000 {
            compatible = "shared-dma-pool";
            reg = <0x0 0x023c0000 0x0 0x00040000>;
            no-map;
            label = "alp_default_rpmsg";
        };
    };
};
```

Base 0x023c0000 = `sram0_base` (0x02000000 from `e8.json`) + 0x1c0000 offset chosen by
`alp_orchestrate.py` to place the 256 KiB carveout at the top of the 4 MiB SRAM0 bank
(not BLOCKED — no competing reservation there).

---

## Step 2 — Grounding MHU nodes from the fork DTS

Grepped `arch/arm/boot/dts/alif/ensemble/common/ensemble-ex.dtsi` in the unpacked
`linux-alif` (6.12+git, alifsemi fork):

| Signal | Node label | `reg` | IRQ (SPI) | compatible |
|--------|-----------|-------|-----------|-----------|
| M55-HP MHU0 TX | `mbox_m55_hp_mhu0_tx` | `0x1b000000 0x1000` | SPI 11 | `arm,mhuv2-tx`, `arm,primecell` |
| M55-HP MHU0 RX | `mbox_m55_hp_mhu0_rx` | `0x1b010000 0x1000` | SPI 12 | `arm,mhuv2-rx`, `arm,primecell` |

Both carry `#mbox-cells = <2>`; consumer passes `<channel ring_mode>`.

**Remoteproc:** The alifsemi/linux_alif fork (6.12+git) has **no** Alif-specific
remoteproc driver in `drivers/remoteproc/` and no binding for an Alif M55 remoteproc
node.  The fork's ensemble-ex.dtsi only has an `arm,client` test consumer for the MHU.
The `compatible = "alif,ensemble-m55-rproc"` in the carrier DTS is a placeholder, marked
`TODO(aen-memory-map)`.

---

## Step 3 — Carrier DTS edits

Appended to `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts`
(after the closing `};` of the `&pinmux` block, lines 134–176):

```dts
#include "aen801-dts-reservations.dtsi"   /* generated reserved-memory carveout */

&mbox_m55_hp_mhu0_tx {
    status = "okay";
};

&mbox_m55_hp_mhu0_rx {
    status = "okay";
};

/ {
    remoteproc_m55_hp: remoteproc-m55-hp {
        /* TODO(aen-memory-map): placeholder compatible — no Alif Linux remoteproc
         * driver exists in alifsemi/linux_alif (6.12+git) as of 2026-06-26. */
        compatible = "alif,ensemble-m55-rproc";
        mboxes = <&mbox_m55_hp_mhu0_tx 0 0>,
                 <&mbox_m55_hp_mhu0_rx 0 0>;
        mbox-names = "tx", "rx";
        memory-region = <&alp_default_rpmsg>;
        firmware-name = "alp/E1M-AEN801/m55_hp.elf";
        status = "okay";
    };
};
```

---

## Step 4 — bbappend update

In `meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend`:

- Added `file://aen801-dts-reservations.dtsi \` to `SRC_URI:append`
- Extended the `install -m 0644` line in `do_configure:prepend()` to include
  `${WORKDIR}/aen801-dts-reservations.dtsi`

---

## Step 4b — Root-cause fix to `e1m_dct_defines.h`

**Problem discovered during build:** `devkit_ex_dct_defines.h` (the file previously
`#include`d by `e1m_dct_defines.h`) is a **0-byte stub** in the `linux-alif` kernel
source tree:

```
-rw-r--r-- 1 alplab alplab  0  Jun 26 10:14
    .../ensemble/common/devkit_ex_dct_defines.h
```

The Alif `dct-kernel` Yocto class populates this file at build time via `do_dct_to_dts`.
Our recipe uses the standard `linux.inc` kernel class (no `dct-kernel`), so the stub is
never populated.  Every DCT macro the DTS references (`CPU1_STATUS`,
`MEM_STITCH_STATUS`, `MEM_HYPER_STATUS`, `MEM_HYP_STITCH_STATUS`) remained undefined,
causing `dtc` to report `e1m-aen801-evk.dts:57.11-12 syntax error` (column 11–12 =
`CP` in `CPU1_STATUS`).

**Fix:** Rewrote `e1m_dct_defines.h` to be fully self-contained — no `#include` of the
stub; all macros defined directly:

```c
#define CPU1_STATUS         "okay"         /* A32 dual-core: cpu1 online for SMP */
#define MEM_STITCH_STATUS   "disabled"     /* TODO(aen-memory-map): pending audit */
#define MEM_HYPER_STATUS    "disabled"
#define MEM_HYP_STITCH_STATUS "disabled"
#define UART2_STATUS        "disabled"
#define UART5_STATUS        "okay"         /* E1M_UART0 → Alif UART5 */
#define I2C1_STATUS         "okay"         /* E1M_I2C1 → Alif I2C1  */
#define I2C2_STATUS         "okay"         /* E1M_I2C0 → Alif I2C2  */
```

**BitBake sstate note:** A clean rebuild requires committing `e1m_dct_defines.h` first
so BitBake detects the `file://` checksum change and invalidates the `do_unpack`/
`do_configure` sstate.  Running `bitbake -c compile linux-alif` with a warm build tree
(work-shared already having the new file) works immediately; `bitbake -c cleansstate
linux-alif` before rebuild is safe after the commit.

---

## Step 5 — DTB compile + decompile verification

Build command:
```bash
export BITBAKEDIR=$SETUP/tools/bitbake
export PATH=$SETUP/tools/bitbake/bin:$PATH
export BUILDDIR=$SETUP/build
export BBPATH=$SETUP/build
export MACHINE=e1m-aen801-a32
source $SETUP/layers/openembedded-core/oe-init-build-env $SETUP/build
bitbake -c compile linux-alif    # exit 0
```

DTB: `build/tmp/work/e1m_aen801_a32-poky-linux-musleabi/linux-alif/6.12+git/`
     `linux-e1m_aen801_a32-standard-build/arch/arm/boot/dts/alif/ensemble/e1m/`
     `e1m-aen801-evk.dtb` (30476 bytes)

### decompile grep output (`dtc -I dtb -O dts`)

```
117:    reserved-memory {
123:        compatible = "shared-dma-pool";
130:        compatible = "shared-dma-pool";
136:    alp_default_rpmsg@23c0000 {
137:        compatible = "shared-dma-pool";
138:        reg = <0x00 0x23c0000 0x00 0x40000>;
140:        label = "alp_default_rpmsg";

226:    mbox-name = "arm-m55_hp-mhu0_tx";
227:    status = "okay";
228:    phandle = <0x04>;

254:    mbox-name = "arm-m55_hp-mhu0_rx";
255:    status = "okay";
256:    phandle = <0x06>;

1481:   remoteproc-m55-hp {
1482:       compatible = "alif,ensemble-m55-rproc";
1483:       mboxes = <0x04 0x00 0x00 0x06 0x00 0x00>;
1484:       mbox-names = "tx\0rx";
1485:       memory-region = <0x1d>;      /* phandle = alp_default_rpmsg */
1486:       firmware-name = "alp/E1M-AEN801/m55_hp.elf";
1487:       status = "okay";
```

All four checks pass:
- ✓ `reserved-memory` present
- ✓ `alp_default_rpmsg@23c0000`: 256 KiB (0x40000), `shared-dma-pool`, `no-map`
- ✓ MHUv2 TX + RX enabled (`status = "okay"`), phandles 0x04 / 0x06
- ✓ `remoteproc-m55-hp` referencing correct carveout phandle (0x1d) and both MHU mailboxes

---

## Step 6 — Commit

```
dts(aen): MHUv2 + remoteproc(M55-HP) + generated rpmsg carveout in the carrier DTS
```

Files committed:
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/aen801-dts-reservations.dtsi` (new — generated)
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m-aen801-evk.dts` (SP3 additions)
- `meta-alp-sdk/recipes-kernel/linux/linux-alif/e1m_dct_defines.h` (self-contained fix)
- `meta-alp-sdk/recipes-kernel/linux/linux-alif_%.bbappend` (dtsi wired into kernel build)
- `.superpowers/sdd/task-3-report.md` (this file)
