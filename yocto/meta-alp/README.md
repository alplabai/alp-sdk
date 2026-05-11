# meta-alp — Yocto BSP layer for the ALP SDK

This layer packages the ALP SDK + its chip drivers as Yocto
recipes so V2N / V2N-M1 / i.MX 93 builds can pull `alp-sdk`
from a deterministic source, plus carries machine configs for
the E1M-X V2N, E1M-X V2N-M1, and E1M-N93 SoMs.

## Status

**v0.3 scaffolding (parses but does not yet build).**

Recipe shells + machine configs are in place; the actual build
glue (real `do_compile` against the Yocto cross-toolchain) lands
in v0.4 alongside the "Yocto first-class" milestone in
[`VERSIONS.md`](../../VERSIONS.md).  Treat the recipes here as
`bitbake -p`-clean shells that consumers can opt into now and
have ready when v0.4 fills in the implementation.

## Layout

```
yocto/meta-alp/
├── conf/
│   ├── layer.conf                       # BBPATH + LAYERSERIES_COMPAT + deps
│   └── machine/
│       ├── e1m-x-v2n.conf               # E1M-X module, Renesas RZ/V2N
│       ├── e1m-x-v2n-m1.conf            # V2N + DEEPX DX-M1 NPU
│       └── e1m-n93.conf                 # E1M module, NXP i.MX 93
├── recipes-alp/
│   ├── alp-sdk/
│   │   ├── alp-sdk_git.bb               # umbrella, v0.1 carryover
│   │   └── alp-sdk-runtime_git.bb       # libalp_sdk.so + headers
│   ├── alp-chips/
│   │   └── alp-chips_git.bb             # libalp_chips.a + Doxygen'd headers
│   ├── alp-studio-codegen/
│   │   └── alp-studio-codegen_git.bb    # CLI codegen helper (opt-in)
│   └── alp-examples/
│       └── alp-edgeai_git.bb            # EdgeAI reference app
└── README.md                            # This file
```

## Required compatible layers

`meta-alp` rides on top of vendor BSP layers.  Pick the one(s)
matching the SoM family you're building for.

### Renesas V2N / V2N-M1 path

Renesas distributes the **RZ/V2N AI SDK** (currently **v7.10**,
documentation at <https://renesas-rz.github.io/rzv_ai_sdk/7.10/>)
as a single downloadable package on the *My Renesas* portal --
free signup, no NDA for the standard build.  The package ID is
**`RTK0EF0045Z94001AZJ-v1.0.3.zip`** (per the
[*RZ/V2N Group Handbook*](https://www.renesas.com/en/document/oth/rzv2n-group-handbook),
item #18).

The zip contains a `rzv2n_ai-sdk_yocto_recipe_*.tar.gz` tarball
that, when extracted, gives you the following pre-arranged set
of meta-layers (each is a git checkout pinned to the v7.10 release;
the tarball model is canonical because V2N silicon support may not
yet be on the corresponding `meta-renesas` upstream branch):

| Layer                                          | Source repo                                                                    | Role                                                       |
|------------------------------------------------|--------------------------------------------------------------------------------|------------------------------------------------------------|
| `poky`                                         | <https://git.yoctoproject.org/poky>                                            | Yocto base.                                                |
| `meta-arm`                                     | <https://git.yoctoproject.org/meta-arm>                                        | ARM-specific recipes.                                      |
| `meta-openembedded`                            | <https://github.com/openembedded/meta-openembedded>                            | Standard OE recipe collection.                             |
| `meta-renesas`                                 | <https://github.com/renesas-rz/meta-renesas>                                   | Renesas RZ base BSP -- provides `rzv2n-evk` MACHINE.       |
| `meta-rz-features/meta-rz-graphics`            | (bundled in `meta-rz-features` under Renesas)                                  | Mali GPU drivers + Weston compositor wiring.               |
| `meta-rz-features/meta-rz-drpai`               | (bundled in `meta-rz-features`)                                                | **DRP-AI userspace runtime + headers.**                    |
| `meta-rz-features/meta-rz-opencva`             | (bundled in `meta-rz-features`)                                                | OpenCV acceleration via DRP.                               |
| `meta-rz-features/meta-rz-codecs`              | (bundled in `meta-rz-features`)                                                | Hardware video codec recipes.                              |
| `meta-econsys`                                 | (bundled; vendored from e-con Systems)                                         | Camera drivers.  Contact e-con Systems for `e-CAM22_CURZH` patch. |

DRP-AI is fully covered by `meta-rz-drpai` -- there's no separate
post-build tarball install for the runtime headers, and no NDA gate
on it.  Only the e-con Systems MIPI camera patch requires a
manufacturer contact, and it's optional (only needed if you
populate `e-CAM22_CURZH` on the carrier).

Yocto release: **Scarthgap (5.0.11)**.  GCC 13.
Toolchain SDK: `bitbake core-image-weston -c populate_sdk` against
the matching MACHINE.

### NXP i.MX 93 path

The N93 path tracks NXP's
[`meta-imx`](https://github.com/nxp-imx/meta-imx) for the i.MX 93
base BSP plus
[`meta-freescale`](https://git.yoctoproject.org/meta-freescale) for
the broader i.MX userspace stack.  Detailed wiring lands alongside
the v0.4 i.MX 93 bring-up; the `e1m-n93.conf` MACHINE shipped today
is a parse-clean placeholder.

## Adding to a Yocto build

### V2N / V2N-M1 (via the Renesas AI SDK 7.10 tarball)

```bash
# 1. Download the BSP zip from My Renesas (free signup required):
#    https://www.renesas.com/en/products/microcontrollers-microprocessors/
#       rz-mpus/rzv2n-15tops-quad-core-vision-ai-mpu
#    File: RTK0EF0045Z94001AZJ-v1.0.3.zip
unzip RTK0EF0045Z94001AZJ-v1.0.3.zip
cd <extracted_dir>

# 2. Extract the recipe tarball; produces poky/, meta-arm/,
#    meta-openembedded/, meta-renesas/, meta-rz-features/, meta-econsys/.
tar zxvf src_setup/rzv2n_ai-sdk_yocto_recipe_*.tar.gz

# 3. Init the Yocto env (the template ships with vlp-v4-conf):
TEMPLATECONF=$PWD/meta-renesas/meta-rz-distro/conf/templates/vlp-v4-conf/ \
    source poky/oe-init-build-env build

# 4. Add the Renesas feature sublayers:
bitbake-layers add-layer ../meta-rz-features/meta-rz-graphics
bitbake-layers add-layer ../meta-rz-features/meta-rz-drpai
bitbake-layers add-layer ../meta-rz-features/meta-rz-opencva
bitbake-layers add-layer ../meta-rz-features/meta-rz-codecs
bitbake-layers add-layer ../meta-econsys

# 5. Add meta-alp on top:
git clone https://github.com/alplabai/alp-sdk ../alp-sdk
bitbake-layers add-layer ../alp-sdk/yocto/meta-alp

# 6. For V2N-M1, also add meta-deepx-m1 (DEEPX's M1 recipes):
git clone https://github.com/DEEPX-AI/meta-deepx-m1 ../meta-deepx-m1
bitbake-layers add-layer ../meta-deepx-m1

# 7. Pick the MACHINE in conf/local.conf:
MACHINE = "e1m-x-v2n"          # plain V2N
# or
MACHINE = "e1m-x-v2n-m1"       # V2N + DEEPX

# 8. Image install + build:
echo 'IMAGE_INSTALL:append = " alp-sdk-runtime alp-chips alp-edgeai"' \
    >> conf/local.conf
bitbake core-image-weston
```

### i.MX 93 (placeholder until v0.4)

```bash
MACHINE = "e1m-n93"            # i.MX 93
# meta-imx + meta-freescale wiring details land alongside the v0.4 N93 bring-up.
```

## Per-machine inference backend

| Machine          | Default backend | Why                                                 |
|------------------|-----------------|-----------------------------------------------------|
| `e1m-x-v2n`      | `drpai`         | DRP-AI3 on-chip; no external NPU.                   |
| `e1m-x-v2n-m1`   | `deepx`         | DEEPX DX-M1 outperforms DRP-AI for most models.     |
| `e1m-n93`        | `ethosu`        | Ethos-U65 micro-NPU shared with the M33 RT-core.    |

Override via `ALP_INFERENCE_DEFAULT_BACKEND` in `local.conf`.

### DRP-AI userspace headers

When `meta-rz-drpai` is in `bblayers.conf`, the DRP-AI userspace
runtime + headers (`<linux/drpai.h>`, the `drpai_*` ioctls) appear
in the target sysroot at standard paths.  The SDK's
`<alp/inference.h>` Yocto backend (planned for v0.4) picks them up
through the sysroot include path -- no per-app pkg-config plumbing
needed.

### Model compilation toolchain (RUHMI / DRP-AI TVM)

Models for DRP-AI compile through Renesas's RUHMI (formerly
DRP-AI TVM) toolchain on the build host -- not at image build
time.  It's a separate Apache-2.0 project at
<https://github.com/renesas-rz/rzv_drp-ai_tvm>; model authors
install it on their workstation and ship the compiled output
as a model asset.  The image build only needs the runtime side,
which is in `meta-rz-drpai`.

## Roadmap

- v0.1 — directory placeholder.
- v0.2 — recipe shells in place.
- v0.3 — machine configs + recipe fleshout for V2N / V2N-M1 / N93;
  bitbake-parses clean.  **README + machine configs rebased onto
  the actual Renesas AI SDK 7.10 BSP** (this revision).
- v0.4 — `src/yocto/` backends go real; recipes actually build.
  Image templates for vision / audio / IoT product classes ship.

## Compatibility

Targeted layer series:

- `scarthgap`  (LTS, gcc 13)  — matches Renesas AI SDK 7.10.
- `kirkstone`  (LTS, gcc 11)  — older builds; not the recommended
  path going forward.

## License

The layer itself is Apache-2.0 (see [`LICENSE`](../../LICENSE) at
repo root).  The chips and SoCs targeted here ship under their
respective vendor licenses.

## See also

- [*RZ/V2N Group Handbook*](https://www.renesas.com/en/document/oth/rzv2n-group-handbook)
  -- Renesas's master index of V2N collateral.
- [RZ/V AI SDK 7.10 docs](https://renesas-rz.github.io/rzv_ai_sdk/7.10/)
  -- Software overview + getting-started + how-to-build.
- [`vendors/deepx-dxm1/README.md`](../../vendors/deepx-dxm1/README.md)
  -- DEEPX DX-M1 integration notes (covers V2N-M1).
