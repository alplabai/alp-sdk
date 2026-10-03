V2N DRP-AI3 access is tightened and DRP1 is enabled. A new kernel patch
(`0018`) requires `CAP_SYS_RAWIO` for the vendor driver's register ioctls
64-69. `/dev/drpai*` moves from group `video` to a new `drpai` system group
(`0660`), installed only in images that carry the SDK DRP-AI backend
(`PACKAGECONFIG[drpai]` pulls `alp-drpai-udev`); `/run/alp` follows. DRP-AI DMA
still reaches any physical address, so `drpai` membership is privileged
(documented in `docs/bring-up-drpai-v2n.md`). `&drp1` (OpenCVA + codec) is
enabled by a dtsi fragment installed only with `meta-rz-opencva` or
`meta-rz-codecs`. `lib-tvm` is no longer installed explicitly; it still arrives through `libalp_sdk.so`'s shared-library dependency on `libtvm_runtime` until the TVM link fix (branch `fix/v2n-audit-yocto-sdk`, which drops `tvm_runtime` from the link) lands.
