### Added — a display support matrix, so #23 can be narrowed to the remaining hardware proof (#23)

`docs/display-support-matrix.md` lists, per SoM family and display path
(AEN CDC200/MIPI-DSI, AEN chip-driver displays, V2N/V2M Linux DRM/KMS and the
`<alp/display.h>` Yocto backend, i.MX93), what exists in the tree, what has a
written bench record with its source, and what is still open. Every cell
without a recorded bench run says "not verified on hardware". It ends with the
remaining work for #23 and a list of statements in other docs that disagree
with the recorded runs. Documentation only; linked from `docs/README.md` and
the Display sections of `docs/boards/e1m-evk.md` and `docs/boards/e1m-x-evk.md`.
