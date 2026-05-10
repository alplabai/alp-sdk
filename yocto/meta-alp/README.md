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

Two upstream BSP layers must be in your `bblayers.conf`
alongside meta-alp:

| Layer                                              | Why                                  |
|----------------------------------------------------|--------------------------------------|
| [`meta-renesas-rz`](https://github.com/renesas-rz/meta-renesas-rz) | Provides `renesas-rzv2n` base for `e1m-x-v2n` and `e1m-x-v2n-m1`. |
| [`meta-imx`](https://github.com/nxp-imx/meta-imx)  | Provides `imx93evk` base for `e1m-n93`. |

You only need the layer matching your SoC family.

## Adding to a Yocto build

```bash
git clone https://github.com/alplabai/alp-sdk
bitbake-layers add-layer alp-sdk/yocto/meta-alp

# In conf/local.conf, pick one machine:
MACHINE = "e1m-x-v2n-m1"       # V2N + DEEPX
# or
MACHINE = "e1m-n93"            # i.MX 93

# Image install
IMAGE_INSTALL:append = " alp-sdk-runtime alp-chips alp-edgeai"
```

## Per-machine inference backend

| Machine          | Default backend | Why                                                 |
|------------------|-----------------|-----------------------------------------------------|
| `e1m-x-v2n`      | `drpai`         | DRP-AI3 on-chip; no external NPU.                   |
| `e1m-x-v2n-m1`   | `deepx`         | DEEPX DX-M1 outperforms DRP-AI for most models.     |
| `e1m-n93`        | `ethosu`        | Ethos-U65 micro-NPU shared with the M33 RT-core.    |

Override via `ALP_INFERENCE_DEFAULT_BACKEND` in `local.conf`.

## Roadmap

- v0.1 — directory placeholder.
- v0.2 — recipe shells in place.
- v0.3 — **(this commit)** machine configs + recipe fleshout
  for V2N / V2N-M1 / N93; bitbake-parses clean.
- v0.4 — `src/yocto/` backends go real; recipes actually build.
  Image templates for vision / audio / IoT product classes
  ship.

## Compatibility

Targeted layer series:

- `kirkstone`   (LTS, gcc 11)
- `scarthgap`   (LTS, gcc 13)

Earlier series may work but aren't tested.

## License

The layer itself is Apache-2.0 (see [`LICENSE`](../../LICENSE) at
repo root).  The chips and SoCs targeted here ship under their
respective vendor licenses.
