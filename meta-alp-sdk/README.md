# meta-alp-sdk

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Recipes parse cleanly
> against the meta-renesas v3.1 (rzv2n) BSP but no full image
> bake has been executed.  Real bring-up gates on v0.7 V2N HiL.

Yocto layer that packages the **ALP SDK** runtime + reference
ROS 2 nodes for V2N + V2N-M1 Linux images.

## What it contains

```
meta-alp-sdk/
├── conf/
│   ├── layer.conf                       # Yocto layer metadata.
│   └── machine/
│       ├── e1m-v2n101.conf              # V2N base SoM machine config.
│       └── e1m-v2m101.conf              # V2N-M1 (V2N + DEEPX) machine config.
├── recipes-core/
│   └── alp-sdk/
│       └── alp-sdk_0.5.bb               # Builds + installs libalp_sdk.so + headers.
├── recipes-ros/
│   └── alp-perception/
│       └── alp-perception_0.5.bb        # Builds the examples/v2n/v2n-m1-ros-perception node.
├── recipes-deepx/
│   └── dx-rt/
│       └── dx-rt_2.4.bb                 # Pins the DEEPX runtime (vendor-licensed).
├── recipes-images/
│   └── alp-image-edge.bb                # Reference image: ROS 2 + alp-sdk + DEEPX.
└── README.md                            # this file
```

## How customers consume it

```bash
# In bblayers.conf of a poky build dir targeting the V2N family:
BBLAYERS += " ${TOPDIR}/../meta-renesas              \
             ${TOPDIR}/../meta-renesas-rz-features  \
             ${TOPDIR}/../meta-openembedded/meta-oe \
             ${TOPDIR}/../meta-ros/meta-ros2-humble \
             ${TOPDIR}/../alp-sdk/meta-alp-sdk "

# In local.conf:
MACHINE = "e1m-v2m101"     # or e1m-v2n101 for the no-DEEPX variant.
DISTRO  = "poky"

# Build the reference edge image:
bitbake alp-image-edge
```

The resulting `core-image-*-e1m-v2m101.wic` flashes onto the
V2N-M1 SoM's eMMC via the standard Renesas flash-writer tooling.

## Licence

Apache-2.0 (umbrella).  Vendor-licensed components (`dx-rt`,
`drp-ai-tvm`) follow their upstream licences and are flagged as
such in the matching recipes' `LICENSE` field.

## What's deferred

- The `dx-rt_*.bb` recipe is a skeleton -- needs the DEEPX SDK
  signed-licence acknowledgement once Hakan's OTA-server work
  closes the legal review (per
  [`docs/vendor-partnerships.md`](../docs/vendor-partnerships.md)
  §C.31).
- `drp-ai-tvm_*.bb` recipe (the Renesas DRP-AI runtime) lands
  alongside meta-renesas-rz-features.
- `alp-image-edge.bb`'s minimal package set is documentary; the
  v1.0 sysbuild matrix in `docs/test-plan.md` adds the OTA
  Mender layer + the BLE provisioning layer + the
  certificate-pinning post-install hook.

## Verification status

`[UNTESTED]`.  Recipe metadata parses but no full image bake
has been done yet -- the recipes pin tags + checksums from the
maintainer's local notes that need re-validation against
upstream releases.  v0.7 V2N HiL is the verification gate.
