### Added — board.yaml `cameras:` now selects the camera (Zephyr `-DSHIELD`, Yocto `ALP_CAMERA_CAM<n>`) (#2633)

A project's `cameras: [{connector, module}]` block, until now only validated,
drives the build. A Zephyr core running a customer app gets one
`-DSHIELD="<carrier shields> <module shield>"` on its `west build` command and in
`cmake-args.txt`; a Yocto core gets `ALP_CAMERA_CAM<n> = "<module_id>"` in its
`local.conf`. Connector `CAMn` is camera index `n`.

- `camera_connectors.<CAMn>.zephyr_shields` (new, optional, ordered) names the
  carrier-side Zephyr shields; the E1M-EVK CAM0 declares `[e1m_evk_rpi_csi]`.
- A module without `zephyr_shield` on a Zephyr core is an ALP-B003 error, and a
  carrier shield with no overlay for the core's board target blocks the build
  command with a `camera-select-failed` plan warning.
- `validate_metadata.py` now checks that every `zephyr_shield` /
  `zephyr_shields` entry is a directory under `zephyr/boards/shields/`.
- New `examples/aen/aen-camera-firstlight/board.yaml` selects the OV9281 this way.

tan-cli's planner mirror must resync `alp_orchestrate/orchestrator.py`,
`buildplan.py`, `kconfig.py` and the new `cameras.py`.
