### Added — `aen-evk-demo` gains a real JPEG-encode phase, and the JPEG+NPU stub splits in two (#2035)

The demo's single `"JPEG + NPU"` stub is now two phases, and the phase count
moves from thirteen to fourteen.

**Phase 13 — JPEG encode — is implemented against the hardware encoder.** It
builds a synthetic 64x64 NV12 gradient (no camera; none is attached to this
bench and none is required) and encodes it through the portable
`<alp/jpeg.h>` surface. It follows `examples/aen/aen-jpeg-regcheck`, the
silicon-proven reference, on all three defects a real `E1M-AEN803` bench run
exposed there:

* `prj.conf` sets `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8=y`. Without it the build
  resolves `ALP_SOC_REF_STR="unknown"`, the selector filters out
  `src/backends/jpeg/alif_hantro.c` (`silicon_ref="alif:ensemble:e8"`,
  priority 100), and the software fallback
  (`src/backends/jpeg/sw_baseline.c`, `"*"`, priority 50) silently wins — the
  app still prints a valid JPEG while never touching the silicon.
* Both DMA buffers are tagged into the `"SRAM0"` linker region and link at
  `0x02000000` (output) and `0x02002000` (source) — the Hantro VC9000E is an
  AXI bus master and cannot reach the M55's core-local DTCM, which is where
  this board's default `zephyr,sram = &dtcm` would otherwise put them.
* The source layout comes from `alp_jpeg_caps_t::pixfmt_mask` on the backend
  that actually won, queried at runtime — not from an `#ifdef` on the build
  target.

**`ALP_OK` is not the verdict.** This app exists because a previous example
counted a successful chip-ID read as a pass while every sensor returned its
reset sentinel, so the phase asserts the bytes really are a JPEG: SOI
`FF D8 FF` at the start, EOI `FF D9` at the end, and an encoded length that
is non-zero, at least 256 B, and smaller than the 6144 B source
(`aen-jpeg-regcheck` measured **935 B** for this exact frame on real AEN801
silicon — an order-of-magnitude reference, not an expected value). A
software-fallback win is also a `FAIL`: on this board the hardware encoder is
the phase. Every return code, the encoded byte count, the marker bytes
checked, and `caps.hw_accelerated` are printed verbatim, so a failure is
diagnosable from the log without a second bench reservation.

`<alp/jpeg.h>` exposes no accessor for the Hantro hardware-ID register, and
the example does not hand-roll a register poke to reach it. A `JPEG_SWREG0`
mismatch against `JPEG_HW_ID` (`0x90001000`) still surfaces in the log twice
over: `jpeg_hw_init()` returns `-ENODEV`, which fails the device's init and
makes `alp_jpeg_open()` return `NULL` with `ALP_ERR_NOT_READY`, and the
driver's own `"JPEG hardware not found (ID: 0x%08x)"` `LOG_ERR` line carries
the value it read (this app builds `CONFIG_LOG=y`).

**Phase 14 — NPU inference — stays a `SKIPPED` stub, for a stated reason
that is a boot-flow constraint, not a missing driver.**
`examples/aen/aen-npu-inference-alp` is the silicon-proven Ethos-U85 path
through `<alp/inference.h>`, but its Vela-compiled `person_detect_u85` model
is **~263 KiB** — precisely why that app links into MRAM slot0 and boots via
Flow D. `aen-evk-demo` is a Flow C ITCM RAM-run, ITCM is **256 KB total**, and
the demo occupied **95008 B (36.24%)** before this change and **108552 B
(41.41%)** after it. The model does not fit alongside it, so adding NPU
inference here means relinking the whole demo into MRAM slot0 — a different
unit of work from adding a phase.
Shrinking the model to fit would trade a proven artefact for an unproven one.

Builds clean for `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` at
`FLASH: 106 KB / 256 KB (41.41%)`, `RAM: 13104 B (5.00%)`, plus
`SRAM0: 14 KB` for the two JPEG DMA buffers — inside the ITCM budget. The
JPEG phase's own `PASS`/`FAIL` still needs a bench run on `e1m-aen-evk-03`;
the six previously implemented phases are unchanged.
