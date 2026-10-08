### Added — board.yaml `cameras:` now selects the camera (Zephyr `-DSHIELD`, Yocto `ALP_CAMERA_CAM<n>`) (#2633)

A project's `cameras: [{connector, module}]` block, until now only validated,
drives the build, for the one core that owns the camera (`cameras[].core`, or the one core whose OS the connector supports: Zephyr needs `zephyr_shields` + a module `zephyr_shield`, Yocto needs `camera_connectors.<CAMn>.linux: true`; none or several is ALP-B003). That Zephyr core gets one
`-DSHIELD="<carrier shields> <module shield>"` on its `west build` command and in
`cmake-args.txt` (`-D<image>_SHIELD` on a sysbuild); a Yocto core gets `ALP_CAMERA_CAM<n> = "<module_id>"` in its
`local.conf`. Connector `CAMn` is camera index `n`.

- `camera_connectors.<CAMn>.zephyr_shields` (new, optional, ordered) names the
  carrier-side Zephyr shields; the E1M-EVK CAM0 declares `[e1m_evk_rpi_csi]`.
  `camera_connectors.<CAMn>.linux: true` (new, optional) marks a connector a Linux
  core can drive; the X-EVK CAM0 sets it.
- On a Zephyr owner, a module without `zephyr_shield`, a connector without
  `zephyr_shields`, or a carrier shield with no overlay for the board target is an
  ALP-B003 error; the planner backstops it with a `camera-select-failed` warning.
- `validate_metadata.py` now checks that every `zephyr_shield` /
  `zephyr_shields` entry is a directory under `zephyr/boards/shields/`.
- New fixture `tests/fixtures/cameras-select/board.yaml` pins the AEN OV9281 plan in the emit snapshots.

tan-cli's planner mirror must resync `alp_orchestrate/orchestrator.py`,
`buildplan.py`, `kconfig.py`, `alp_project.py` and the new `cameras.py` and `camera_owner.py`.
