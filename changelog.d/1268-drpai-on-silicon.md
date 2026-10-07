### Changed — the DRP-AI3 backend has run on silicon (#1268)

On an E1M-V2M103 (board #1), with the `&drpai0` override applied by hand to
the board DTB and `src/yocto/inference_drpai.cpp` cross-built against the real
MERA2 runtime, YOLOX-S/VOC ran through `<alp/inference.h>`:
- `open()` works under `DRPAI` and `AUTO`, and `invoke()` takes ~40 ms per
  640x640 frame.
- Outputs match the compiler's interpreter reference (correlation 0.996-0.998).
- A VOC-calibrated bundle detects an aeroplane photo as aeroplane at 0.519,
  against 0.525 from ONNX Runtime CPU on the same input.

`docs/bring-up-drpai-v2n.md`'s status banner and the stale "never run on
DRP-AI silicon" comments are updated. A baked `alp-image-edge` doing the same
on its own is still to be confirmed.
