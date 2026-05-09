# yocto/meta-alp — Yocto BSP layer (v0.4+)

Empty placeholder for v0.1.  Yocto support is a v0.4 deliverable per
[`VERSIONS.md`](../../VERSIONS.md):

> **v0.4.0 — "Yocto first-class"** — `meta-alp` layer, BSPs for V2N
> + V2N+M1, recipes per ALP module, image templates for
> vision/audio/IoT product classes.

Until v0.4 lands, the directory carries this README so the porting
guide can point at a concrete location and so the layout is stable
across versions.

## Anticipated layout (v0.4)

```
yocto/meta-alp/
├── conf/
│   ├── layer.conf
│   └── machine/
│       ├── e1m-x-v2n.conf
│       └── e1m-x-v2n-m1.conf
├── recipes-alp/
│   ├── alp-sdk/
│   │   └── alp-sdk_%.bbappend       # builds this repo for the target rootfs
│   ├── images/
│   │   ├── alp-vision-image.bb
│   │   ├── alp-audio-image.bb
│   │   └── alp-iot-image.bb
│   └── ...
└── README.md
```
