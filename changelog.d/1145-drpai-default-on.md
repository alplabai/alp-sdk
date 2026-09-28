### Changed — DRP-AI3 is reachable by default on V2N/V2M images (#1145)

Every shipped V2N/V2M image already carried the DRP-AI3 kernel driver
(`CONFIG_DRPAI=y`), its 512 MiB `drp-ai@d0000000` arena and the vendor
runtime (`lib-tvm`, `mmngr`), but not `/dev/drpai0`: `&drpai0` stayed
`status = "disabled"`, because `ALP_ENABLE_DRPAI` defaulted to `"0"`. Board #1
(E1M-V2M103, Alp SDK 0.7.0 image) showed exactly that on 2026-09-28.

Changes:
- `ALP_ENABLE_DRPAI` now defaults to `"1"` whenever meta-rz-drpai is in
  bblayers.conf, in all six V2N/V2M machine confs and the kernel bbappend
  fallback. `ALP_ENABLE_DRPAI = "0"` opts out.
- `PACKAGECONFIG[drpai]` (the SDK backend) turns on by itself on an
  `rzv2n-family` MACHINE when the node is on and `RUHMI_DRPAI_TVM_DIR` points
  at a RUHMI checkout. Without one, it stays off instead of failing the bake.
- `alp-image-edge` installs the `alp-drpai-inference` demo only when that
  backend is built.
- `docs/bring-up-drpai-v2n.md`, `meta-alp-sdk/README.md` and the example
  docs describe the new defaults.
