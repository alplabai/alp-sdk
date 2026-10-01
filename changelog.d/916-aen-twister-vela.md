### Fixed — AEN twister installs Vela for the U55 inference example (#916)

`examples/aen/aen-npu-inference-alp-u55` gained a `testcase.yaml` in #2567,
which put it into `pr-twister-aen.yml`. That job had no `vela` on PATH, so
all four board targets failed at CMake time with
`aen-npu-inference-alp-u55: model-gen failed (rc=2)`. The job now
pip-installs `ethos-u-vela` at the version `pyproject.toml` pins.
