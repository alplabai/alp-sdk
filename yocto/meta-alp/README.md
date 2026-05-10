# meta-alp — Yocto BSP layer for the ALP SDK

This layer packages the ALP SDK and its examples as Yocto recipes
so V2N / V2N-M1 / i.MX 93 builds can pull `alp-sdk` from a
deterministic source.

## Status

**v0.1: skeleton.**  Recipe shells exist (`alp-sdk_git.bb`,
`alp-edgeai_git.bb`) and the layer.conf advertises the layer to
bitbake; the actual build glue (`do_compile` / `do_install` /
`FILES:${PN}`) wires up in v0.4 alongside the "Yocto first-class"
milestone in [`VERSIONS.md`](../../VERSIONS.md).

## Layout

```
yocto/meta-alp/
├── conf/
│   └── layer.conf                       # BBPATH + LAYERSERIES_COMPAT
├── recipes-alp/
│   ├── alp-sdk/
│   │   └── alp-sdk_git.bb               # Builds libalp_sdk.so + headers
│   └── alp-examples/
│       └── alp-edgeai_git.bb            # Builds the EdgeAI reference app
└── README.md                            # This file
```

## Adding to a Yocto build

```bash
# In your Yocto workspace
git clone https://github.com/alplabai/alp-sdk
bitbake-layers add-layer alp-sdk/yocto/meta-alp

# In conf/local.conf
IMAGE_INSTALL:append = " alp-sdk alp-examples"
```

## Anticipated layout (v0.4 fills in)

```
yocto/meta-alp/
├── conf/
│   ├── layer.conf
│   └── machine/
│       ├── e1m-x-v2n.conf                  # MACHINE = "e1m-x-v2n"
│       └── e1m-x-v2n-m1.conf
├── recipes-alp/
│   ├── alp-sdk/
│   │   └── alp-sdk_git.bb                  # libalp_sdk.so + headers
│   ├── alp-examples/
│   │   ├── alp-edgeai_git.bb
│   │   └── alp-iot_git.bb
│   └── images/
│       ├── alp-vision-image.bb             # SDK + camera + inference
│       ├── alp-audio-image.bb              # SDK + ALSA + audio examples
│       └── alp-iot-image.bb                # SDK + Mosquitto + Paho + OTA
└── README.md
```

## Roadmap

- v0.1 — directory placeholder.
- v0.2 — recipe shells in place (this commit).
- v0.3 — `alp-sdk_git.bb` builds the SDK as a shared library
  against the host toolchain provided by the Yocto SDK.
- v0.4 — Linux backends in `src/yocto/` go from stub to real
  (`/dev/i2c-N`, `gpiod`, ALSA, V4L2 / GStreamer, libusb).  Image
  templates for vision / audio / IoT product classes ship.

## Compatibility

Tested layer series (planned):

- `kirkstone`   (LTS, gcc 11)
- `scarthgap`   (LTS, gcc 13)

Earlier series may work but aren't tested.

## License

The layer itself is Apache-2.0 (see `LICENSE` at repo root).
The chips and SoCs targeted here ship under their respective
vendor licenses.
