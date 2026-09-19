# v2n-drpai-inference

Run a compiled model through the RZ/V2N's on-die **DRP-AI3** NPU via
`<alp/inference.h>`, on one or more still frames given on the command
line, and print per-image results plus timing -- the demo the
exhibition booth runs (issue #1268).

> **`[UNTESTED on silicon]`.** Builds clean as a standalone C translation
> unit against the real `<alp/inference.h>` header (see "What actually
> ran" below). Linking against a real `libalp_sdk` (`ALP_OS=yocto`) and
> running end-to-end through the documented NOSUPPORT path have **not**
> been re-verified by this PR. No inference has run against real DRP-AI
> hardware yet -- see
> [`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)
> for the full silicon status.

## What this shows

1. **Load a DRP-AI-compiled model bundle** (a `drpai_dir` tar) and open
   it through the portable `<alp/inference.h>` surface with
   `backend = ALP_INFERENCE_BACKEND_DRPAI` and
   `format = ALP_INFERENCE_MODEL_DRPAI`.
2. **Feed one or more raw pre-processed frames**, one inference call
   each, printing per-image output size, wall-clock invoke time, and
   the top-N raw output values.
3. **Handle DRP-AI being unavailable** the way the documented
   NOSUPPORT contract requires -- `alp_inference_open()` returns NULL,
   this program reports why (backend not compiled in / driver absent /
   driver busy) and exits cleanly instead of crashing. Same shape as
   the [`v2n-m1-deepx-inference`](../v2n-m1-deepx-inference/) sibling.

## Input: raw pre-processed frames, not JPEG/PNG

Decoding a real image file needs an image codec library outside this
SDK's portable surface, so this example reads **raw pre-processed
frames** instead: flat **640x640x3 float32 NCHW** buffers, exactly
4,915,200 bytes each (`640 * 640 * 3 * sizeof(float)`) -- **planar**
(the whole R plane, then the whole G plane, then the whole B plane),
not interleaved HWC. NCHW is the target model's declared ONNX input
shape (RUHMI's `yolox-S_VOC.onnx`, `1,3,640,640` per
[`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)
Sec 5 and `scripts/alp_model/adapters/drpai.py`), and it reaches the
model unchanged: the DRP-AI backend
([`src/yocto/inference_drpai.cpp`](../../../src/yocto/inference_drpai.cpp))
pushes this buffer straight into the MERA runtime's `SetInput()` and
never runs the compiled bundle's own `preprocess/` step against it.
The runtime cannot catch a layout mistake for you -- `alp_inference_
get_input()` reports only a byte count, never a shape (the MERA
wrapper exposes no per-input shape, so rank stays 0), so an
interleaved HWC file of the identical byte count passes the size check
and would silently be fed to the NPU with the wrong channel ordering.
Getting NCHW right is entirely the caller's job.

Channel order (RGB vs BGR), pixel normalisation (raw 0-255 vs /255 vs
mean/std), and letterbox padding are **unverified** on this review
host (no vendor sample was run here) -- the vendor's own
`how-to/sample_app_v2h/app_yolox_cam` sample is the authority for all
three; match it exactly, do not guess. A camera/video capture path is
explicitly **out of scope** here (that is issue #1149); a customer
with a live pipeline produces frames in this NCHW layout with whatever
resize/normalise/HWC->CHW transpose their capture path already needs.
A host-side sketch with Pillow + NumPy -- RGB, raw 0-255 float32,
plain resize with no letterbox -- is a **placeholder only**, to be
checked against `app_yolox_cam` before trusting it for real
detections:

```python
import numpy as np
from PIL import Image
img = Image.open("photo.jpg").convert("RGB").resize((640, 640))
# HWC (640, 640, 3) -> CHW (3, 640, 640): move the channel axis from
# last to first -- the transpose the model's NCHW input needs and a
# same-size HWC file would silently skip.
np.asarray(img, dtype=np.float32).transpose(2, 0, 1)[None].tofile("frame0.bin")
```

## Model bundle

The first argument is a path to a `drpai_dir` bundle **tar** -- the
output of

```sh
python3 -m alp_model build --target drpai --product V2N <model.onnx>
```

(`scripts/alp_model/adapters/drpai.py`; see
[`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)
Sec 5). The target is **YOLOX-S trained on VOC**
(RUHMI's `how-to/sample_app_v2h/app_yolox_cam/yolox-S_VOC.onnx`, ONNX
input `1,3,640,640` -- NCHW). **No compiled `drpai_dir` bundle exists
yet, though** -- Sec 5 of that doc confirms only the ONNX source is
present in a fresh checkout, not a compiled
`drp_desc.bin`/`weight.bin`/`addr_map.txt`/`deploy.json` set;
compiling one with real (not random-calibration) accuracy needs a real
calibration image set, tracked in alp-sdk#1271, not done here. Once a
bundle is compiled, its `deploy.json`'s single fused `mera_drp` op
means the whole graph is NPU-offloaded (no CPU-side split), and this
example's frame generator should be checked byte-for-byte against
that bundle's own sample `input_0.bin` -- also owed, not done, since
neither the bundle nor that sample exists in this checkout.

## Output: raw scores, not decoded detections

Because the compiled graph is fully fused, `alp_inference_get_output()`
hands back **one flat float32 tensor**: the raw, pre-decode network
output across YOLOX-S's roughly 8400 candidate boxes (three feature-map
strides -- 8 / 16 / 32 for a 640x640 input -- each carrying box
regression + objectness + 20 VOC class scores).

Turning that into real `(class, confidence, box)` detections needs a
full YOLOX decoder: generate each stride's grid + anchor points, apply
sigmoid to the objectness and class-score channels, regress
`(cx, cy, w, h)` against the matching grid cell, and run
non-maximum-suppression across the candidate set. **That decoder is
not implemented in this example** -- writing one against a model this
SDK has never run on silicon would be unverifiable guesswork dressed
up as a real feature, not a teaching example. Instead the program
prints the **top-5 largest raw values** in the output tensor, clearly
labelled as raw and undecoded (see `print_top_scores()` in
`src/main.c`, backed by the selection logic in `src/top_scores.c`). A
team productising this demo adds the real decoder once a board
confirms the raw tensor's actual layout.

## Build

### Yocto (recommended)

The `alp-drpai-inference` recipe
(`meta-alp-sdk/recipes-examples/alp-drpai-inference/`) packages this
binary into `alp-image-edge` whenever the image is built with the
existing DRP-AI opt-in:

```
ALP_ENABLE_DRPAI = "1"
PACKAGECONFIG:append:pn-alp-sdk = " drpai"
```

in `local.conf`. **Both switches are required and neither implies the
other** -- see
[`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)
Sec 4 for the full contract, including what each half installs and
what omitting either one does.

> The recipe's `SRC_URI` currently tracks alp-sdk's `dev` branch (this
> example is not on `main` yet) -- flip it to `branch=main` at the next
> promotion, the same pattern `alp-lvgl-dashboard_0.6.bb` documents.
> A bake against `main` before that promotion will not find this
> example's `CMakeLists.txt`.

After boot:

```sh
v2n-drpai-inference <model.tar> <frame0.bin> [frame1.bin ...]
```

### Standalone cross-compile

```sh
cmake -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
      -S examples/v2n/v2n-drpai-inference -B build/drpai-demo
cmake --build build/drpai-demo
scp build/drpai-demo/v2n-drpai-inference root@<board-ip>:
```

### Host build (no DRP-AI; the link and the NOSUPPORT path have NOT been re-verified by this PR)

```sh
cmake -S . -B build/host -DALP_OS=yocto
cmake --build build/host
gcc -I include -o v2n-drpai-inference \
    examples/v2n/v2n-drpai-inference/src/main.c \
    examples/v2n/v2n-drpai-inference/src/top_scores.c \
    build/host/libalp_sdk.a -lpthread
```

## Hardware needed

- E1M-V2N101/102 or E1M-V2M101/102 SoM (DRP-AI3 is on-die in every
  RZ/V2N-family SKU, per
  [`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)'s
  intro -- DEEPX on V2M is an addition, not a replacement).
- E1M-X-EVK carrier.
- An `alp-image-edge` bake with `ALP_ENABLE_DRPAI = "1"` AND
  `PACKAGECONFIG:append:pn-alp-sdk = " drpai"` both set in `local.conf`
  (see "Yocto (recommended)" above), and `meta-rz-drpai` in
  `bblayers.conf`.

## What actually ran

- **Builds clean as a standalone C translation unit** against the real
  `<alp/inference.h>` header. **Linking against a real `libalp_sdk` and
  running end-to-end through the documented NOSUPPORT path have NOT
  been re-verified by this PR**: that claim was carried over from
  `feat/1145-drpai-v2n-bringup` -- the branch whose self-reported
  claims were found false and which is the reason this salvage PR
  exists -- and was never independently re-run. This PR's own review
  host is macOS, where the full SDK does not build at all for an
  unrelated reason (a `section` attribute error blocks `src/backend.c`
  / `src/backends/storage/sw_fallback.c` repo-wide), so the claim stays
  open, not confirmed, until it runs on Linux. The frame-size guard and
  argv parsing have been read and hand-traced for correctness, not
  executed.
- **`print_top_scores()`'s top-N selection is unit-tested** against
  hand-built float arrays (descending order, ties, negative values,
  `count < TOP_N`, empty input, non-finite values) -- the selection
  logic lives in `src/top_scores.c` and its ZTEST suite is
  [`tests/unit/top_scores`](../../../tests/unit/top_scores/), run by
  `pr-twister` like every other example-algorithm unit test in this
  repo (e.g. `tests/unit/defect_map` for `visual-defect-detection`).
- **Nothing has run against real DRP-AI silicon.** `alp_inference_open()`
  with a real bundle first executes on a `drpai`-enabled
  `alp-image-edge` bake on an E1M-X V2N board; see
  [`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)
  Sec 7 for the on-board verification steps this gates.

## Reference

- [`<alp/inference.h>`](../../../include/alp/inference.h) -- portable
  inference surface (CPU / Ethos-U / DRP-AI / DEEPX-DX).
- [`docs/bring-up-drpai-v2n.md`](../../../docs/bring-up-drpai-v2n.md)
  -- the full DRP-AI3 bring-up story: kernel driver, devicetree,
  image opt-in, model compile, deploy, on-board verification.
- [`v2n-m1-deepx-inference`](../v2n-m1-deepx-inference/) -- the DEEPX
  sibling on V2N-M1; same NOSUPPORT contract shape, different backend.
- [`src/yocto/inference_drpai.cpp`](../../../src/yocto/inference_drpai.cpp)
  -- the SDK's DRP-AI backend this example calls into.
