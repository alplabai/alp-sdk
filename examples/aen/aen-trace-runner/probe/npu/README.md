# NPU probe (M55-HP, no camera)

DIAGNOSTIC, NOT PRODUCT CODE. This probe proves the NPU body-control pipeline on silicon
before the camera comes back to the game board. Design:
`docs/superpowers/specs/2026-09-24-npu-body-control-design.md`.

The M55-HP runs the cut MoveNet (`tools/movenet_cut.py`) on its Ethos-U55-256 through
`<alp/inference.h>`. It runs three canned 640x400 GREY8 frames (empty, standing, crouching)
through the game's own pre- and post-processing (`src/vision/movenet.c`, `pose.c`). All data
comes from an MRAM payload at `0x80100000`.

It runs two passes:
- **Pass A:** the model is copied into SRAM0. This is the placement proven on the HE
  (alp-sdk `aen-npu-inference-alp-u55`).
- **Pass B:** the NPU reads the weights in place from MRAM. The game needs this, because it has
  no 2.4 MB of SRAM to spare.

Per frame the probe records, from the DWT:
- Pre-process time.
- Invoke time (min/avg/max over 16 runs).
- Decode time.
- The CRC of each output map against the host's (informational only; an NPU is not bit-exact
  to the TFLite reference kernels).
- The torso keypoint error against the host decode.
- Presence.

Then it runs a 1 s sustained loop. Results go to the RAM console and to a fixed result block
at `TR_MEM_PSLOT` = SRAM0 `0x0237F200`. Words 0..3 of that block are the
`flash-jlink-hp.sh` beacon layout: magic `0x4E505552`, CPUID, VTOR, heartbeat.

`RESULT PASS` requires all of the following:
- Both passes open and invoke.
- Every frame's presence matches the host.
- The torso error is ≤ 27 px, two MoveNet cells.

## Build (host only)

```sh
cd examples/aen/aen-trace-runner
# 0. venv + models (see tools/npu_body_proto.py's header)
python3 -m venv .venv && .venv/bin/pip install ai-edge-litert ethos-u-vela numpy pillow flatbuffers
# flatc (object API for the cut): github.com/google/flatbuffers releases v25.12.19 Linux.flatc.binary.g++-13.zip
.venv/flatc/flatc --python --gen-object-api -o .venv/flatc/gen \
  <tflite-micro>/tensorflow/compiler/mlir/lite/schema/schema.fbs
PYTHONPATH=.venv/flatc/gen .venv/bin/python tools/movenet_cut.py \
  /tmp/tr-npu-models/movenet_lightning_int8.tflite /tmp/tr-npu-models/movenet_cut.tflite
# 1. frames (writes /tmp/tr-npu-frames/frame_{stand,crouch}.npy), then the payload
.venv/bin/python tools/npu_body_proto.py --header /tmp/tr-npu-clip.h --vectors /tmp/tr-npu-vec
.venv/bin/python tools/npu_probe_payload.py --cut /tmp/tr-npu-models/movenet_cut.tflite \
  --out /tmp/tr-npu-probe/payload.bin \
  --vela-config <Alif sdk-alif>/samples/modules/executorch/ensemble_vela.ini
# 2. the HP image (ITCM, 0x50000000)
ZEPHYR_BASE=$ZEPHYR_BASE ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR \
west build -p always -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp -d /tmp/tr-npu-probe/build probe/npu -- \
  -DEXTRA_ZEPHYR_MODULES="<alp-sdk>;<hal_alif>;<tflite-micro module>" \
  -DPython3_EXECUTABLE=/usr/bin/python3
# 3. the probe ATOC, in a PRIVATE SETOOLS copy (app-gen-toc rewrites build/)
cp -a <SETOOLS> /tmp/tr-npu-probe/setools
cp /tmp/tr-npu-probe/build/zephyr/zephyr.bin /tmp/tr-npu-probe/setools/build/images/tr_npu_probe_hp.bin
cat > /tmp/tr-npu-probe/setools/build/config/tr-npu-probe.json <<'JSON'
{
    "DEVICE":  { "disabled": false, "binary": "app-device-config.json", "version": "0.5.00", "signed": true },
    "HP-APP":  { "disabled": false, "binary": "tr_npu_probe_hp.bin", "version": "1.0.0", "signed": true,
                 "cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"] }
}
JSON
(cd /tmp/tr-npu-probe/setools && ./app-gen-toc -f build/config/tr-npu-probe.json)
```

Build of record (2026-09-24, host):

| artefact | size | md5 |
|---|---|---|
| `build/zephyr/zephyr.bin` (ITCM 174,728 B of 256 KiB) | 174,728 | `205d3e6391f6d0d0874851645466ff2c` |
| `payload.bin` @ `0x80100000..0x8040CC0F` | 3,197,968 | `5bdf23d0035e1e51028503cfe07d2c34` |
| `setools/build/AppTocPackage.bin` @ `0x80553FC0..0x8057FFFF` | 180,288 | `f727480b22811f556c746f5d68844db1` (re-signed per `app-gen-toc` run, so the md5 changes each build) |

The Vela'd model inside the payload is 2,429,520 B, crc32 `46f36a1a`.

## Run: `the E1M-AEN803 2026W36-0009 EVK` (E1M-AEN803 2026W36-0009), Flow D

Nothing on the board may be moved. The HP boots only through an ATOC; it cannot be loaded
over the debugger. This flow replaces the release ATOC, which is why step 4 exists.

```sh
export LG_PLACE=the E1M-AEN803 2026W36-0009 EVK LG_COORDINATOR=<coordinator> ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR/gnu
export SETOOLS_DIR=/tmp/tr-npu-probe/setools
labgrid-client -p <place> acquire
cd examples/aen/aen-trace-runner
# 1. save the WHOLE live MRAM (0x80000000..0x8057FFFF, 5,767,168 B) -- read-only. Keep this file.
probe/npu/flash-probe.sh readback /tmp/tr-npu-probe/mram-before.bin
# 2. optional: FLOWD_DRY_RUN=1 first (prints the padded write session, touches nothing)
probe/npu/flash-probe.sh write /tmp/tr-npu-probe/setools /tmp/tr-npu-probe/payload.bin --atoc-unqueryable
#    -> two sector-padded blobs: 0x80100000 (3,211,264 B) + 0x80550000 (196,608 B incl. the package),
#       DPIDR gate, fresh-session read-back proof, race check, RSetType 2 / r / g, then the result block.
# 3. re-read results any time (read-only)
probe/npu/flash-probe.sh results
# 4. restore the release ATOC + the payload sectors, byte-exact, from step 1
probe/npu/flash-probe.sh restore /tmp/tr-npu-probe/mram-before.bin --atoc-unqueryable
#    then a DPS cold cycle; the release game must come back by itself (HE "m55 boot", A32 RUNNING).
```

`--atoc-unqueryable` applies because 2026W36-0009 has no SE-UART (its SE-UART path resolves to
"None"). It is alp-sdk's Flow D acknowledgement for that case (`bench_flowd_atoc_guard`).
Use `--replace-atoc` instead on a place whose SE-UART is wired.

### Reading the result block

`mem32 0x0237F200, 0x61` returns these words (388 B):

| word | field |
|---|---|
| 0 | magic `4E505552` |
| 1 | CPUID. The HP reads `0x411FD220`-class for Cortex-M55. |
| 2 | VTOR |
| 3 | heartbeat. It advances during the sustained loops and every 100 ms after them. |
| 4 | stage: 1 payload, 2 pass A, 3 pass B, 4 done |
| 5 | alp status of the last failure |
| 6 | CPU MHz |
| 7 | verdict: 1 PASS, 2 FAIL |
| 8..51 | pass A frames 0..3 |
| 52..95 | pass B frames 0..3 |
| 96 | sustained Hz x10 (the last pass that ran) |

Each frame's 11 words are `us_pre`, `us_invoke_min`, `us_invoke_max`, `us_invoke_avg`,
`us_decode`, `crc_match` bits, `max_err` px, `person`, and `tr_box_t` (x, y, w, h int16, then confidence and valid bytes, padded to 12 B).

What each stage tells you when it sticks:
- **Stage 1 with verdict 2:** the payload is missing or corrupt at `0x80100000`.
- **Stage 3 with no verdict and a frozen heartbeat:** pass B faulted. The NPU cannot fetch
  weights from MRAM through the SRAM port the backend selects. Pass A's numbers are still in
  words 8..51.
- **Stage 2 stuck:** the HP U55 itself did not come up. Compare against the HE's U55-128 proof
  in alp-sdk #931.

The RAM console is `ram_console_buf`, located with `nm` on `build/zephyr/zephyr.elf`. It sits
at an HP-local DTCM address, readable over AP `0x00200000` once the HP runs.
