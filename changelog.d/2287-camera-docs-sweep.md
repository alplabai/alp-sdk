### Documented — bring the three RPi-style CSI-2 camera sensors (OV5647, OV9281, IMX296) into consistency across the docs that enumerate them (#2287)

`docs/camera-shields.md` was already the authoritative reference for the three merged
sensors, but several other places that enumerate the supported camera modules still
named only a subset. Brought into consistency with `docs/camera-shields.md`, with
status taken only from that table (no bench-claim upgrades made anywhere in this sweep):

- `metadata/chips/ov5647.yaml` — added, mirroring `imx296.yaml`'s catalogue-only shape
  (`driver_status: none`, no `chips/ov5647/` portable stub — streaming lives entirely
  in `zephyr/drivers/video/ov5647.c`); `scripts/check_chip_manifest_parity.py`'s
  `KNOWN_MANIFEST_NO_DRIVER` allowlist gained the matching `ov5647` entry, and its
  stale `imx296` comment (claiming OV5647 carried no manifest at all) was corrected.
- `docs/architecture.md`, `examples/aen/README.md`, `docs/aen-bench-bringup.md`,
  `docs/boards/e1m-evk.md`, `zephyr/kconfigs/vendor-alif-peripherals.kconfig` — the
  camera-stack/shield rows now name all three sensors, each's status attributed
  correctly (IMX296 Stage A + Stage B, Stage B via `examples/aen/aen-isp-capture` /
  `examples/connectivity/camera-mjpeg-stream`; OV5647/OV9281 flagged as due for a
  re-bench after issue #2287 Stage B's shared CPI driver change).
- `metadata/chips/ov9281.yaml` — noted the same re-bench-pending caveat next to its
  `hil_silicon: verified`, kept at "verified" rather than downgraded, matching
  `docs/camera-shields.md`'s own framing (the caveat is an open follow-up, not a
  verification regression); `metadata/chips/ov5647.yaml`'s `hil_silicon: partial`
  is unchanged and stays "partial" primarily because RAW8 is unverified.
- `zephyr/boards/shields/e1m_evk_rpi_csi/doc/index.rst` — added a "Compatible sensor
  shields" list naming all three.
- `zephyr/kconfigs/video-sensors.kconfig` and `zephyr/drivers/video/Kconfig.imx296` —
  the stale "PARTIALLY BENCH-VERIFIED (I2C identity only)" comments now say Stage A +
  Stage B bench-verified.
- `examples/aen/aen-camera-firstlight/README.md` — IMX296 updated from Stage A to
  Stage A + Stage B, attributing Stage B to `aen-isp-capture` / `camera-mjpeg-stream`;
  added the OV5647/OV9281 re-bench-pending caveat.
- `docs/camera-shields.md` — corrected a stale sentence in the IMX296 driver section
  that said OV5647 "has no `chips/` stub or `metadata/chips/` manifest" (true when
  written, false once `metadata/chips/ov5647.yaml` above landed).
- `docs/boards/e1m-evk.md` — reworded the IMX296 pull-up-rework paragraph: issue
  #2287's IMX296 bench ran on the same E1M-AEN803/E1M-EVK pairing (serial
  2026W36-0001) as the OV5647 bench, which needed the J5 pin-11 pull-up rework
  (the rework lives on the EVK's J5 connector, not the SoM) — so the rework was
  present for that bench, but whether IMX296 itself needs it (self-enables or not)
  is not established either way in issue #2287's own bench history.
- `docs/test-plan.md`'s AEN801 peripheral-matrix surface column now names all three
  sensors; `docs/verification-status.md` regenerated from it via
  `scripts/gen_verification_status.py`.

`docs/README.md` already listed all three consistently and needed no change.
`examples/aen/aen-isp-capture/README.md` and
`examples/connectivity/camera-mjpeg-stream/README.md` already document IMX296 in
full. IMX335 is intentionally excluded — it lands in its own branch.
