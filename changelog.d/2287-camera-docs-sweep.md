### Docs — list all three RPi-style CSI-2 camera sensors (OV5647, OV9281, IMX296) everywhere they're enumerated (#2287)

`docs/camera-shields.md` was already the authoritative reference for the three merged
sensors, but several other places that enumerate the supported camera modules still
named only a subset. Brought into consistency with `docs/camera-shields.md`, with
status taken only from that table (no new bench claims made anywhere in this sweep):

- `metadata/chips/ov5647.yaml` — added, mirroring `imx296.yaml`'s catalogue-only shape
  (`driver_status: none`, no `chips/ov5647/` portable stub — streaming lives entirely
  in `zephyr/drivers/video/ov5647.c`); `scripts/check_chip_manifest_parity.py`'s
  `KNOWN_MANIFEST_NO_DRIVER` allowlist gained the matching `ov5647` entry.
- `docs/architecture.md` — the Camera row now says "Three ... (OV5647, OV9281, IMX296)".
- `examples/aen/README.md` — the `aen-camera-firstlight` row now names all three.
- `docs/boards/e1m-evk.md` — J5's shield-pairing examples and the
  `aen-camera-firstlight` bench summary now include IMX296.
- `zephyr/boards/shields/e1m_evk_rpi_csi/doc/index.rst` — added a "Compatible sensor
  shields" list naming all three.
- `docs/aen-bench-bringup.md` and `zephyr/kconfigs/vendor-alif-peripherals.kconfig` —
  the camera-stack bench-status notes now mention IMX296 alongside OV9281/OV5647.

`docs/README.md`, `docs/test-plan.md`, and `docs/verification-status.md` already listed
all three consistently and needed no change. `examples/aen/aen-isp-capture/README.md`
and `examples/connectivity/camera-mjpeg-stream/README.md` already document IMX296 in
full. IMX335 is intentionally excluded — it lands in its own branch.
