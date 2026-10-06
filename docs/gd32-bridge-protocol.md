# GD32 bridge protocol (V2N supervisor MCU)

> **Scope.** This document is the **wire** specification for the
> Renesas RZ/V2N ⇄ GD32G553MEY7TR bridge on the E1M-X V2N / V2N-M1
> SoMs.  Both sides — the host driver under
> `chips/gd32g553/` and the GD32-side firmware under `gd32-bridge-firmware:` —
> implement what is described here.  Bit, byte and timing decisions
> live in this file; the host-side public API is documented under
> `<alp/chips/gd32g553.h>`.
>
> **Authoritative board wiring** is in
> [`metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`](../metadata/e1m_modules/v2n/renesas-peripheral-map.tsv)
> (`GD32_SPI.*` rows + `BRD_I2C` rows).

## 1. Hardware transports

The GD32 supervisor is reachable on V2N over **two parallel physical
buses**.  The bridge speaks the **same command-set** on both — only
the framing layer differs (v0.15: the I2C link is restricted to a
management allow-list, §5.3, and the SPI link gains negotiated big frames,
`BATCH` and an `ATTN` line, §3.14 - §3.18).

| Property              | SPI (fast path)                                                            | I2C (management path)                          |
|-----------------------|----------------------------------------------------------------------------|------------------------------------------------|
| Master                | Renesas RZ/V2N SCI7 (Simple-SPI mode)                                      | Renesas RZ/V2N RIIC8 (BRD_I2C master)          |
| Slave                 | GD32G553 SPI peripheral                                                    | GD32G553 I2C peripheral                        |
| Renesas pads          | `P76` MOSI, `P77` MISO, `P96` SCLK, `P97` CS                               | `P07` SCL, `P06` SDA                           |
| GD32 pads             | `PB15` MOSI, `PA10` MISO, `PA9` SCLK, `PA8` CS                             | `PA15` SCL, `PB9` SDA (GD32 = bus slave)       |
| Speed                 | ≤ 25 MHz (host can step down for noisy boards; firmware caps at SoC limit) | Standard / Fast / Fast+ (host picks)           |
| Mode                  | SPI mode 0 (CPOL=0, CPHA=0), MSB-first, 8-bit words                        | 7-bit addressing                               |
| Bus arbitration       | GD32 is dedicated slave — no other masters on the SPI link                 | BRD_I2C is shared (PMICs, RTC, OPTIGA, …)      |
| Liveness signalling   | CS edge transitions                                                        | START / STOP envelope                          |

The host driver can **route a given command over whichever
transport is open** at call time.  Boards that only wire one of the
two transports work unchanged; commands on the un-wired transport
return `ALP_ERR_NOSUPPORT`.

## 2. Endianness and integers

All multi-byte integers are **little-endian** on the wire.  Where a
field is declared `uint32_t` in the C structs, byte 0 is the LSB.

## 3. Common command set

Both transports carry the same command set, with two v0.15 exceptions:
`BATCH`, `ADC_STREAM_BEGIN2` and `ADC_STREAM_READ2` are SPI-only, and the
I2C link answers `STATUS_NOSUPPORT` to every opcode outside the §5.3
allow-list.  Command opcodes are 1 byte; their numeric encoding is:

| Opcode | Name                  | Payload (request)                                  | Reply payload                                      |
|--------|-----------------------|----------------------------------------------------|----------------------------------------------------|
| `0x00` | `PING`                | _empty_                                            | `0x00` (`ALP_OK`)                                  |
| `0x01` | `GET_VERSION`         | _empty_                                            | `major:u8 minor:u8 patch:u8` (`major.minor.patch`) |
| `0x02` | `GET_BUILD_ID`        | _empty_                                            | `build_id:char[20]` (truncated SHA-1, ASCII hex)   |
| `0x03` | `RESET_REASON`        | _empty_                                            | `cause:u8` (`gd32g553_reset_cause_t`)              |
| `0x04` | `BATCH` (v0.15)       | `count:u8 {op:u8 len:u8 args[len]}[count]`         | `executed:u8 {status:u8 len:u8 payload[len]}[executed]` (SPI only; needs the `BATCH` link feature -- see §3.16) |
| `0x10` | `GPIO_READ`           | `mask:u32`                                         | `levels:u32` (masked subset)                       |
| `0x11` | `GPIO_WRITE`          | `mask:u32 levels:u32`                              | _empty_                                            |
| `0x20` | `PWM_SET`             | `channel:u8 reserved:u8 period_ns:u32 duty_ns:u32` | _empty_                                            |
| `0x21` | `PWM_GET`             | `channel:u8`                                       | `period_ns:u32 duty_ns:u32`                        |
| `0x30` | `ADC_READ`            | `channel:u8 samples:u8`                            | `mv[samples]:u16` (millivolt, raw averaged)        |
| `0x40` | `DA9292_STATUS_FORWARD` | _empty_                                          | `da9292_faults:u8` (always `0xFF` on this HW rev — see §3.4) |
| `0x41` | `SE_RESET` (v0.8)       | `assert:u8` (`0` = release, `1` = hold in reset) | _empty_ (see §3.15) |
| `0x42` | `BOOT_CONFIG` (v0.15)   | `op:u8 flags:u32` (`op` 0 = GET, 1 = SET; `flags` ignored on GET) | `flags:u32` (the stored value; see §3.19) |
| `0x50` | `DAC_SET`             | `channel:u8 reserved:u8 value_mv:u16`              | _empty_                                            |
| `0x51` | `DAC_GET`             | `channel:u8`                                       | `value_mv:u16`                                     |
| `0x60` | `QENC_READ`           | `encoder:u8`                                       | `position:i32`                                     |
| `0x61` | `QENC_RESET`          | `encoder:u8`                                       | _empty_                                            |
| `0x70` | `COUNTER_READ`        | `counter:u8`                                       | `ticks:u32`                                        |
| `0x22` | `PWM_CONFIGURE`       | `channel:u8 align:u8 dead_time_ns:u32 break_cfg:u8` | _empty_ (see §3.8; `align` applied, `dead_time_ns`/`break_cfg` return `STATUS_NOSUPPORT` -- V2N routes only single-ended outputs / no BRK pad) |
| `0x32` | `ADC_CONFIGURE`       | `channel:u8 reserved:u8 oversample:u16 sample_cycles:u16 resolution:u8` | _empty_ (see §3.9; `sample_cycles`/`oversample`/6-12b `resolution` sticky; 14/16b unsupported by the hardware, return `STATUS_NOSUPPORT`) |
| `0x33` | `ADC_STREAM_BEGIN`    | `stream_id:u8 channel:u8 reserved:u8 sample_rate_hz:u32` | _empty_                                      |
| `0x34` | `ADC_STREAM_READ`     | `stream_id:u8 max_samples:u8`                      | `got:u8 mv[max_samples]:u16` (zero-padded)         |
| `0x35` | `ADC_STREAM_END`      | `stream_id:u8`                                     | _empty_                                            |
| `0x80` | `TRNG_READ`           | `len:u8` (1..32)                                   | `random_bytes[len]`                                |
| `0x90` | `TMU_COMPUTE`         | `function:u8 format:u8 reserved:u16 in_a:u32 in_b:u32` | `result:u32`                                  |
| `0x36` | `ADC_STREAM_CONFIGURE_DSP` | _(reserved tombstone -- see §3.x)_             | _(empty; returns `STATUS_NOSUPPORT` permanently -- use `CMD_ADC_DSP_CHAIN_*` instead)_ |
| `0x3A` | `ADC_SPECTRUM_READ` (v0.9) | `stream_id:u8 bin_offset:u16 max_bins:u8`          | `seq:u32 total_bins:u16 got:u8 bins[max_bins]:f32` (see §3.x -- FFT-terminal chains; `STATUS_NOSUPPORT` if not FFT-bound, `STATUS_BUSY` before first frame) |
| `0x3B` | `ADC_STREAM_BEGIN2` (v0.15) | `stream_id:u8 channel:u8 trigger_src:u8 trigger_arg:u8 sample_rate_hz:u32 watermark:u16 reserved:u16` | `tick_hz:u32 period_ticks:u32 full_scale:u16 vref_mv:u16 flags:u8 watermark:u16 ring_depth:u16` (SPI only; needs `ADC_STREAM2` -- see §3.18) |
| `0x3C` | `ADC_STREAM_READ2` (v0.15) | `stream_id:u8 max_samples:u8`                    | `first_index:u32 dropped:u32 got:u8 codes[got]:u16` (**variable length**, `9 + 2*got` bytes; SPI only; needs `ADC_STREAM2` -- see §3.18) |
| `0x23` | `PWM_CAPTURE_BEGIN`   | `channel:u8 edge:u8`                                  | _empty_ (see §3.y -- reconfigures the pad as input-capture) |
| `0x24` | `PWM_CAPTURE_READ`    | `channel:u8`                                          | `period_ns:u32 pulse_ns:u32` (see §3.y -- returns `STATUS_NOSUPPORT` ("ring empty") until an edge lands; no edges land on this V2N HW rev pending a pad-routing rework) |
| `0x25` | `PWM_CAPTURE_END`     | `channel:u8`                                          | _empty_                                            |
| `0x26` | `PWM_SINGLE_PULSE`    | `channel:u8 reserved:u8 reserved:u16 pulse_ns:u32`    | _empty_                                            |
| `0x27` | `TIMER_SYNC`          | `master:u8 slave:u8 mode:u8`                          | _empty_                                            |
| `0x28` | `POWER_MODE_SET`      | `mode:u8 reserved:u8 wake_bitmap:u32 wake_after_ms:u32` | _empty_ (see §3.z -- `wake_bitmap` bits `UART_RX`/`USB`/`ETH_LINK` return `STATUS_NOSUPPORT`; no HW path on GD32G5) |
| `0x81` | `LINK_FEATURES` (v0.7) | `features:u8` (wanted; bit0 = `STATUS_SEQ`) **or** (v0.15, 6 bytes) `want:u32 max_payload_req:u16` | `features:u8` (granted + armed) **or** (v0.15, 10 bytes) `granted:u32 supported:u32 max_payload:u16`; see §3.14 / §4.1.1 |
| `0xA0` | `I2CM_CONFIG` (v0.17) | `bus_khz:u16` (`100` or `400`; `0` releases PC8/PC9 to hi-Z) | _empty_ (I2C link only; see §3.20) |
| `0xA1` | `I2CM_XFER` (v0.17)   | `tag:u8 addr7:u8 flags:u8 wlen:u8 rlen:u8 wdata[wlen]` (`flags` = 0, `wlen` <= 60, `rlen` <= 62) | _empty_ (job queued; I2C link only; see §3.20) |
| `0xA2` | `I2CM_RESULT` (v0.17) | _empty_ | _empty_ while `BUSY`, else `tag:u8 result:u8 nread:u8 rdata[nread]` (**variable length**; I2C link only; see §3.20) |

Opcodes `0x82..0xEF` are **reserved** for future Alp-defined
extensions (next slot: hardware AES via the CAU engine).  Boards
SHOULD NOT define their own opcodes in this range -- the firmware
replies with **`ALP_ERR_NOSUPPORT`** (see §6) for any opcode it
does not implement at build time, so a host that speaks a newer
command set than the firmware degrades gracefully.

### 3.z System power-mode set (v0.5+)

`CMD_POWER_MODE_SET` (opcode `0x28`) is the host-to-supervisor
sleep-transition request.  The portable surface lives in
[`<alp/power.h>`](../include/alp/power.h):
`alp_power_open / alp_power_configure_wake_source /
alp_power_request_sleep / alp_power_close`.  The supervisor wakes
the Renesas SoC on the configured wake source(s), then re-runs
its own GD32 handshake so the bridge stays usable across deep-
sleep cycles.

The firmware-side dispatcher implements all four modes: `0` (run)
and `1` (sleep) are accepted no-ops (the bridge's main loop already
parks in `__WFI()` between transport interrupts), `2` (deep-sleep)
calls `pmu_to_deepsleepmode()`, and `3` (standby) calls
`pmu_to_standbymode()`.  Of the `wake_bitmap` bits, `RTC` and
`TIMER` arm the RTC wakeup timer (IRC32K / DIV16, 0.5 ms LSB, up to
~32.7 s; `wake_after_ms` also arms this same timer regardless of
the bitmap, per the `<alp/power.h>` contract), and `GPIO` enables
`PMU_WAKEUP_PIN0..4`.  `UART_RX` / `USB` / `ETH_LINK` return
`STATUS_NOSUPPORT` -- the GD32G5 baseline has no LPUART wake, USB
OTG, or MAC, so there is no hardware path for these bits on this
SoC.  The portable surface in `<alp/power.h>` honours INVAL
pre-checks (e.g. RUN mode, no wake sources + zero wake_after_ms)
before falling through to the firmware.  HIL verification of the
sleep transitions on real V2N silicon is still ahead (see
`docs/v1.0-readiness.md` §1a).

### 3.y Advanced timer extras (v0.5+)

The wave-2 §2B.2 advanced-timer extras add five opcodes within the
existing PWM range (`0x23..0x27`) and the timer-sync group. All
five dispatch to real `bridge_hw_*` bodies today (not the
default-case `STATUS_NOSUPPORT` branch); HIL verification on real
V2N silicon is still ahead for all three groups below (see
`docs/v1.0-readiness.md` §1a):

* `CMD_PWM_CAPTURE_BEGIN / READ / END` (opcodes `0x23..0x25`)
  reconfigure a PWM channel's pin as an input-capture source so
  the firmware can latch the timer counter on each edge of the
  caller's chosen polarity (`RISING / FALLING / BOTH`).  The
  host-side surface is `alp_pwm_capture_open / read / close` in
  [`<alp/pwm.h>`](../include/alp/pwm.h).  The firmware body is
  landed (polled drain + wrap-aware delta + ns conversion), but
  every E1M PWM pad on V2N binds the timer's COMPLEMENTARY
  (`CHxN`) output, while the classic input-capture stage samples
  the MAIN `CHx` pad -- a different physical pin.  `BEGIN`
  correctly switches the channel to input-capture mode, but no
  edges land on this hardware revision until a follow-up hardware
  bring-up commit reworks the pad routing; `READ` reports
  `STATUS_NOSUPPORT` ("ring empty") in the meantime.
* `CMD_PWM_SINGLE_PULSE` (opcode `0x26`) drives a one-shot pulse
  of caller-specified width on a PWM channel then stops.  The
  host-side surface is `alp_pwm_single_pulse(pwm, pulse_ns)` in
  [`<alp/pwm.h>`](../include/alp/pwm.h).  Implemented via the
  timer's one-pulse mode (OPM); the whole timer's SP bit flips, so
  sibling channels on the same `TIMER0`/`TIMER7` also run
  single-pulse until a subsequent `PWM_SET` restores repetitive
  mode.
* `CMD_TIMER_SYNC` (opcode `0x27`) links the GD32G5's `TIMER0` /
  `TIMER7` / `TIMER19` (wire ids `0`/`1`/`2`) in master-slave
  configuration for synchronised multi-channel output.
  Implemented: the master emits `TRGO0` on update and the slave
  listens on `ITI0`, with the wire `mode` byte translated to the
  vendor's `TIMER_SLAVE_MODE_*` / `TIMER_QUAD_DECODER_MODE*`
  encoding.  The `SYSCFG` trigger router is left at its chip
  default; a non-default (master, slave) pair needing a different
  route is a follow-up.

### 3.x ADC-stream DSP pipeline (v0.5+)

The wave-2 ADC-stream DSP pipeline attaches a chain of
FIR / IIR / WINDOW / FFT stages to a streaming ADC source so raw
samples never leave the GD32 when the customer wants filtered or
spectral data -- the bandwidth win that makes the GD32G5's FFT
and FAC blocks load-bearing rather than vestigial.  Portable
surface lives in [`<alp/adc.h>`](../include/alp/adc.h)
(`alp_adc_filter_t` / `alp_adc_spectrum_t`) plus the standalone
in-RAM chain primitives in [`<alp/dsp.h>`](../include/alp/dsp.h).

Three opcodes own the upload path -- `chain_open` / `stage_push` /
`chain_bind` allocate, upload, validate, and bind a chain (see the
per-opcode subsections below) -- and the **runtime side is now
implemented (#496)**: a bound chain's stages transform the stream's
samples through the GD32 FAC (FIR/IIR) or FFT hardware block in a
base-level pump.  What a bound chain does to the reads:

* **FIR/IIR terminal:** `CMD_ADC_STREAM_READ` returns FILTERED mV
  (same wire format as the raw read) drained from the pump's
  processed ring.
* **FFT terminal:** `CMD_ADC_STREAM_READ` returns `STATUS_NOSUPPORT`;
  the spectrum is pulled with `CMD_ADC_SPECTRUM_READ` (`0x3A`).

One FAC and one FFT block exist.  A second `chain_bind` while the
matching block is already serving another bound stream returns
`STATUS_NOSUPPORT`: a FIR/IIR (FAC-terminal) chain against a busy FAC,
and, since protocol v0.10 (`gd32-bridge-firmware` PR #121), an
FFT-terminal chain against a busy FFT block
(`adc_dsp_fft_stream_busy()` in `gd32-bridge-firmware:hal/gd32/adc_dsp_chain.c`).
The refused chain stays open, so a host can wait for the other stream's
`stream_end` and retry the same `chain_id`.  The host-side
standalone API in `<alp/dsp.h>` ships working in v0.5.0 (runs the
chain locally with CMSIS-DSP or the portable C fallback over
in-RAM buffers), so application code can test against the same
chain primitives and, since v0.9 (#496), run the same workload
bridge-offloaded through the dispatch in
`bridge_hw_adc_stream_read()`
(`gd32-bridge-firmware:hal/gd32/adc_stream.c:253-279`) -- `<alp/dsp.h>`
remains the in-RAM host-side alternative when no bridge is present.

#### `CMD_ADC_DSP_CHAIN_OPEN` (`0x37`)

| Direction | Layout              |
|-----------|---------------------|
| Request   | (empty)             |
| Reply     | `chain_id:u8`       |

Allocates a fresh chain handle from the firmware's pool (size
`GD32G553_BRIDGE_ADC_DSP_MAX_CHAINS`, default 4 chains).  The
opaque `chain_id` is what the host passes to the subsequent
STAGE_PUSH and CHAIN_BIND calls.  Exhausting the pool returns
`STATUS_NOSUPPORT` (`0x06`) -- the firmware maps
`BRIDGE_HW_ERR_NOTIMPL` there because `hal/bridge_hw.h` has no
NOMEM-equivalent `BRIDGE_HW_ERR_*` for this path
(`gd32-bridge-firmware:hal/gd32/adc_stream.c:547-552`).  A host must
not read that `0x06` as "opcode unknown".  Chains auto-release when
the bound stream's
`CMD_ADC_STREAM_END` runs; there is no explicit `CHAIN_CLOSE` at
v0.5.

#### `CMD_ADC_DSP_STAGE_PUSH` (`0x38`)

| Direction | Layout                                                                                       |
|-----------|----------------------------------------------------------------------------------------------|
| Request   | `chain_id:u8 stage_index:u8 kind:u8 chunk_offset:u16 chunk_total_size:u16 chunk_data[0..58]` |
| Reply     | (empty)                                                                                      |

Uploads one chunk of one stage's per-kind parameter blob into the
named chain at `stage_index`.  The 7-byte header carries:

* `chain_id` -- handle from CHAIN_OPEN.
* `stage_index` -- 0..`GD32G553_BRIDGE_ADC_DSP_MAX_STAGES-1` (default 0..3).
* `kind` -- FIR (`0`), IIR (`1`), WINDOW (`2`), FFT (`3`).
* `chunk_offset` -- byte offset of this chunk's payload within the
  full per-kind blob.  Little-endian u16.
* `chunk_total_size` -- total size of the full per-kind blob (not
  this chunk's length).  Little-endian u16.  Lets the firmware
  know when assembly is complete.

The remaining 0..`GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES` (58)
bytes of `chunk_data` are appended at `chunk_offset` into the
firmware's per-stage scratch buffer.  The host helper
`gd32g553_adc_dsp_stage_push` splits oversized blobs into
back-to-back STAGE_PUSH calls automatically; firmware accepts
chunks in any order as long as the full `[0, chunk_total_size)`
range is eventually covered before the chain is bound.

Reassembled per-kind blob layouts (what the firmware decodes
once the chunks have all landed):

| Kind     | Layout                                                            | Max size            |
|----------|-------------------------------------------------------------------|---------------------|
| FIR (0)  | `format:u8 n_taps:u8 reserved:u16 taps[n_taps * 4]`               | 4 + 4 * 64 = 260 B  |
| IIR (1)  | `format:u8 n_sections:u8 reserved:u16 coeffs[n_sections * 5 * 4]` | 4 + 4 * 5 * 8 = 164 B |
| WINDOW (2) | `shape:u8 reserved[3]`                                          | 4 B                 |
| FFT (3)  | `n_points:u16 output_format:u8 reserved:u8`                       | 4 B                 |

`format` is `0` for Q31 fixed-point coefficients, `1` for
IEEE-754 single-precision (matches `alp_dsp_coeff_format_t` in
`<alp/dsp.h>`).  `shape` is one of `0` (rectangular), `1` (Hann),
`2` (Hamming), `3` (Blackman) (matches `alp_dsp_window_kind_t`).
`output_format` is `0` (interleaved complex re/im pairs) or `1`
(per-bin magnitude only) (matches `alp_dsp_fft_output_t`).

Firmware-side validation runs at CHAIN_BIND time, not on each
STAGE_PUSH -- so a half-uploaded chain is OK as long as the host
eventually completes every staged kind before binding.

#### `CMD_ADC_DSP_CHAIN_BIND` (`0x39`)

| Direction | Layout                       |
|-----------|------------------------------|
| Request   | `chain_id:u8 stream_id:u8`   |
| Reply     | (empty)                      |

Attaches a fully-populated chain to a streaming ADC source that
was previously opened with `CMD_ADC_STREAM_BEGIN` (opcode
`0x33`).  The firmware validates the chain, stores the binding on
both sides, and **routes the stream's samples through the bound
chain at runtime (#496)**.  What the reads return then depends on
the chain's terminal stage:

* **FIR/IIR terminal:** `CMD_ADC_STREAM_READ` returns FILTERED mV
  (same `i16`/mV wire format as the raw read).  Wire Q31/F32
  coefficients are mapped to the FAC's Q15 at bind.
* **FFT terminal:** `CMD_ADC_STREAM_READ` returns `STATUS_NOSUPPORT`;
  the spectrum is read with `CMD_ADC_SPECTRUM_READ` (`0x3A`),
  formatted per the FFT stage's `output_format` (COMPLEX =
  interleaved `(re,im)` f32; MAGNITUDE / MAGNITUDE_ONESIDED =
  per-bin f32 magnitudes).

`CHAIN_BIND` fails (`STATUS_INVAL`) if:

* `stream_id` is out of range (`>= GD32G553_BRIDGE_ADC_STREAM_COUNT`),
* `chain_id` does not name an open chain,
* the chain has unfinished stages (some `[0, chunk_total_size)`
  range was not covered),
* the chain violates the ordering rules from `<alp/dsp.h>` (FFT
  must be terminal; WINDOW must immediately precede FFT).

It returns `STATUS_NOSUPPORT` when the single FAC (FIR/IIR) hardware
block, or (since v0.10) the single FFT block, is already serving
another bound stream of the same terminal class. The chain is not
released on that refusal; retrying the same `chain_id` after the other
stream ends is the supported recovery.

#### `CMD_ADC_SPECTRUM_READ` (`0x3A`)

| Direction | Layout                                                              |
|-----------|--------------------------------------------------------------------|
| Request   | `stream_id:u8 bin_offset:u16 max_bins:u8`                           |
| Reply     | `seq:u32 total_bins:u16 got:u8 bins[max_bins]:f32` (zero-padded)    |

Reads one chunk of the latest completed FFT frame for a stream bound
to an FFT-terminal chain.  The firmware runs the HW FFT on each full
N-point window and publishes the reduced bins (`total_bins` =
`N` complex-pairs*2 / `N` magnitude / `N/2+1` magnitude-onesided);
`seq` increments per frame so a host fetching a spectrum across
several chunks can detect a frame roll.  `max_bins` is capped at
`GD32G553_BRIDGE_ADC_SPECTRUM_READ_MAX` (14) to keep the fixed reply
inside the wire envelope.  Returns `STATUS_NOSUPPORT` if the stream
isn't FFT-bound, `STATUS_BUSY` before the first frame completes.

#### Tombstone: `CMD_ADC_STREAM_CONFIGURE_DSP` (`0x36`)

The original single-shot configure opcode `0x36` is retained as a
**reserved tombstone**.  The 65-byte wire envelope can't fit a
single FIR stage's 256-byte Q31-tap blob in one shot, so the
chunked upload via `0x37`..`0x39` is the actual mechanism.  The
firmware default-case path returns `STATUS_NOSUPPORT` for `0x36`
indefinitely; host code SHOULD NOT call it.

### 3.1 GPIO masks

`mask` selects which GD32 pads the host wants to read or write.  The
mask is a **logical** index space owned by the GD32 firmware — the
bit-to-pad mapping (bits 0..26) is documented in
`gd32-bridge-firmware:README.md`; the host header names only bits 18/19
and 21/22 (below).  The host MUST NOT assume that
bit `n` corresponds to GD32 pad `Pxn`.

`GPIO_WRITE` is atomic in the firmware: read-modify-write of the
pad output register is done with interrupts disabled around the
masked update so a concurrent `GPIO_WRITE` on the other transport
cannot interleave a partial state.

At protocol minor `>= 11` (firmware `0.2.12`), the pad map grows from
18 to 20 lines, adding the on-module Murata LBEE5HY2FY-922 (Infineon
CYW55513) Wi-Fi+BT module's two REG_ON enables:

| Bit | Name       | GD32 pad | Boot state   | Host macro |
|-----|------------|----------|--------------|------------|
| 18  | `bt-reg-on` | `PE14`  | OUTPUT LOW   | `GD32G553_GPIO_LINE_BT_REG_ON` |
| 19  | `wl-reg-on` | `PE15`  | OUTPUT LOW   | `GD32G553_GPIO_LINE_WL_REG_ON` |

Both enables drive their module low-then-high: the host holds
`GPIO_WRITE` low for >= 10 ms before the rising edge, matching the
on-module Murata LBEE5HY2FY-922's REG_ON timing requirement.

The Linux `gpio-gd32-bridge` driver additionally exports line 21 `se-rst` (line 20 is
reserved for `can-stby`, bridge bit 20, #2341), which is not a `GPIO_WRITE` pad: setting it sends `CMD_SE_RESET`
(`0x41`, payload one byte, 1 = assert = hold the OPTIGA Trust M in reset) and
it is never replayed, so a bridge reset leaves the part released. Userspace
pulses it through the gpiochip labelled `gd32-bridge-gpio` instead of opening
the bridge itself (#2507).

At protocol minor `>= 15` (firmware `0.3.1`), the pad map grows from 21 to 23
lines, adding two ordinary E1M pads after the sideband block (no earlier bit
moves; bit 20 is `CAN_STBY`):

| Bit | E1M pad | GD32 pad | Boot state          | Host macro |
|-----|---------|----------|---------------------|------------|
| 21  | `IO15`  | `PB4`    | analog, no drive (PB4 parked explicitly) | `GD32G553_GPIO_LINE_E1M_IO15` |
| 22  | `IO26`  | `PC2`    | analog, no drive    | `GD32G553_GPIO_LINE_E1M_IO26` |

`PB4` resets as the JTAG `NJTRST` pin (alternate function, pull-up) on GD32
parts, so firmware 0.3.1 parks it analog / no pull at boot; debug access on
this board is SWD only. The `GD32G553_GPIO_LINE_*` host macros are bridge
bit numbers, not Linux gpiochip line numbers.

At protocol minor `>= 17` (firmware `0.17`), the pad map grows from 23 to 27
lines, adding the four SoM camera LDO enables right after `IO26` (SoM
power-supply sheet signals, not E1M-X pads; no earlier bit moves).  The bit
numbers below match firmware branch `feat/i2c3-proxy-cam-ldo`:

| Bit | Signal         | GD32 pad | Host macro |
|-----|----------------|----------|------------|
| 23  | `CAM_EN_LDO0`  | `PC3`    | `GD32G553_GPIO_LINE_CAM_EN_LDO0` |
| 24  | `CAM_EN_LDO1`  | `PE8`    | `GD32G553_GPIO_LINE_CAM_EN_LDO1` |
| 25  | `CAM_EN_LDO2`  | `PE7`    | `GD32G553_GPIO_LINE_CAM_EN_LDO2` |
| 26  | `CAM_EN_LDO3`  | `PE10`   | `GD32G553_GPIO_LINE_CAM_EN_LDO3` |

A host below `GD32G553_I2CM_MIN_PROTOCOL_MINOR` (17) never learns these bits.

Like every other E1M pad they stay undriven until a host first reads or writes
them. `PC14` (E1M IO24) is not part of this change. A bridge below minor 15
ignores bits 21/22 and still answers success, so `gd32g553_gpio_read` /
`gd32g553_gpio_write` return `ALP_ERR_NOSUPPORT` for a mask naming either bit
when the cached protocol minor is below
`GD32G553_IO15_IO26_MIN_PROTOCOL_MINOR` (15), and the Linux
`gpio-gd32-bridge` driver refuses `.request()` of lines 22/23 (`-ENODEV`)
until `GET_VERSION` confirms it (`-EAGAIN` while the bridge has not answered
yet; no kernel consumer uses these lines, so `-EPROBE_DEFER` would only leak
errno 517 to userspace). Linux line numbers are one above the bridge bit for
these two pads because line 21 is `se-rst` (below): line 22 = IO15 (bit 21),
line 23 = IO26 (bit 22). `se-rst` keeps line 21 because its line number is
already consumed by the optiga reset DT and HIL spec; renumbering it would
churn a stable userspace-visible line for no functional gain.

A bridge below minor 11 never learned these two bits; a host driving
`GPIO_WRITE` against them on such a bridge silently powers nothing
while the firmware reports success. This is true only through protocol
0.12: firmware 0.2.16 / protocol 0.13 rejects a write to an unknown pad
bit outright (Refs #2341) rather than reporting success. The Linux
`gpio-gd32-bridge`
kernel driver resolves this at first *consumer* request rather than
once at `probe()` (a single best-effort `GET_VERSION` at boot
consistently races the bridge's own startup) -- see
`GD32G553_REG_ON_MIN_PROTOCOL_MINOR` in
[`include/alp/chips/gd32g553.h`](../include/alp/chips/gd32g553.h).

**Host-behaviour note (Refs #2297; bug bench-observed 2026-09-26,
E1M-V2M103; fix bench-verified 2026-09-26 on E1M-V2M103 with GD32_NRST
held ~40 s past probe: the resolve poller's fixed 1 Hz ticks (1..30 s,
then 31, 33, 37, 45, 61 s ...) mean the bridge was first observed by
the 45 s poll — SDIO card enumerated 48.4 s, brcmfmac firmware 49.7 s,
hci0 UP+RUNNING 54.8 s, devices_deferred empty, no rebind; a normal
boot is unchanged):** `.request()`'s single non-blocking `GET_VERSION` attempt can
end in one of three states -- confirmed supported, confirmed
unsupported (a bridge that answered and reported a minor below 11 or
an unexpected major), or *still unresolved* because the bridge hasn't
answered at all yet. On the bench, that third case used to leave
`mmc-pwrseq-simple`'s `wlan-pwrseq` (line 19) and `hci_bcm`'s
`shutdown-gpios` (line 18) consumer sitting in
`/sys/kernel/debug/devices_deferred` for the whole boot even though the
bridge answered seconds later, because a bare `-EPROBE_DEFER` only
gets retried by the kernel when some *other* driver's (un)registration
happens to walk the deferred-probe list -- nothing guarantees that
happens on its own. `.request()` must still return `-EPROBE_DEFER`
rather than grant the request in that still-unresolved case, though:
`mmc-pwrseq-simple` and `hci_bcm` each run a one-shot power-up sequence
(`mmc_rescan()` / loading the `.hcd` patch) immediately once
`.request()` succeeds, and SDHI2 being non-removable means that
sequence never reruns on its own if it runs before REG_ON can actually
move.

What resolves a slow bridge instead: a second, independent poll -- a
dedicated `delayed_work` polls `GET_VERSION` regardless of whether any
consumer has requested the lines yet (flat at ~1 Hz for the first
~30 s, then backing off exponentially to a 30 s cap). This poller is
terminal, not perpetual: it stops rescheduling itself the moment it
gets a definitive answer (confirmed supported, or confirmed
unsupported) and kicks deferred probing once. It does not keep running
afterwards to notice a later bridge reset or OTA A/B swap -- nothing
re-resolves the lines-18/19 *capability* answer once this poller has
settled it; only the separate output-state replay below keeps
re-applying pad *levels* after a reset. On its first definitive answer
it briefly registers a throwaway `platform_device` purely so that
device's bind runs `driver_bound()` -> the kernel's own
`driver_deferred_probe_trigger()` (`drivers/base/dd.c`, called from
`driver_bound()` on every successful bind) -- a convenient in-tree
hook module code can use to queue a deferred-probe retry
(asynchronous, on `system_unbound_wq`) on demand, not the only such
mechanism (`device_reprobe()` / `bus_rescan_devices()` /
`wait_for_device_probe()` also exist). That gets `mmc-pwrseq-simple` /
`hci_bcm` to retry `.request()` with
the real answer -- granted, or failing outright with `-ENODEV` --
instead of lingering deferred indefinitely. A confirmed `-ENODEV`
is cached for the life of the driver instance (until the next reboot);
it is not re-checked even across a bridge OTA A/B swap, since a
firmware update that adds REG_ON support mid-boot is not a scenario
this driver defends against today.

**Any GD32 reset drops both lines low again** (WDT, fault, OTA A/B
swap, SE reset -- the boot-time OUTPUT LOW default applies on every
reset), which power-cycles the module. The host must re-assert them
after a bridge reset, not only at first bring-up.

The Linux `gpio-gd32-bridge` driver does this without reset detection
(`CMD_RESET_REASON` is clear-on-read on the firmware side, so polling
it would itself lose the very information a *second* poller needs):
every line ever written is latched host-side in `output_mask`/
`output_vals` **before** the write is attempted, so a failed transfer
never loses the requested state, and a `delayed_work` re-issues one
idempotent `GPIO_WRITE(output_mask, output_vals)` roughly once a
second whenever `output_mask != 0`. Re-asserting an already-correct
level is glitch-free, so this needs no reset detection at all -- it
just needs to run often enough that a bridge coming back up (first
bring-up, or after any reset) is re-promoted within about one period.
This costs one small I2C frame/s on BRD_I2C (`i2c8`) while any line is
held as output; see `gd32_bridge_replay_work()` (Refs #2297).

Only the Linux `gpio-gd32-bridge` driver does this. The CM33/Zephyr side
of the bridge does not yet re-assert anything after a reset, so #2297
stays open for that half. Also note what re-asserting REG_ON does and
does not fix: it restores the *pin level* only. The Linux Wi-Fi/BT
drivers (`cyw-fmac` etc.) are not re-initialised by this replay -- a
GD32 reset that also wedges or resets the Murata module itself still
needs the normal Linux driver-level recovery, on top of the pin being
re-asserted.

**I2C error replies and multi-line requests (Linux driver):** an error
reply on I2C is `[STATUS][CRC]` (3 bytes, no payload), so the driver decodes
that short shape before judging the full-width CRC, and maps the status to an
errno (`BUSY` -> `-EBUSY`, retried a bounded number of times; `NOT_READY` ->
`-EAGAIN`; `TIMEOUT` -> `-ETIMEDOUT`; `NOSUPPORT` -> `-EOPNOTSUPP`). The
gpiochip implements `.get_multiple` / `.set_multiple`, so a multi-line request
is one `GPIO_READ` / `GPIO_WRITE` transaction (line 21, `SE_RST`, stays a
separate `SE_RESET`).

**Shared-bus caveat:** BRD_I2C (`i2c8`) may be multi-mastered -- the
CM33 also owns a device on it (DA9292 @ `0x1E`). Arbitration loss on a
contended bus surfaces to the replay as an ordinary transfer failure
(the "output state replay failed" warning below), indistinguishable
from the bridge simply being down. A wedged bus is retried at the same
~1 Hz rate with no backoff, so a stuck bus gets one failing frame per
second indefinitely rather than escalating or giving up.

### 3.2 PWM channels

PWM channel ids are an **opaque enum** assigned by the GD32 firmware.
Mapping to GD32 timer + GTIOC pad lives in the firmware's
`pwm_channel_map[]`.  On the V2N base SoM **all eight** E1M PWM
channels (PWM0–PWM7) are driven by the GD32 IO MCU — the Renesas
drives none.  See `pwm_routing` in
[`metadata/chips/gd32g553.yaml`](../metadata/chips/gd32g553.yaml)
for the per-channel `e1m` → GD32 timer + pad map (the single source
of truth).

> **The PERIOD is shared per underlying timer** (one 16-bit ARR per
> timer; duty is per-channel).  PWM0–3 ride TIMER0 and PWM4–7 ride
> TIMER7, so a `PWM_SET` on any channel of a bank silently re-tunes
> the period of **every** channel in that bank — last write wins
> (1 kHz on PWM0 followed by 25 kHz on PWM2 leaves PWM0 at 25 kHz
> with its programmed duty *ticks* rescaled to the new period).
> `PWM_GET` reads live registers, so it always reports the
> bank-shared truth.  Keep co-resident consumers of one bank on a
> common period, or split them across the two banks.

### 3.3 ADC samples

`samples = 0` is invalid (reply: `ALP_ERR_INVAL`).  `samples >
GD32G553_BRIDGE_ADC_MAX_SAMPLES` (firmware-defined, currently 8) is
rejected with `STATUS_OUT_OF_RANGE` -- the firmware does NOT silently
cap, because the host driver compares the echoed `samples` byte
against the originally-requested count and treats a mismatch as a
wire error.  Callers that want N-sample averaging at higher fan-out
should issue multiple `ADC_READ` opcodes and accumulate on the
host side.  Each `mv[i]` carries the firmware's internal-reference-
corrected reading; the host treats the values as **ground truth**
for telemetry purposes.

Two converter-level guards (fw `v0.2.8+`) make `ADC_READ` answer
`STATUS_IO` instead of serving unreliable data:

- **converter owned by a stream** -- two logical channels ride each
  ADC converter, and a running `ADC_STREAM_*` session owns its
  converter outright.  A single-shot read on either channel of a
  streaming converter is refused (it would corrupt the live stream's
  ring AND return wrong data); retry after `STREAM_END`.
- **analog reference not ready** -- if the on-chip reference buffer
  never reported ready at boot, every conversion would be garbage
  referenced to a dead node.  The firmware fails the read loudly
  rather than answering `STATUS_OK` with bogus millivolts, and
  re-probes the reference on each attempt so a late-locking buffer
  self-heals.

### 3.4 DA9292 status forward

The GD32 has **no I2C path** to the DA9292, and on the **current SoM
revision it has no wiring to the DA9292 fault pins either**
(schematic-verified 2026-06-04: the `DA9292_INT` / `DA9292_TW` nets
land only on Renesas pads `P37` / `P36`; the GD32 schematic page
carries no DA9292 net).  Firmware on this revision therefore
**always returns the `0xFF` "no sample" sentinel**.

The reply packing below is reserved for a future HW revision that
mirrors the fault nets onto GD32 inputs:

| Bit   | Meaning                                        |
|-------|------------------------------------------------|
| 0     | `DA9292_INT` asserted (active-low net)         |
| 1     | `DA9292_TW` asserted (active-low net)          |
| 2–6   | reserved (0)                                   |
| —     | `0xFF` = "no sample available" sentinel        |

On today's hardware the host observes the fault pins **directly**:
`DA9292_INT` (Renesas `P37`) and `DA9292_TW` (Renesas `P36`) are
CM33-readable GPIO inputs — `da9292_get_fault_pins()` in the
`chips/da9292` driver packs them with the same bit layout, so host
code written against this byte works unchanged when a future HW rev
lets the bridge serve it.  This byte is **not** `PMC_STATUS_00` and
does not mirror its bit layout.  For register-level PMIC status
(`PMC_STATUS_00` etc.) the host reads the DA9292 directly over
`BRD_I2C` from the Cortex-A55 (Linux, or U-Boot for the DEEPX-rail
bring-up sequence) via `da9292_get_status()` in the `chips/da9292`
driver — RIIC8/BRD_I2C is Cortex-A55-exclusive, and the CM33 must
never master it — see `<alp/chips/da9292.h>`.

### 3.5 DAC outputs (`v0.2+`)

The V2N module routes both E1M DAC channels (`DAC0` → GD32 `PA4`,
`DAC1` → GD32 `PA6`) through the bridge.  `DAC_SET` programs the
output in millivolts; the firmware rounds to the GD32's 12-bit DAC
resolution (~0.44 mV/LSB on the module's 1.8 V analog rail) and
saturates above the rail.  `DAC_GET` reads back the DAC's live hold
register (the code actually driving the pad) — useful for
verification + closed-loop telemetry.

Both opcodes are live on silicon from fw `v0.2.6` (which also
brings up the on-chip analog reference buffer the converters
depend on; DAC→ADC copper loopback validated 1:1 on the bench
2026-06-05).  From fw `v0.2.8`, a reference buffer that never
reports ready makes both opcodes answer `STATUS_IO` — a loud
failure instead of silently driving/reading garbage against a dead
reference node.

### 3.6 Quadrature encoders (`v0.2+`)

The four E1M encoders (`ENC0..ENC3`) are all GD32-driven on V2N
(GD32 pad pairs `PA0/PB3`, `PC6/PC7`, `PB6/PB5`, `PB2/PA1`).
`QENC_READ` returns the signed accumulated count since the last
reset; the firmware uses a 32-bit accumulator that wraps modulo
2³² rather than saturating, so callers can subtract two snapshots
to get velocity even across a wrap.  `QENC_RESET` zeroes the
accumulator atomically (encoder pulses arriving during the reset
window contribute to the post-reset count, not the pre-reset one).

### 3.8 PWM configure (`v0.3+`)

`PWM_CONFIGURE` programs sticky per-channel knobs that subsequent
`PWM_SET` calls honour: counter alignment mode (edge-aligned vs
center-aligned-up / -down / -both), dead-time for complementary
outputs (in ns; firmware rounds to the timer's achievable step), and
break-input enable.  Request payload is 7 bytes:
`channel:u8 align_mode:u8 dead_time_ns:u32 break_cfg:u8` -- reply
is empty.

On V2N every E1M PWM channel maps to a TIMER0 / TIMER7 channel
(see `metadata/chips/gd32g553.yaml` `pwm_routing:` for the table).
Both timers are 16-bit advanced timers running at the 216 MHz
CK_TIMER (= CK_APB at DIV1 = the core clock; this part has no
separate timer PLL).  Unprescaled that would be a ~4.63 ns LSB and a
~303 us longest single-counter period -- but **the firmware never runs
the timer unprescaled**, so neither figure is the achievable limit.  It
programs a fixed 216:1 prescale to a 1 MHz counter tick
(`PWM_TIMER_PRESCALER` / `PWM_TIMER_TICK_NS` in the `gd32-bridge-firmware`
repo), giving a **1 us LSB and a 65.536 ms** longest period at full
16-bit count.  The 65.536 ms figure is the one this document already
quotes below as the boot default, and the one an over-long `PWM_SET` is
silently clamped to (#1730).  `CMD_PWM_GET` reads the live
timer registers (auto-reload + compare) and converts ticks back to
nanoseconds -- it reports what the pad is actually generating, never
an echo of the request.  Two consequences of the hardware truth:
the period is shared per timer (a `PWM_SET` on a sibling channel of
the same timer moves this channel's reported period too), and before
the first `PWM_SET` a channel reports the boot default (65.536 ms
period, 0 duty).  Callers that need exact frequency confirmation
should read back rather than recompute.

**Current GD32 HAL status (#495: alignment landed):**
`bridge_hw_pwm_configure()` applies `align_mode` (`0` edge, `1`
center-up, `2` center-down, `3` center-both) to the channel's timer.
`CAM` is timer-wide, so the mode is shared by the four sibling channels
of a `TIMER0`/`TIMER7` (last write wins) and `PWM_SET`/`PWM_GET` convert
period/duty accordingly (a center-aligned counter runs `0->ARR->0`, so
period is `2*ARR` ticks).  An out-of-range `align_mode` returns
`STATUS_INVAL`.  Two consequences of the shared, doubled counter worth
noting: switching a timer to a center-aligned mode **re-times any
sibling channel already running** (its physical period doubles until
the host re-issues `PWM_SET`), and center-aligned period/duty quantise
to `2 us` (the `ARR`/compare are the commanded microseconds halved), so
the minimum non-zero duty is `2 us` and odd values round down.
`PWM_SINGLE_PULSE` (`0x26`) is edge-aligned only and returns
`STATUS_NOSUPPORT` while its timer is center-aligned -- set `align_mode`
back to `0` first.

`dead_time_ns` and `break_cfg` still return `STATUS_NOSUPPORT` -- but on
V2N this is a **hardware-routing** limit, not an unimplemented feature.
The E1M PWM connector exposes only each channel's *complementary* output
(`CHxN`); the main `CHx` is not routed, so there is no complementary
pair for a dead-time gap to act on.  Likewise the V2N
`gd32-io-mcu-map.tsv` routes no `BRK` pad, so break-input logic could be
armed but never triggered.  Both knobs are therefore physically inert on
this board; a future carrier that routes the complementary pair / a
`BRK` pad would lift the restriction.

### 3.9 ADC configure (`v0.3+`)

`ADC_CONFIGURE` programs sticky per-channel oversampling +
sample-and-hold cycle count + resolution.  Subsequent `ADC_READ`
calls on that channel honour the configured tuning.  Request payload
is 7 bytes:
`channel:u8 reserved:u8 oversample_ratio:u16 sample_cycles:u16 resolution_bits:u8` --
reply is empty.

* `oversample_ratio` is one of 1/2/4/8/16/32/64/128/256.  0 means
  "firmware default" (per-channel-configured at build time).  The
  firmware rounds down to the nearest power-of-two.
* `sample_cycles` is the raw RSMP value in ADCCK cycles (sample time =
  value + 2.5 cycles; the vendor `adc_routine_channel_config`
  `sample_time` argument), not microseconds and not a rung selector; the
  firmware clamps it into `2..638`.  `0` means "firmware default"
  (240 cycles) -- a `0` here is NOT the fastest window, so an
  oversample-only reconfigure keeps the settling time its high-Z
  inputs need.
* `resolution_bits` is 0 (default = 12) / 6 / 8 / 10 / 12 (hardware
  `DRES`); 14 / 16 are accepted on the wire but unsupported.  The mV conversion divides by the width's
  full-scale (`4095`/`1023`/`255`/`63` for 12/10/8/6).  `14`/`16` are
  not supported by the GD32G553 (the `DRES` field tops out at 12-bit),
  so they reply `STATUS_NOSUPPORT`.  Any
  other width replies `STATUS_INVAL`.

The GD32 returns ADC readings as 16-bit millivolts (`ADC_READ` /
`ADC_STREAM_READ` reply payload format unchanged); higher
oversampling improves the effective-resolution / SNR of the
returned values but doesn't widen the on-wire word.

**Current GD32 HAL status (#494 landed):** `sample_cycles`,
`oversample_ratio`, and `resolution_bits` are all sticky.
`bridge_hw_adc_configure()` programs the channel's cached format into
the converter on the next `ADC_READ` / `ADC_STREAM_BEGIN` (inside the
`ADCON == 0` window `DRES`/`OVSAMPCTL` require):

* `oversample_ratio` `0`/`1` disables oversampling; any larger value is
  floored to the nearest power of two in `2..256` and applied with a
  matching `OVSS` right-shift so the result stays at the resolution's
  full-scale (the on-wire mV word is unchanged; oversampling improves
  SNR).
* `resolution_bits` `0` means default (12); `6`/`8`/`10`/`12` map to the
  hardware `DRES` field and the mV conversion divides by that width's
  full-scale (`4095`/`1023`/`255`/`63`).  `14`/`16` are
  not supported by the GD32G553 (the `DRES` field tops out at 12-bit),
  so they return `STATUS_NOSUPPORT`.
  Any other width returns `STATUS_INVAL`.

### 3.10 ADC streaming (`v0.3+`)

`ADC_STREAM_BEGIN` starts a DMA-backed continuous acquisition into
a firmware-side ring buffer at the requested `sample_rate_hz`.
Two streams supported concurrently -- stream 0 binds to GD32 DMA0,
stream 1 binds to DMA1, so they can run truly in parallel against
different channels at different rates.  Calling `STREAM_BEGIN` on a
stream slot that's already active replies with `STATUS_INVAL` (the
slot must be `STREAM_END`ed first).
Request payload is 7 bytes:
`stream_id:u8 channel:u8 reserved:u8 sample_rate_hz:u32`;
reply empty.

The sample rate is a real hardware contract (fw `v0.2.4+`): a
dedicated pacing timer per stream (update-event TRGO routed to the
converter's routine trigger) starts exactly one conversion per
period.  Accepted range is 1 Hz..100 kHz -- 0 answers
`STATUS_INVAL`, above the cap answers `STATUS_OUT_OF_RANGE`.  The
rate quantises to the pacer tick (1 us at 16 Hz and above, 100 us
below 16 Hz), truncating: e.g. 300 Hz runs at 1 MHz/3333 ticks =
300.03 Hz.  A rate whose period is shorter than the channel's
configured sample-and-hold time degrades gracefully -- the silicon
ignores trigger edges that land mid-conversion, so the stream runs
at the channel's achievable rate instead of corrupting data.  Two
constraints fall out of the silicon's converter-level trigger
routing: only one stream may run per ADC converter at a time (a
second `BEGIN` whose channel shares the first stream's converter
answers `STATUS_INVAL`), and `STREAM_END` restores the converter's
single-shot state for subsequent `ADC_READ` calls.  While a stream
runs, a single-shot `ADC_READ` on **either** channel of its
converter answers `STATUS_IO` (fw `v0.2.8+`) -- retry after
`STREAM_END`.  `STREAM_END` itself answers `STATUS_IO` in the rare
case the converter's restore re-calibration never completes (the
stream still tears down; the converter is back but in an unproven
state -- the next `ADC_READ`'s own timeout/self-heal path arbitrates
from there).

`ADC_STREAM_READ` drains up to `max_samples` samples from the
named stream's ring.  The wire envelope is fixed-length (host
pre-commits to clocking `1 + max_samples*2 + 2` reply bytes
regardless of how many samples the firmware actually has ready);
reply byte 0 is the real count `got` (0..max_samples) and slots
beyond `got` are zero-padded.  Request payload is 2 bytes:
`stream_id:u8 max_samples:u8` (firmware caps at
`GD32_BRIDGE_ADC_STREAM_READ_MAX = 32`); reply payload is
`got:u8 mv[max_samples]:u16` with trailing zero-padded slots.

`ADC_STREAM_END` stops the named stream's DMA, flushes its ring,
and releases the DMA controller for re-binding.  Request payload
is 1 byte: `stream_id:u8`; reply empty.

Ring overrun on the firmware side returns `STATUS_BUSY` on the next
`STREAM_READ`, signalling "host should poll faster" -- the
firmware does NOT silently drop samples.  The firmware detects the
overrun exactly (a per-reload DMA lap counter tracks total samples
written vs total drained); the `STATUS_BUSY` answer also discards
the lapped (corrupt) backlog and resynchronises the read cursor to
the live write position, so the following `STREAM_READ` returns
fresh, gap-free samples.

On a v0.15 link that granted `ADC_STREAM2`, prefer `ADC_STREAM_BEGIN2` /
`ADC_STREAM_READ2` (§3.18): raw codes, a drop count and a sample index
instead of millivolts-and-`BUSY`.  The legacy opcodes stay available on
every link and behave exactly as above.

### 3.11 TRNG read (`v0.3+`)

`TRNG_READ` pulls true-random bytes from the GD32G5's NIST
SP800-90B pre-certified TRNG unit.  Request payload is 1 byte:
`len:u8` (1..32); reply payload is the `len` random bytes.  The
firmware accumulates 32-bit pulls (or 128-bit pulls on the NIST
path) until `len` is satisfied; latency at typical TRNG_CLK is
~40 cycles per pull (sub-microsecond).  Self-check failures
(documented in the GD32 user manual TRNG_STAT register) cause
the firmware to reply with `STATUS_IO`.

**Fault-recover contract** (fw `v0.2.4+`, silicon-validated
2026-06-04): a tripped self-check parks the unit with latched fault
flags and answers exactly **one** honest `STATUS_IO` while the
firmware demotes and lazily rebuilds the TRNG (full re-seed); the
**next** `TRNG_READ` succeeds.  Callers should therefore retry once
on `STATUS_IO` before treating the unit as down.  A reply of
`STATUS_BUSY` is different: the unit is healthy but mid-conditioning
(a max-length pull can drain the FIFO) -- poll again.

### 3.12 TMU compute (`v0.4+`)

`TMU_COMPUTE` issues one operation against the GD32G5's TMU
(Trigonometric Math Unit -- the chip's CORDIC engine).  The TMU is a
**general-purpose** math accelerator (sin / cos / tan / atan / atan2
/ sqrt / log / exp / sinh / cosh / tanh / vector magnitude) -- it is
NOT an ADC post-processor, and the opcode does not interact with the
streaming ADC pipeline.  Request payload is 12 bytes:

```
   offset  size  field
   ------  ----  -----------------------
   0       u8    function    (see table)
   1       u8    format      (0 = Q31, 1 = IEEE-754 single)
   2..3    u16   reserved (MUST be 0)
   4..7    u32   in_a        (first operand, format-dependent encoding)
   8..11   u32   in_b        (second operand for two-input functions;
                              ignored for one-input functions)
```

Reply payload is 4 bytes (`result:u32`) in the same format as the
inputs.  The status byte carries the usual `STATUS_OK` /
`STATUS_OUT_OF_RANGE` (operand outside the function's mathematical
domain, e.g. `sqrt(-1)` in Q31) / `STATUS_INVAL` (bad function or
format enum) / `STATUS_NOSUPPORT` (firmware HAL stub) /
`STATUS_IO` (TMU fault).

Function table (`function` field):

| Code | Mnemonic | Inputs | Output                                       |
|------|----------|--------|----------------------------------------------|
| `0`  | `SIN`    | 1      | `sin(in_a)`                                  |
| `1`  | `COS`    | 1      | `cos(in_a)`                                  |
| `2`  | `TAN`    | 1      | `tan(in_a)`                                  |
| `3`  | `ATAN`   | 1      | `atan(in_a)`                                 |
| `4`  | `ATAN2`  | 2      | `atan2(in_a, in_b)` (y, x)                   |
| `5`  | `SQRT`   | 1      | `sqrt(in_a)` (non-negative input)            |
| `6`  | `LOG`    | 1      | `log(in_a)` (natural log; positive input)    |
| `7`  | `EXP`    | 1      | `exp(in_a)`                                  |
| `8`  | `SINH`   | 1      | `sinh(in_a)`                                 |
| `9`  | `COSH`   | 1      | `cosh(in_a)`                                 |
| `10` | `TANH`   | 1      | `tanh(in_a)`                                 |
| `11` | `HYPOT`  | 2      | `sqrt(in_a*in_a + in_b*in_b)` (no overflow)  |

Single-input functions MUST set `in_b = 0`; the firmware does not
inspect it but a non-zero value MAY be rejected by future revisions.

The format byte picks the number-encoding of both inputs and the
output: `format = 0` is Q31 fixed-point (full-scale = ±1.0; for trig
functions, angles are in units of pi); `format = 1` is IEEE-754
single precision (binary32).  The SDK's portable `<alp/tmu.h>`
surface always uses IEEE-754 single because its public API takes
`float`; Q31 is available to applications that need to skip the
boundary conversion when working directly with the chip wrapper
`gd32g553_tmu_compute`.

Latency: one CORDIC iteration is ~14 chip cycles on the GD32G5; the
firmware blocks the bridge until the result is ready (typical
end-to-end is < 50 us on the SPI fast path).  Higher-throughput
designs that need to dispatch many TMU ops should batch them on the
host side rather than spinning per-op round-trips.

### 3.7 Free-running counter (`v0.2+`)

`COUNTER_READ` exposes one GD32 hardware counter whose tick rate is
firmware-defined.  The bridge does not yet advertise the tick
frequency on the wire; callers needing wall-clock conversion must
either (a) consult `gd32-bridge-firmware:README.md` for the firmware's
current tick configuration, or (b) wait for the v0.3 protocol
revision which will add `COUNTER_GET_FREQ`.  The SDK's portable
`alp_counter_us_to_ticks` returns `ALP_ERR_NOSUPPORT` on V2N until
the freq opcode lands.

Counter **alarms** are not in scope for the bridge.  The GD32 has
no interrupt line back to the Renesas host, so deadline callbacks
fired in firmware ISR context cannot be relayed across the bridge
in bounded time.  Apps that need alarms must either use a host-
local timer or poll `COUNTER_READ` and synthesise the callback
client-side.

### 3.14 Link-feature negotiation (`v0.7+`, extended in `v0.15`)

`LINK_FEATURES` (0x81) negotiates opt-in framing upgrades.  The host
sends the feature set it wants; the firmware grants the intersection
with what it implements **on that link**, **arms it immediately**, and
echoes the result.  Pre-v0.7 firmware answers `STATUS_NOSUPPORT`
through the dispatch default, so a new host degrades to the legacy
framing automatically — and an un-negotiated link is byte-identical to
the v0.14 wire.

The request has two accepted forms, chosen by the request payload
length:

| `req_len` | Layout | Semantics |
|-----------|--------|-----------|
| 1 (legacy, v0.7+) | `features:u8` | The link's whole feature word becomes `features & 0x01`; every other bit (`BIG_FRAME`, `ATTN`, `ADC_STREAM2`, `BATCH`) is **cleared** on that link, so `features = 0` still disables everything.  `max_payload` becomes 65.  The reply is `granted:u8` (1 byte), byte-identical to v0.14. |
| 6 (extended, v0.15) | `want:u32` @0, `max_payload_req:u16` @4 | Grant algorithm below.  The reply is 10 bytes. |
| anything else | n/a | `STATUS_INVAL`, empty payload, link state unchanged. |

Defined feature bits (bits 5..31 are reserved: never granted, the host
sends 0):

| Bit | Mask | Name | Effect when granted |
|-----|------|------|---------------------|
| 0 | `0x00000001` | `STATUS_SEQ` | Every **SPI** reply STATUS byte carries a 4-bit slave-side sequence stamp in bits `[7:4]` (§4.1.1).  The reply to the negotiation itself is already stamped — the host takes that stamp as its baseline.  I2C replies are never stamped. |
| 1 | `0x00000002` | `BIG_FRAME` | SPI only.  Raises the payload ceiling from 65 to `max_payload` (66..252) for **`BATCH` and the `ADC_STREAM_READ2` reply only** (§3.16, §3.18); every other opcode keeps 65.  The SPI frame is then at most 256 bytes (`1 SOF + 1 CMD/STATUS + 252 + 2 CRC`). |
| 2 | `0x00000004` | `ATTN` | SPI only; needs `STATUS_SEQ`.  The GD32 drives `PA14` (Renesas `P71`) HIGH when a reply is armed or stream events are pending (§3.17). |
| 3 | `0x00000008` | `ADC_STREAM2` | SPI only.  Gates `ADC_STREAM_BEGIN2` / `ADC_STREAM_READ2` (§3.18). |
| 4 | `0x00000010` | `BATCH` | SPI only.  Gates `BATCH` (§3.16). |

**Grant algorithm (extended form, arriving on link L).**

1. `supported(L)` depends on the build and the link: SPI with the gd32
   backend `0x0000001F`; SPI with the stub backend (no ATTN, no
   `ADC_STREAM2`) `0x00000013`; I2C on any backend `0x00000001`
   (`STATUS_SEQ` is echoed with no wire effect, preserving the v0.14
   idempotent `features = 0` path).
2. `g = want & supported(L)`.
3. Drop `ATTN` from `g` if `STATUS_SEQ` is not in `g`, or if
   `DHCSR.C_DEBUGEN` (`0xE000EDF0` bit 0) reads 1 — a debugger is
   attached and may be driving SWCLK on the same pin.
4. If `BIG_FRAME` is in `g`, `mp = clamp(max_payload_req, 65, 252)`; if
   that gives 65, `BIG_FRAME` is dropped.  Without `BIG_FRAME`, `mp = 65`.
5. **Arm before staging the reply**, as v0.7 already does: write L's
   feature word and `max_payload`, and apply the `ATTN` pin transition
   only if the `ATTN` bit actually changed (re-sending the same `want`
   causes no pin activity).
6. Stage the reply.  It is stamped if `STATUS_SEQ` is in `g`.  If `ATTN`
   is in `g`, this reply is the first ATTN-signalled one (the host's
   self-test, §3.17).  If `ATTN` was just removed, this reply is **not**
   ATTN-signalled; the host reads it with the v0.14 drain rule.

Removing a bit takes effect from the next request.  Removing
`ADC_STREAM2` does not stop running streams (`0x3B`/`0x3C` then answer
`STATUS_NOSUPPORT`, while `0x35` still ends a stream); removing
`BIG_FRAME` restores the 65-byte limit; removing `ATTN` returns `PA14` to
SWCLK.

**Extended reply (10 bytes):**

| Off | Field | Type | Meaning |
|-----|-------|------|---------|
| 0 | `granted` | u32 | bits now armed on L |
| 4 | `supported` | u32 | bits this build implements on L.  A bit in `supported` but not in `granted` was refused (missing `STATUS_SEQ`, debugger attached, `mp` = 65). |
| 8 | `max_payload` | u16 | effective payload ceiling on L (65, or 66..252) |

**Older firmware** answers the 6-byte form with `STATUS_INVAL` (v0.7..v0.14:
`req_len != 1`; a 4-byte error envelope, stamped if `STATUS_SEQ` was
already armed, no state change) or `STATUS_NOSUPPORT` (< v0.7).  The host
never sends the 6-byte form when `GET_VERSION` reports minor < 15 (§8);
`INVAL` is only a safety net.

After a firmware reboot every feature resets to off (replies revert to
unstamped, `PA14` returns to SWCLK); the host's normal link-recovery path
(re-init → `GET_VERSION` → negotiate) re-negotiates.  Any reset clears
every link feature: `NRST`, `FWDGT`, OTA `COMMIT`/`ROLLBACK`/trial
confirm, a fault reset, or a `STANDBY` wake.  During the OTA trial window
every opcode, `LINK_FEATURES` included, answers `BUSY`, so `ATTN` cannot
be enabled there.

### 3.15 Secure-element reset (`v0.8+`)

`SE_RESET` (`0x41`) drives the on-module secure element's reset line,
`SE_RST` = GD32 **`PC13`** (per
`metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`).  The SE is an **OPTIGA
Trust M** slave on the shared **BRD_I2C** management bus.  Request
payload is one byte:

| `assert` | Effect |
|----------|--------|
| `0`      | Release reset — the SE runs. |
| `1`      | Hold the SE in reset. |

The reply is empty.  The firmware owns the **active level** (OPTIGA
Trust M `RST` is **active-low**, so `assert=1` drives `PC13` low); the
host expresses intent, not polarity.  At boot the firmware parks the
line **released** so the SE runs and `PC13` no longer floats at its
GPIO power-on default.

**Bus-recovery use.**  The OPTIGA speaks I²C with clock-stretching; an
aborted APDU (e.g. the host warm-resets mid-transaction while the SE
keeps power) can leave it holding **SCL low**, wedging BRD_I2C for every
other master/slave (the 5L35023B PCIe-refclk generator, the PMICs, …).
The recovery is a **reset pulse**, sequenced by the host:

1. `SE_RESET assert=1` (hold in reset),
2. wait ≥ a few hundred µs,
3. `SE_RESET assert=0` (release),
4. wait ~15 ms for the OPTIGA to warm-boot before re-probing BRD_I2C.

Crucially this works **even when BRD_I2C is dead**: `PC13` is an
independent GPIO and the **SPI transport** reaches the GD32 while the
I²C bus is held low, so the host can always command the reset.

Returns `STATUS_INVAL` for an `assert` byte outside `{0, 1}` and
`STATUS_NOSUPPORT` on firmware whose GD32 HAL body is not built (the
stub backend).

### 3.16 `BATCH` (`v0.15+`)

`BATCH` (`0x04`) runs up to 16 bounded sub-operations in one SPI
transaction pair.  It is SPI-only and needs the `BATCH` link feature
(`STATUS_NOSUPPORT` otherwise).  On a `BIG_FRAME` link both the request
and the reply may use up to `max_payload` (252) bytes of payload.

**Request:** `count:u8` (1..16), followed by `count` entries
`{op:u8, len:u8, args[len]}`.  The request payload length must equal
`1 + Σ(2 + len_i)` exactly; any trailing byte is `STATUS_INVAL` (the
defence against zero-extended captures).

**Validation pass** (no side effects; a batch-level error is an
empty-payload envelope and nothing executes):

| Condition | Status |
|-----------|--------|
| `count == 0`, length mismatch, an op not on the allow-list (nested `0x04`, `0x81`, `0x28` and every `0xF0..0xFF` included), or `len_i` different from the op's fixed request length | `STATUS_INVAL` |
| `count > 16`, or worst-case reply `1 + Σ(2 + maxreply_i) > max_payload` | `STATUS_OUT_OF_RANGE` |

**Allow-list** (bounded, side-effect-local handlers):

| Op | Request len | Max reply |
|----|-------------|-----------|
| `0x00` PING | 0 | 0 |
| `0x10` GPIO_READ | 4 | 4 |
| `0x11` GPIO_WRITE | 8 | 0 |
| `0x20` PWM_SET | 10 | 0 |
| `0x21` PWM_GET | 1 | 8 |
| `0x24` PWM_CAPTURE_READ | 1 | 8 |
| `0x3C` ADC_STREAM_READ2 | 2 | `9 + 2 * max_samples` |
| `0x40` DA9292_STATUS_FORWARD | 0 | 1 |
| `0x50` DAC_SET | 4 | 0 |
| `0x51` DAC_GET | 1 | 2 |
| `0x60` QENC_READ | 1 | 4 |
| `0x61` QENC_RESET | 1 | 0 |
| `0x70` COUNTER_READ | 1 | 4 |
| `0x90` TMU_COMPUTE | 12 | 4 |

Excluded: `ADC_READ` (up to 1 ms each), the stream `BEGIN`/`END` variants
(calibration can spin for ~200000 iterations), `TRNG_READ` (budgeted
`DRDY` polls), `SE_RESET` (needs host-timed gaps), the DSP chain opcodes
(setup-time only), `LINK_FEATURES` (would change framing in the middle of a
reply), `POWER_MODE_SET`, OTA and nested `BATCH`.

**Execution.**  Ops run in order through the same handlers as standalone
ops.  Execution **stops at the first non-OK** sub-status.  The budget is at
most 16 ops × at most 20 µs per handler (to be measured per op before the
allow-list is frozen) plus ≤ 30 µs framing, ≈ 350 µs in the CS-EXTI
handler — below the 1000 µs `ADC_READ` budget; `I2C0` and base level stall
for that window, so `BRD_I2C` may be clock-stretched by up to about 350 µs.

**Reply.**  The outer `STATUS` is `OK` whenever the batch passed
validation and carries the one `STATUS_SEQ` stamp (sub-statuses are never
stamped).  Payload: `executed:u8` (ops attempted, including a failing one),
then per executed op `{status:u8, len:u8, payload[len]}`.  A non-OK
sub-status always has `len = 0`.  The length is self-delimiting and its
maximum can be computed from the request: the host clocks the worst case,
walks the entries to find the CRC (§4), and returns `ALP_ERR_IO` if
`executed > count`, if `len_i > maxreply_i`, or if the length of an op with
a fixed reply differs from its expected value.

Host API: `gd32g553_batch()`, which validates against the allow-list and
the link's ceiling before touching the bus.

### 3.17 ATTN (`v0.15+`) — data-ready / attention line

**Signal.**  GD32 `PA14` (net `GD32_SWCLK`, see
`metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`) drives Renesas `P71`
(`renesas-peripheral-map.tsv`).  Active **HIGH**, push-pull, a **level**
(not a pulse); low means deasserted.  GD32 drive: slowest speed class
(`GPIO_OSPEED_12MHZ`), no pull.  Fail-safe: whenever `ATTN` is not enabled,
`PA14` is `SWCLK` with its reset pull-down, so the host reads it as
deasserted.  `PA14` therefore has a **dual role**: `SWCLK` (debug / SWD
recovery) when `ATTN` is off, `ATTN` when it is granted.

**Ownership and the enable/disable handshake** (the GD32 never drives
`PA14` while a debugger may drive `SWCLK`):

Host rules:

* **H1.** In every boot stage, A55 pinctrl and CM33 init included, `P71`
  defaults to input (not driven).
* **H2.** The host sets `P71` to input with a rising-edge IRQ *before* it
  requests `ATTN`.
* **H3.** The host may drive `P70`/`P71` as outputs (SWD bit-bang) only if
  (a) its most recent CRC-valid `LINK_FEATURES` reply on SPI showed `ATTN`
  not in `granted` and it has sent no `LINK_FEATURES` since, or (b)
  `GD32_NRST` (`P74`, open-drain, shared with ACT88760 GPIO4) is held
  asserted (connect-under-reset).  The recovery procedure is always: first
  assert `P74`, then drive `P70`/`P71`.  Reset puts `PA13`/`PA14` back to
  SWD and the bootloader never touches `PA14`.  (`gd32_swd_init()` asserts
  `NRST` before it configures the SWD pins as outputs.)
* **H4.** Bench probes attach connect-under-reset only.
* **H5 (host SWD session).**  The host's own SWD recovery is
  connect-under-reset: `gd32_swd_init()` asserts `P74` first (switched to an output with an
  **initial low** level — the net is shared with the PMIC's open-drain
  reset-out and is never driven high; recovery without `NRST` is not
  supported), switches `P70`/`P71` to outputs, keeps `NRST` asserted through
  `gd32_swd_connect()`, which sets `DHCSR.C_DEBUGEN` and `DEMCR.VC_CORERESET`,
  releases `NRST` and reads `DHCSR.S_HALT` back (failing, with `NRST` asserted
  again, if the core did not halt), so the core stops at its reset vector with
  `PA13`/`PA14` still SWD and no application code run (ATTN cannot be granted
  during the session).  `gd32_swd_reset_and_run()` returns `P70`/`P71` to
  inputs **before** its final `NRST` release.  `gd32_swd_deinit()` returns
  `P70`/`P71` to inputs.  The driver opens the three pads itself: the reserved
  ids are refused by the portable `alp_gpio_open()`.  For the whole session
  the host's bridge link is closed and **blocked**: the V2N supervisor
  answers every bridge command `ALP_ERR_BUSY` and does not re-initialise or
  renegotiate until the session ends, after which the next init
  re-configures `P71` as input + rising-edge IRQ (never trusting a setup from
  before the session).

Firmware rules:

* **F1.** The firmware drives `PA14` only while `ATTN` is in the SPI link's
  feature word.
* **F2.** `ATTN` is never granted while `DHCSR.C_DEBUGEN` = 1.
* **F3.** Enable: write `GPIO_BC` for `PA14` (output low), then push-pull /
  slowest speed / no pull, then mode = output (no high glitch).  Disable:
  drive low, set `GPIO_AF_0`, then mode = AF with pull-down — restoring
  `PA14`'s reset SWD configuration (confirm against UM Rev1.2 GPIOA reset
  values).
* **F4.** `PA14` must never be added to the GPIOA lock mask (today
  `0x8700`; the lock is irreversible until reset).
* **F5.** Any reset clears every link feature and returns `PA14` to `SWCLK`.

**Semantics** (firmware state: `attn_on`, per-stream event bits `ev[0..1]`):

| Trigger | ATTN action |
|---------|-------------|
| CS falling (`PA8`, EXTI8 handler, low-level branch) | drive LOW |
| CS-rising handler entry | drive LOW (covers a coalesced falling edge; guarantees a low→high edge for every fresh reply) |
| CS-rising handler exit, after a **fresh** reply was staged and the TX DMA armed | drive HIGH.  "Fresh" = any call to `stage_reply()`: a decoded request, an error envelope, the SPI transport-error `IO`, or the tar-pit breaker |
| CS-rising handler exit with no fresh stage (drain / empty transaction / quiesce failure) | HIGH if any `ev[s]` is set, otherwise stay LOW |
| Watermark reached (raw ring: DMA HTF/FTF; FIR/IIR ring: base-level pump) | set `ev[s]`; then, inside a PRIMASK section, drive HIGH only if `PA8` reads high **and** `EXTI_PD0` bit 8 is clear (otherwise the next CS-rising exit re-evaluates) |
| `READ2` on stream `s` | clear `ev[s]`; set it again if the remaining backlog is still ≥ watermark |
| `STREAM_END` on `s`, or `ATTN` disabled | clear `ev[s]`; disable drives LOW and releases the pin (F3) |
| Any `POWER_MODE_SET` transition | drive LOW before entering (`STANDBY` is a reset) |

What the host infers: `ATTN` goes high after its own request transaction →
that reply is armed; `ATTN` goes high with no request outstanding → events
are pending.

**Host procedure.**

1. Drain the backend's edge latch (`hook.drain()`), read the edge clock
   (`hook.now()`), then clock the request.  The firmware drives `ATTN` low at
   CS falling and never raises it while CS is low, so every edge that belongs
   to this reply is time-stamped at or after that reading.  The backend's
   interrupt handler stamps each edge from the same free-running 32-bit clock
   (`k_cycle_get_32()` on Zephyr).  The drain keeps an old stamp from aliasing
   a fresh one after a counter wrap (a wrap-safe compare cannot tell "2^31+
   ticks old" from "in the future"); the portable SPI API has no CS-release
   instant, so a latch clear cannot be raced against the request instead.
2. Wait — interrupt plus semaphore, **never polling** — for a rising edge,
   up to `T_ATTN` = `GD32G553_BRIDGE_REPLY_TIMEOUT_MS` (10 ms).  Accept an
   edge only if (a) its stamp is not earlier than the clock reading **and** (b)
   `hook.read_level()` reads `ATTN` **HIGH** at accept time.  Rationale for
   (b): the firmware drives `ATTN` LOW at CS falling, so a stale rise in the
   short window between the clock reading and our own CS falling is always
   followed by a LOW before the wait begins (the wait starts after the request
   write returned), whereas a genuine reply leaves the line HIGH.  A rejected
   edge is discarded and the wait repeated (bounded); no accepted edge counts
   as a lost one.
3. Clock the reply with no staging gap.
4. Validate with `STATUS_SEQ`: `ATTN` is a hint, the stamp decides.  A
   stale stamp under `ATTN` triggers the same single re-send as §4.1.1; the
   RESET signature (stamp 0 after a non-zero baseline) means drop all
   negotiated state including `ATTN`, keep `P71` an input and re-run §8.

**Self-test at enable.**  The `LINK_FEATURES` reply that grants `ATTN` must
arrive on an edge.  If it does not, the host immediately re-sends the
extended form with `ATTN` cleared and treats `ATTN` as unusable until the
next `gd32g553_init()`.

**Event path.**  An edge while idle: issue `READ2` for every stream with a
watermark armed (one `BATCH` when two streams are armed).  See
`gd32g553_attn_wait_event()`.

**Lost or stuck ATTN.**

* *Lost* (no edge within `T_ATTN`): continue that command with the v0.14
  drain rule (25 µs → 1.6 ms re-read ladder) plus `STATUS_SEQ`.  After 3
  consecutive timeouts the host stops waiting on `ATTN` and re-negotiates
  without the `ATTN` bit, which also returns `PA14` to `SWCLK`.
* *Stuck high:* a violation is `ATTN` reading high when no request is in
  flight and no watermark stream is armed.  (The firmware spec defines it
  as "high when CS releases on a request whose CS-low time was ≥ 20 µs";
  the host driver samples the idle level before each request instead, as
  the portable SPI API cannot see the CS release.)  After 3 consecutive
  violations, or 8 consecutive idle-path edges where every armed stream
  returns `got = 0`, `dropped = 0`, take the same action as for lost `ATTN`.

Without `ATTN` the host's 35 µs staging gap grows by about 45 ns per byte
of request plus reply on `BATCH` / `READ2` frames (an estimate; the re-read
ladder covers the rest).

### 3.18 ADC streaming with exact accounting: `BEGIN2` / `READ2` (`v0.15+`)

Both opcodes are SPI-only and need `ADC_STREAM2` granted; otherwise they
answer `STATUS_NOSUPPORT`.

**`ADC_STREAM_BEGIN2` (`0x3B`) request (12 bytes):**

| Off | Field | Type | Rules |
|-----|-------|------|-------|
| 0 | `stream_id` | u8 | 0..1, else `INVAL` |
| 1 | `channel` | u8 | 0..7; an unmapped channel → `OUT_OF_RANGE` (as legacy) |
| 2 | `trigger_src` | u8 | `0x00` `PACE_TIMER`: the firmware-programmed timer, TIMER5 for stream 0 and TIMER6 for stream 1 (today's behaviour).  Reserved: `0x01` TIMER0_CC, `0x02` TIMER7_CC (arg = CC channel 0..3), `0x03` TIMER5_TRGO shared, `0x04` TIMER6_TRGO shared, `0x05` EXTI (arg = line 0..15).  `0x01..0x05` → `NOSUPPORT`; `≥ 0x06` → `INVAL` |
| 3 | `trigger_arg` | u8 | must be 0 for `PACE_TIMER`, else `INVAL` |
| 4 | `sample_rate_hz` | u32 | `PACE_TIMER`: 1..100000.  0 → `INVAL`; > 100000 → `OUT_OF_RANGE` |
| 8 | `watermark` | u16 | one of {0, 16, 32, 64, 128, 256, 512}, else `INVAL`.  0 = no events |
| 10 | `reserved` | u16 | must be 0, else `INVAL` |

**Reply (17 bytes):**

| Off | Field | Type | Meaning |
|-----|-------|------|---------|
| 0 | `tick_hz` | u32 | pace-timer tick: 1000000 when rate ≥ 16, otherwise 10000; 0 for non-timer triggers |
| 4 | `period_ticks` | u32 | `floor(tick_hz / sample_rate_hz)`.  **Realised rate = `tick_hz / period_ticks` exactly** (300 Hz → 3333 ticks = 300.03 Hz) |
| 8 | `full_scale` | u16 | `(1 << res_bits) − 1` captured at `BEGIN2` (4095/1023/255/63); oversampling does not change it |
| 10 | `vref_mv` | u16 | `adc_vref_mv` captured at `BEGIN2` |
| 12 | `flags` | u8 | bit0 `VREF_MEASURED` = a real `VREFINT` measurement in [1700, 1900] mV (not the 1800 fallback).  Bits 1..7 = 0 |
| 13 | `watermark` | u16 | the **granted** watermark = `ring_depth / 2`.  It may be **larger** than the watermark requested (see below); the host uses this value, never the one it asked for |
| 15 | `ring_depth` | u16 | the ring the firmware actually allocated, in samples: the smallest power of two ≥ `max(2 × watermark_req, ceil(realised_rate × 5 ms))`, capped at 1024 — the overrun budget (a request with `watermark = 0` asks for no events and keeps the 1024-deep ring) |

`BEGIN2` rules beyond the legacy `BEGIN` checks (vref dead → `IO`, slot in
use or shared converter → `INVAL`, converter claimed → `BUSY`, DMA or
calibration failure → `IO`):

* **Conversion-time check.**  `STATUS_OUT_OF_RANGE` if
  `ratio × (sample_cycles + 12.5) / 36 MHz ≥ period_ticks / tick_hz` (the
  `ADC_READ` residency model in `hal/gd32/gd32_common.h`; the maintainer
  chose that model over the 2.5-cycle figure in `protocol.h`, which the
  firmware repo corrects).  Unlike legacy `BEGIN`, which silently degrades,
  `BEGIN2` never reports a rate it cannot achieve.
* `bridge_core_clock_matches == false` → `STATUS_IO`.
* A late `VREF` re-measure pending → `STATUS_NOT_READY`; retry after ≥ 50 ms.
* **The reply, not the request, is authoritative for `watermark` and
  `ring_depth`.**  The ring is sized for at least 5 ms of samples at the
  realised rate, so a fast stream with a small requested watermark gets a
  larger one: 100 kHz with `watermark = 16` is granted `ring_depth` 512 and
  `watermark` 256.  The host must never assume granted == requested; its
  read schedule (below) and the overrun slack both derive from the echoed
  values.  `gd32g553_adc_stream2_read_interval_us()` computes the interval.
* A stream started with `BEGIN2` answers only `READ2` (legacy `0x34` →
  `INVAL`) and vice versa.  `0x35` `STREAM_END` ends either kind; `0x39`
  `CHAIN_BIND` and `0x3A` `SPECTRUM_READ` behave as in v0.14.

**`ADC_STREAM_READ2` (`0x3C`).**  Request (2 bytes): `stream_id:u8`,
`max_samples:u8`.  `max_samples` must be 1..`READ2_MAX(L) =
floor((mp − 9) / 2)` — 28 at `mp` = 65, 121 at `mp` = 252; 0 → `INVAL`,
above the ceiling → `OUT_OF_RANGE`.  Reply (**variable**, `9 + 2·got`
bytes):

| Off | Field | Type |
|-----|-------|------|
| 0 | `first_index` | u32: index of `codes[0]` in this stream's sample sequence since `BEGIN2`; wraps mod 2^32 |
| 4 | `dropped` | u32: samples discarded immediately before `codes[0]` since the previous `READ2` reply |
| 8 | `got` | u8: 0..`max_samples` |
| 9 | `codes[got]` | u16 LE each: right-aligned raw ADC codes, clamped to `full_scale` |

Framing: the CRC follows the last code directly (§4).  The host clocks
`13 + 2·max_samples` bytes, reads `got`, and ignores bytes after the CRC
(TX-underrun filler).  A non-OK `STATUS` means the 4-byte error envelope.
`got > max_samples` → the host returns `ALP_ERR_IO`.

**Accounting (normative).**

* Each stream keeps a delivered index `D` (u32), 0 at `BEGIN2`.  Backlog =
  (`total_written − total_read`) mod 2^32; ring index arithmetic uses the
  stream's own `ring_depth` (a power of two).
* **Overrun:** if backlog > `ring_depth − GUARD` (`GUARD` = 8), then
  `skip = backlog − (ring_depth − GUARD)`; the read cursor advances by
  `skip`, `dropped += skip`, and the reply is `STATUS_OK` with the freshest
  contiguous samples.  **`READ2` never answers `BUSY` for overrun.**
* `first_index = D_prev + dropped`, `got = min(max_samples, remaining
  backlog)`, `D = first_index + got`.
* Host invariant: `first_index(n) == first_index(n−1) + got(n−1) +
  dropped(n)` (mod 2^32), except when `dropped` is the sentinel.  The
  `gd32g553` driver counts violations in `ctx->stream2_gaps`.
* `dropped` saturates at `0xFFFFFFFE`.  `0xFFFFFFFF` is a **discontinuity
  of unknown length** with `got = 0` and `first_index = D_prev` (causes:
  ROVF recovery, a DSP pump `proc_gap`, backlog > 2^31).  The V2N ADC
  backend maps exactly this to `ALP_ERR_BUSY`.
* The reply capacity is checked **before** the ring is consumed:
  `9 + 2·max_samples > cap` → `NOMEM`, nothing consumed.
* FIR/IIR-bound stream: `READ2` returns processed-ring codes in the same
  code space.  FFT-bound: `NOSUPPORT`.  Sticky DSP faults keep their v0.14
  mapping (`dsp_cfg_bad` → `OUT_OF_RANGE`, `dsp_sat` → `IO`).
* Watermark events come from HTF (W samples) and FTF (2W) on a raw ring of
  length `2W` (W = the granted watermark), or from the base-level pump when
  the processed backlog reaches W.  Size the request with
  `W ≥ rate × host round-trip`; the slack before overrun is `W − GUARD`
  sample periods of the **granted** W.  Without `ATTN` granted, schedule
  reads from a host timer at `granted_W / realised_rate` (not by
  busy-polling).
* Host conversion to mV: `(min(code, full_scale) × vref_mv) / full_scale`,
  integer truncation — bit-identical to the legacy `STREAM_READ` maths.

| | `BEGIN2` | `READ2` |
|---|---|---|
| `NOSUPPORT` | feature not granted; reserved trigger; stub HAL | feature not granted; FFT-bound; stub |
| `INVAL` | length; stream_id; slot in use / shared converter; trigger ≥ `0x06`; arg / reserved ≠ 0; rate 0; watermark | length; stream_id; stream inactive or started with legacy `BEGIN` |
| `OUT_OF_RANGE` | rate > 100000; unmapped channel; conversion time ≥ period | `max_samples` > ceiling; `dsp_cfg_bad` |
| `NOT_READY` | `VREF` re-measure pending | n/a |
| `BUSY` | converter claimed | n/a (dispatcher-level `BUSY` only) |
| `IO` | vref dead; DMA disable or calibration failure; clock mismatch | DMA ERRIF; ROVF recalibration failure; `dsp_sat` |
| `NOMEM` | n/a | reply exceeds capacity (nothing consumed) |

Host API: `gd32g553_adc_stream_begin2()` / `gd32g553_adc_stream_read2()`.

### 3.19 Persistent boot configuration (`v0.15+`)

`BOOT_CONFIG` (`0x42`) reads or stores a small persistent flag word in GD32
flash.  Request `op:u8 flags:u32` (little-endian, 5 bytes): `op` `0` = GET
(`flags` ignored), `1` = SET.  The reply is always the **stored** `flags:u32`
(on a SET, the value stored *before* the commit).  **Allowed on the I2C link
as well as SPI**, because provisioning runs from Linux.

| Flag (bit) | Name | Effect |
|------------|------|--------|
| 0 | `SDMUX_EN_HIGH` | From every GD32 reset the firmware drives `PD11` (E1M `IO29`, the EVK's `SDIO_MUX_EN`, active-low: low = microSD connected, high = disconnected) **high**, so a provisioning run keeps the SD out across cold power cycles. |

* **Opt-in.**  `IO29`'s meaning is carrier-specific, so the default
  (nothing stored) leaves the pad untouched; only a host SET changes that.
* **SET never moves a pad.**  It takes effect at the next GD32 reset, so
  setting it on a unit booted from the SD cannot pull its rootfs out.  A
  host that wants the SD out now also writes `IO29` high with `GPIO_WRITE`.
  Clearing the flag does not release a pad that is already driven (that
  needs a GD32 reset).
* **Asynchronous SET.**  The firmware never touches flash inside the
  transport interrupt.  A SET is queued and answered at once; the main loop
  then commits it (on dual-bank parts two 1 KB page erases, each up to 20 ms
  with interrupts masked: 2 x 20 ms of blackout plus main-loop latency).
  There is no fixed idle window; the host waits ~50 ms and then polls GET,
  tolerating transport errors, until the stored value equals the request; `gd32g553_boot_config_set()`
  does exactly that.  A SET equal to the stored value is a no-op (no queue,
  no erase).  A different SET while one is still queued, or while an OTA
  session is active, answers `STATUS_BUSY`.  If the commit fails the GET poll never matches; the host
  re-sends the SET.
* **Power-loss safe.**  The flag lives in two A/B record pages (counter +
  CRC, commit doubleword last) outside every image; a cut at any point of a
  SET leaves either the old or the new value, never a fault: the boot-time
  read checks the flash ECC flags and treats an uncorrectable doubleword as
  "absent".  With no valid record every flag is off.
* Returns `STATUS_INVAL` for an unknown `flags` bit, `op` > 1 or a wrong
  length; `STATUS_BUSY` as above; `STATUS_NOSUPPORT` on firmware that
  predates the opcode, a build without the flash HAL, a single-bank part
  (`OBCTL.DBS` = 0) or an image running from slot B.  The host helpers map
  it to `ALP_ERR_NOSUPPORT`.

Host API: `gd32g553_boot_config_get()` / `gd32g553_boot_config_set()` with
`GD32G553_BOOT_CONFIG_SDMUX_EN_HIGH`.

### 3.20 I2C3 master proxy: `I2CM_CONFIG` / `I2CM_XFER` / `I2CM_RESULT` (`v0.17+`)

The bridge is the bus master of E1M-X I2C3 (GD32 `PC8` = SCL, E1M-X pad A24;
`PC9` = SDA, pad A23).  The 2625-R2 SoM has no pull-ups on I2C3; the carrier or
module provides them.  The pads are hi-Z until the first `I2CM_CONFIG`.  The
opcodes are **I2C-link only** (Linux over `BRD_I2C`, address `0x70`, 65-byte
payload cap): on SPI they answer `STATUS_NOSUPPORT`, they are on the §5.3
allow-list, and they are **not** in the `BATCH` allow-list.  A transfer is
three frames (queue, then poll) because the bus runs at base level and never
blocks the request handler; there is no host helper in `chips/gd32g553` (no CM33
caller).

| Opcode | Request | Reply |
|--------|---------|-------|
| `0xA0` `I2CM_CONFIG` | `bus_khz:u16` | _empty_ |
| `0xA1` `I2CM_XFER`   | `tag:u8 addr7:u8 flags:u8 wlen:u8 rlen:u8 wdata[wlen]` | _empty_ |
| `0xA2` `I2CM_RESULT` | _empty_ | `tag:u8 result:u8 nread:u8 rdata[nread]` |

* **`I2CM_CONFIG`**: `bus_khz` is `100` or `400`; `0` releases PC8/PC9 to hi-Z.
  Any other value answers `STATUS_INVAL`.  It runs the 9-clock bus recovery
  first.  The stub HAL answers `STATUS_NOSUPPORT`.
* **`I2CM_XFER`**: `flags` must be `0` (else `STATUS_INVAL`), `wlen` <= 60,
  `rlen` <= 62.  `wlen > 0 && rlen > 0` is `S W.. Sr R.. P`; one of them nonzero
  is write-only or read-only; both zero is a quick-write address probe.  The
  reply is empty with `STATUS_OK` (queued), `STATUS_BUSY` (a job is running) or
  `STATUS_NOT_READY` (not configured).  A new `XFER` discards an uncollected
  result.
* **`I2CM_RESULT`**: `STATUS_BUSY` with an empty payload while the job runs;
  `STATUS_OK` with `tag result nread rdata[nread]` once it finished;
  `STATUS_NOT_READY` if no job has run since the last `CONFIG`.

`result` byte (the outer `STATUS` keeps its generic §6 meaning; the Linux errno
the proxy adapter returns is in parentheses):

| `result` | Name | Meaning |
|----------|------|---------|
| 0 | `OK` | transfer completed |
| 1 | `NACK_ADDR` (`-ENXIO`) | address not acknowledged |
| 2 | `NACK_DATA` (`-EIO`) | data byte not acknowledged |
| 3 | `ARB_LOST` (`-EAGAIN`) | arbitration lost |
| 4 | `BUS_ERROR` (`-EIO`) | misplaced START/STOP |
| 5 | `TIMEOUT` (`-ETIMEDOUT`) | SCL held low ~10 ms or the ~20 ms job deadline passed |
| 6 | `BUS_STUCK` (`-EBUSY`) | SDA still low after recovery |

Recovery drives PC8 as a GPIO open-drain for up to 9 SCL pulses at about
100 kHz, then a STOP, then returns the pad to its alternate function.
`POWER_MODE_SET` answers `STATUS_BUSY` while a job runs, and after a wake the
bridge marks I2C3 unconfigured (the host re-sends `CONFIG`; `NOT_READY` is the
cue).  The Linux flow is: `XFER`, wait for the estimated transfer time,
`RESULT`; on `BUSY` back off up to 30 ms; on `NOT_READY` re-`CONFIG` the cached
speed and retry once; a `tag` mismatch is `-EAGAIN`.  The default speed is
100 kHz.  Register values and AF numbers live in the firmware, not here.

## 4. SPI framing

Each command on SPI is a **request frame** sent by the host while CS
is asserted, followed by a **reply frame** that the GD32 firmware
clocks back out on the *next* CS transaction within
`GD32G553_BRIDGE_REPLY_TIMEOUT_MS` (default 10 ms).

```
                                 1 byte  1 byte    N bytes     2 bytes
                                 ┌──────┬───────┬───────────┬─────────┐
   REQ  (host  → GD32)           │ SOF  │  CMD  │  PAYLOAD  │   CRC   │
                                 └──────┴───────┴───────────┴─────────┘
                                 1 byte  1 byte    M bytes     2 bytes
                                 ┌──────┬───────┬───────────┬─────────┐
   REPLY (GD32 → host)           │ SOF  │ STATUS│  PAYLOAD  │   CRC   │
                                 └──────┴───────┴───────────┴─────────┘
```

| Field   | Width | Notes                                                                                          |
|---------|-------|------------------------------------------------------------------------------------------------|
| SOF     | 1     | `0xA5`. Anything else → host or firmware abandons the frame and resyncs on the next CS edge.   |
| CMD     | 1     | Opcode from §3.                                                                                |
| STATUS  | 1     | Reply only.  Bits `[3:0]` = status code (`0x0` = OK, §6).  Bits `[7:4]` = the v0.7 **sequence stamp** — zero until `LINK_FEATURES` negotiates `STATUS_SEQ` (§3.14), so the legacy wire is unchanged. |
| PAYLOAD | N / M | Length is **opcode-derived** — both ends know the byte count from the opcode + status pair.   |
| CRC     | 2     | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, xor-out 0x0000, **non-reflected**), LSB first |

The CRC is transmitted **LSB first** (low byte on the wire first, then the
high byte) — e.g. CRC-16/CCITT-FALSE over the PING request body `A5 00` is
`0xFF84`, which goes on the wire as `84 FF`. That is the same little-endian
order as every other multi-byte field in this protocol (§2): the OTA
`size`/`crc32`/`offset` fields and the GPIO masks are all packed low byte
first. The trap is the firmware repo's `protocol_vectors.txt`, which prints CRCs
MSB-first (`A500FF84`) as hex text; that is the generator's text convention,
not the wire order. A host that packs OTA fields big-endian gets
`STATUS_OUT_OF_RANGE` (`0x08`) back from `OTA_BEGIN`, because the byte-swapped
`size` exceeds the slot (#2307).

Length is **not** carried on the wire because a single opcode has a
fixed request-payload width and a status-code-determined reply-payload
width — keeping the envelope at 1+1+N+2 bytes minimises the
fast-path overhead and avoids ambiguity at the SPI boundary
(SPI doesn't have a natural "end-of-frame" token).

Variable-length replies (`ADC_READ`, …) carry the length as the
first byte of the payload (`samples` for `ADC_READ`) — same byte the
host already sent in the request, **echoed back** before the data.

**Self-delimiting replies (v0.15).**  Two replies are not padded to a
fixed width: `ADC_STREAM_READ2` (`9 + 2·got` payload bytes, §3.18) and
`BATCH` (`1 + Σ(2 + len_i)`, §3.16).  Their **CRC position depends on the
reply's own bytes** — in a `READ2` SPI frame it sits at offset `11 + 2·got`
(`SOF` @0, `STATUS` @1, `got` @10).  The host clocks the worst case
(`13 + 2·max_samples` bytes for `READ2`, `4 + worst-case payload` for
`BATCH`; never more than one 256-byte frame per CS window), checks `STATUS`
first, then derives the payload length from the reply, validates it
against the request (`got ≤ max_samples`; for `BATCH`
`executed ≤ count`, `len_i ≤ maxreply_i`, fixed-reply ops exactly their
length) and checks the CRC where that length puts it.  Bytes after the CRC
are TX-underrun filler and ignored.  A non-OK `STATUS` is always the
4-byte error envelope with the CRC at offset 2.  A reply whose length field
is not legal for the request is `ALP_ERR_IO`.  On I2C these opcodes do not
exist (§5.3), so I2C replies stay fixed-length.

### 4.1 SPI timing

SPI uses a **two-transaction** pattern:

1. **Request transaction.**  Host asserts CS, clocks the request
   envelope out, deasserts CS.  This is a single half-duplex write
   (no useful data on MISO).
2. **Inter-transaction gap.**  The GD32's CS-rising handler decodes
   the request, runs the handler, and stages the reply; until that
   completes, a reply read clocks idle bytes.  Hosts MUST handle
   this with a **reply re-read schedule**: on SOF/CRC mismatch,
   re-issue the reply transaction after a short backoff (the
   `gd32g553` host driver ladders 25 µs → 1.6 ms, bounding the
   total wait at ~3.2 ms).  Re-reading is always safe: a drain
   transaction (all-`0x00` capture) never re-dispatches, and the
   slave **rewinds and re-arms the staged reply on every drain**
   (firmware ≥ v0.2.1; silicon-validated 2026-06-04), so the
   schedule converges as soon as the handler finishes.  A fixed
   ≥ 100 µs pre-read wait also works but wastes latency on the
   fast (sub-10 µs) handlers.
3. **Reply transaction.**  Host asserts CS, clocks filler bytes out
   (`0x00`; an all-`0x00` capture is what the slave classifies as a
   reply-drain), reads the reply envelope on MISO, deasserts CS.

The pattern is **half-duplex** on each transaction (request: host
talks; reply: GD32 talks).  Single-CS full-duplex is not used
because the slave cannot pre-populate its TX FIFO before it has
decoded the opcode.  A future v2 protocol revision may add a
single-CS variant with fixed inter-phase padding bytes for
latency-critical use cases.

If the GD32 ISR has not finished staging the reply by the time the
host begins the reply transaction, the host reads idle bytes
(`0x00`) or a partially armed reply.  The CRC check then fails and
the driver re-reads per the schedule above; only after the schedule
is exhausted does it return `ALP_ERR_IO`.  Even then, callers can
retry the whole command safely because the request side has already
executed (commands are idempotent except for `GPIO_WRITE` /
`PWM_SET`, where the host knows the desired final state and writing
the same value twice is benign).

### 4.1.1 The residual stale-reply hazard and its v0.7 kill

The two-transaction pattern has one residual hazard (documented in
`transport_spi.c` since v0.2.1): if a REQUEST is ever lost whole —
every byte landing inside the slave's SPI-reset window while the
previous handler still runs — the re-armed PREVIOUS reply is
CRC-valid stale data, and for a **same-opcode re-read** it is
indistinguishable from a fresh success.  This is not theoretical: it
was fingerprinted on silicon 2026-06-06 as byte-exact value replays
on back-to-back identical `COUNTER_READ` frames, with a fail rate
that swung 0 % ↔ 100 % purely on host-side timing phase.

Protocol v0.7 kills it with the **`STATUS_SEQ` sequence stamp**
(negotiated via `LINK_FEATURES`, §3.14): the slave keeps a 4-bit
counter that advances once per freshly decoded request and stamps it
into bits `[7:4]` of every SPI reply STATUS byte.  The drain/rewind
re-serves of the *same* staged reply keep the *same* stamp — so a
host that reads a CRC-valid reply whose stamp has **not advanced**
past its previously accepted reply knows its request was never
decoded, and re-sends it (the `gd32g553` driver does this once
automatically, counting occurrences in `ctx->seq_stale_count`).
While the slave keeps stamping, a stale verdict means the request was
never decoded, so the re-send is safe even for non-idempotent opcodes.
That inference is void across a slave **reset** (OTA commit/rollback,
watchdog): the feature reverts to off and every reply is stamped 0.
The `gd32g553` driver treats a CRC-valid stamp of 0 after a non-zero
baseline as that signature, and never re-sends -- it drops its
sequencing state, re-negotiates `LINK_FEATURES`, and fails the call
with `ALP_ERR_IO` (the request may or may not have executed).  The
stamp wraps mod 16; replies re-served across the wrap remain
detectable because detection compares against the last accepted
stamp, and the one legitimate advance to 0 (from 0xF) is accepted.
I2C replies are **never** stamped (`STATUS_NO_PENDING` owns bit 7
on that transport, and the hazard is SPI-specific).

### 4.1.2 ATTN-timed reply reads (`v0.15+`)

When `ATTN` is granted and passed its self-test (§3.17) the host replaces
the 35 µs staging gap and the ladder's first rung with a wait on the
`ATTN` rising edge (interrupt + semaphore, never polling): the firmware
raises the line only after the TX DMA holds the complete reply, so the host
starts the reply transaction on the edge with no gap.  Provisional timing
(bench-verify with `timing_stats` plus a scope on `P71`/`P97`):

| Parameter | Guarantee |
|-----------|-----------|
| CS falling → `ATTN` low | ≤ 2 µs while the CS-EXTI vector is idle (prio 1, the highest configured) |
| `ATTN` rise | Only after the TX DMA is armed with the complete reply; the host may start the reply transaction on the edge |
| CS rising → `ATTN` high | `t_frame` (≤ ~30 µs on 256-byte frames) + `t_dispatch(op)`; `ADC_READ` is bounded by `ADC_READ_ISR_BUDGET_US` = 1000 |
| Low time around each fresh stage | ≥ 1 µs (deassert at handler entry to assert at exit) |
| Host `ATTN` timeout `T_ATTN` | `GD32G553_BRIDGE_REPLY_TIMEOUT_MS` (10 ms) |
| CS setup (assert → first SCK) | Unchanged; the slave needs about 3 µs |
| Reply-read CS rising → next request CS falling | ≥ 10 µs (a request falling edge that coalesces with the drain handler re-arms RX over bytes already captured) |

On `BIG_FRAME` links the CS-rising handler works on frames up to 256 bytes
(`BRIDGE_SPI_DMA_BUF_LEN` 72 → 260, `.bss` ≈ +1 KB, MSP stays 2 KB); the
table-driven CRC costs about 9 instructions per byte (≈ 11 µs per 256-byte
frame at 216 MHz, twice per round trip).  Legacy opcodes stay limited to
65 bytes of payload on every link: the firmware answers `STATUS_INVAL` to
`payload_len > 65` on any opcode but `BATCH` (and to a `BATCH` above
`max_payload`), which restores the v0.14 exposure to a CS-edge-coalesced
capture whose frame CRC is byte-palindromic.  The host never clocks more
than 256 bytes in one CS window (the RX DMA count is 260; beyond that the
4-frame RX FIFO overflows, `RXORERR` sets and the error seam replaces the
staged reply with `STATUS_IO`) and never sends a request over 65 bytes
before `BIG_FRAME` is granted.

### 4.2 CRC validation

* On bad CRC the receiver returns `ALP_ERR_IO` and *does not* execute
  the command body.
* On a bad SOF the receiver returns `ALP_ERR_IO` and waits for the
  next CS-low edge before sampling again.
* Reply CRCs are computed over `SOF | STATUS | PAYLOAD`.
* CRC-16/CCITT-FALSE is identical to the variant Zephyr's
  `crc16_itu_t(0xFFFF, …)` produces and to Python
  `crcmod.predefined.PredefinedCrc('xmodem-falseinit')` —
  reference vector: CRC over the ASCII string "123456789" =
  `0x29B1`.

## 5. I2C framing

I2C carries the **same opcode + payload + CRC envelope**, but the
SOF byte is replaced by the I2C `START + slave address` envelope,
and the **register-style addressing** familiar to bus-management
code is used so the bridge looks like a regular I2C slave to
discovery tools.

The GD32 firmware presents itself at a **single 7-bit slave
address** configured at compile time (default `0x70`; boards can
override).  Inside that slave address space the firmware exposes
**one virtual register** through which all commands flow:

```
   write transaction (host → GD32)
   ┌──────────┬──────┬──────┬──────────┬─────────┐
   │ S+ADDR+W │ 0x00 │  CMD │ PAYLOAD  │   CRC   │
   └──────────┴──────┴──────┴──────────┴─────────┘

   read transaction (host → GD32)
   ┌──────────┬──────┬──────────┬─────────┬─────────┐
   │ S+ADDR+R │STATUS│ PAYLOAD  │   CRC   │   P     │
   └──────────┴──────┴──────────┴─────────┴─────────┘
```

The leading `0x00` register-address byte is the bridge's **command
register**; it exists only so the I2C framing matches the dominant
"write reg-addr then data" idiom on BRD_I2C.  All write transactions
write to register `0x00`; the firmware does not expose any other
register.

After a `write`, the host issues a `repeated-start read` (or a
fresh `read`) to consume the reply.  The reply payload length is
the same opcode-derived value as on SPI — but for I2C, where the
slave **can hold the bus** by clock-stretching, the firmware
guarantees that the reply bytes are available before it releases
SCL.  Hosts that don't support clock-stretching can poll the bus
busy bit instead.

The firmware's I2C allow-list is a subset of the opcode set; `BOOT_CONFIG`
(`0x42`) is on it (§3.19), so the persistent boot flags can be set from
Linux without the SPI link.

If the host issues a `read` before any `write` since the last
START, the firmware replies with one byte `STATUS = 0x80` (no
pending command) and an empty payload + CRC.

### 5.1 CRC on I2C

The CRC of an I2C transaction covers `CMD | PAYLOAD` on write and
`STATUS | PAYLOAD` on read — the I2C address byte is NOT included,
matching the convention used by most smart-battery / SMBus PEC
protocols.  The polynomial and parameters are the same as SPI
(CRC-16/CCITT-FALSE), so both transports share one
verification routine in `gd32-bridge-firmware:src/protocol.c`.

### 5.2 Slave-address overlap

The default GD32 slave address `0x70` is **not** occupied by any
chip on the V2N BRD_I2C bus (verified against
`metadata/e1m_modules/E1M-V2N101.yaml`).  When a board
allocates BRD_I2C to a device that conflicts, the firmware is
rebuildable with a different `CONFIG_GD32G553_BRIDGE_I2C_ADDR` —
host code reads back the address from `GET_VERSION` reply payload
in future protocol revisions (TODO; today the host has to know the
configured address out-of-band).

### 5.3 I2C opcode policy (`v0.15+`, unconditional)

Firmware v0.15 enforces an **allow-list on the I2C link**, in
`protocol_dispatch_inner()` after the OTA trial gate (any CRC-valid frame
on either link still confirms a trial) and before the opcode switch:

* **Allowed on I2C:** `0x00` PING, `0x01` GET_VERSION, `0x02`
  GET_BUILD_ID, `0x03` RESET_REASON, `0x10` GPIO_READ, `0x11` GPIO_WRITE,
  `0x41` SE_RESET, `0xA0..0xA2` I2CM_* (v0.17, §3.20; I2C-only), `0x81` LINK_FEATURES (I2C grants only `STATUS_SEQ`, with
  `mp` = 65, so the extended form is accepted but changes nothing) and
  `0xF0..0xFF` OTA.
* **Any other opcode** answers `STATUS_NOSUPPORT` (`0x06`) with an empty
  payload; the handler never runs.  Host code treats it like any other
  `NOSUPPORT`.
* The firmware keeps SWD-readable diagnostics `bridge_i2c_denied_count:u32`
  and `bridge_i2c_denied_last_cmd:u8` (same style as `bridge_i2c_rx_diag`).
* SPI is unrestricted; no per-pad GPIO ownership between Linux (I2C) and the
  CM33 (SPI) is enforced.

The in-tree I2C callers were audited against the list and use only allowed
opcodes: the kernel `gpio-gd32-bridge` driver (`0x00/0x01/0x10/0x11/0x41`),
`tools/gd32-ota-host` (`gd32g553_init` + OTA `0xF0..0xF6`) and
`examples/v2n/v2n-brd-i2c-bringup` (`PING`/`GET_VERSION`).  OTA stays
reachable on I2C; SWD is for factory flashing and recovery.

## 6. Status codes (firmware → host)

The status byte returned in every reply maps 1:1 onto a subset of
the host-side `alp_status_t` enum.  The wire encoding is the
**absolute value** of the negative-numbered host enum so the status
byte is naturally unsigned and human-readable on a logic analyser:

| Wire `STATUS` | Host `alp_status_t`     | Meaning                                                |
|---------------|-------------------------|--------------------------------------------------------|
| `0x00`        | `ALP_OK`                | Command executed successfully.                         |
| `0x01`        | `ALP_ERR_INVAL`         | Bad arguments (e.g. channel out of range).             |
| `0x02`        | `ALP_ERR_NOT_READY`     | Sub-resource (PWM, ADC peripheral) not initialised.    |
| `0x03`        | `ALP_ERR_BUSY`          | Firmware busy servicing a long-running operation.      |
| `0x04`        | `ALP_ERR_TIMEOUT`       | Sub-bus operation timed out (e.g. ADC/timer peripheral).|
| `0x05`        | `ALP_ERR_IO`            | CRC failure or transport-layer error.                  |
| `0x06`        | `ALP_ERR_NOSUPPORT`     | Opcode unknown to this firmware build, or a request the firmware understands but cannot service (e.g. DSP chain-pool exhaustion — see §3.x). |
| `0x07`        | `ALP_ERR_NOMEM`         | v0.15: a `READ2` reply that would not fit the firmware's buffer (nothing consumed, §3.18).  Otherwise reserved. |
| `0x08`        | `ALP_ERR_OUT_OF_RANGE`  | Parameter beyond hardware capability (e.g. PWM freq).  |
| `0x80`        | _(I2C: no pending cmd)_ | I2C read before any write on this START — see §5.      |
| Other         | `ALP_ERR_IO` (mapped)   | Unknown wire status → host returns `ALP_ERR_IO`.       |

Hosts MUST translate the wire byte back to a negative `alp_status_t`
via the table above before returning it from a public API call.

**v0.15 I2C note:** on the I2C link an opcode outside the §5.3 allow-list
answers `STATUS_NOSUPPORT` (`0x06`) with an empty payload — for example
`ADC_READ` or `PWM_SET` over I2C against a v0.15 bridge.  Pre-v0.15
firmware serviced those opcodes on I2C; hosts that need them use SPI.

**v0.17 I2CM note:** `I2CM_*` replies keep the generic meanings above
(`STATUS_BUSY` = a job is running, `STATUS_NOT_READY` = unconfigured or no job
yet); the outcome of the I2C3 transfer itself is the `result` byte in the
`I2CM_RESULT` payload (§3.20), not the outer status.

**v0.7 SPI note:** once `STATUS_SEQ` is negotiated (§3.14), the SPI
status byte carries the sequence stamp in bits `[7:4]` — hosts mask
with `0x0F` before the table lookup (every SPI-visible code fits the
low nibble; `0x80 NO_PENDING` is I2C-only, where stamping never
applies).  Masking is safe unconditionally on SPI: legacy firmware
never sets the high nibble there.

**OTA post-COMMIT/ROLLBACK trial window (v0.12, firmware >= 0.2.14):**
after `OTA_COMMIT` or `OTA_ROLLBACK` commits a trial-capable image the
bridge reboots into an unconfirmed TRIAL image and answers
`STATUS_BUSY` to **every** opcode — including `PING` and
`GET_VERSION` — until it decodes its first CRC-valid frame of any
kind, which itself confirms the trial (see §10).  `gd32g553_init()`
rides this out with a bounded (~2 s) retry ladder gated on the
following rule: `ALP_ERR_BUSY` is always retried; `ALP_ERR_IO` /
`ALP_ERR_TIMEOUT` are retried **only** once this same `init()` call
has already seen at least one `ALP_ERR_BUSY` (proof a trial is
actually in progress and a confirm-triggered second reset is the
explanation) — an absent or unflashed bridge never answers `BUSY` at
all, so it still fails fast on the very first `IO`/`TIMEOUT`, exactly
as before this retry existed.  Other callers hitting `STATUS_BUSY` in
this window should retry after a re-init rather than treat it as a
hard failure, using the same BUSY-gates-IO/TIMEOUT rule if they build
their own retry.

## 7. Liveness handshake

`gd32g553_init()` issues:

1. `PING` with a 100 ms timeout — confirms the link physically
   answers and the firmware is not wedged.
2. `GET_VERSION` — caches the major.minor.patch into the driver
   context.  If `major` mismatches `GD32G553_HOST_PROTOCOL_MAJOR`,
   the init returns `ALP_ERR_NOSUPPORT` and refuses to operate on
   incompatible firmware (avoids a host that speaks newer commands
   talking past an older firmware build).
3. **Link-feature negotiation (SPI only; §8 has the full order).**  For a
   peer reporting minor ≥ 15 the 6-byte `LINK_FEATURES` form
   (`STATUS_SEQ | BIG_FRAME | ADC_STREAM2 | BATCH`, plus `ATTN` when the
   backend registered an `ATTN` hook, `max_payload_req` 252), then the
   `ATTN` self-test; a peer below 15, or one answering `INVAL` /
   `NOSUPPORT`, gets the legacy 1-byte `STATUS_SEQ` form.  Best-effort: a
   failure leaves the link on the legacy framing.
4. `GET_BUILD_ID` (optional, only if the host logs it) — useful in
   production-test logs to confirm the GD32 has the firmware build
   that QC signed off on.

There is **no periodic keep-alive** between host and bridge.  The
host issues commands on demand; the bridge always replies.  The
host can detect a wedged bridge by issuing `PING` whenever the
caller wants liveness confirmation.

## 8. Backward compatibility policy

* The protocol carries a `major.minor.patch` version returned by
  `GET_VERSION`.
* `major` is bumped on **wire-breaking** changes (frame layout,
  CRC algorithm, command renumbering).
* `minor` is bumped on any **additive, backward-compatible** change
  older hosts don't have to know about -- not only a new opcode: v0.10
  (chain-bind refusal) and v0.11 (the REG_ON pad-map growth, §3.1) both
  bumped `minor` with no new opcode at all.
* `patch` is bumped on documentation or non-observable firmware
  changes.

Until the bridge fleet ships in production, `major` stays at `0`
and the host driver insists on **exact** major-number match.

**Negotiated compatibility (v0.15).**  A different `minor` is no longer a
lock-step violation: mixing a v0.14 host or firmware with a v0.15 one is
supported in all four directions, because every v0.15 feature is gated on
**negotiation**, not on the minor alone.  Everything new except the I2C
opcode policy (§5.3) stays off until the host enables it on that link with
the 6-byte `LINK_FEATURES` form (§3.14); an SPI link that has not
negotiated is byte-identical to v0.14.  Features that change an existing
opcode's payload without a feature bit remain minor-gated, as v0.6 did to
`OTA_WRITE_CHUNK`, and the major-only handshake cannot see that.

**Host discovery order** (`gd32g553_init()`):

1. `PING`, using the BUSY-gated retry ladder (§6).
2. `GET_VERSION`: `major` must equal `GD32G553_HOST_PROTOCOL_MAJOR`; record
   `m` = minor.
3. If SPI is open: (a) if `m ≥ 15`, send the 6-byte `LINK_FEATURES` with
   `want` = `STATUS_SEQ | BIG_FRAME | ADC_STREAM2 | BATCH`, plus `ATTN` only
   if the backend registered an `ATTN` hook with `P71` as input + IRQ, and
   `max_payload_req` = 252; store `granted` / `mp`, take the stamp baseline
   from the reply and run the `ATTN` self-test (§3.17); if the reply is
   `INVAL` or `NOSUPPORT`, go to (b).  (b) if `m < 15`, send the 1-byte
   `STATUS_SEQ` form, as in v0.7..v0.14.
4. If I2C is open nothing is required (the I2C link grants only
   `STATUS_SEQ`; a v0.15 host may send the 6-byte form there to read
   `supported`, expecting `0x00000001` and `mp` 65).
5. On the RESET signature (§4.1.1) or any reply showing an OTA
   `COMMIT`/`ROLLBACK` reset: drop every negotiated item (sequence,
   `BIG_FRAME`, `ATTN`, `STREAM2`, `BATCH`, stream handles) and repeat from
   step 1.

| Host | Firmware | Result |
|------|----------|--------|
| 0.14 | 0.14 | Unchanged. |
| 0.14 | 0.15 | 1-byte negotiation, so the wire is byte-identical to v0.14 on SPI.  On I2C, opcodes outside §5.3 now answer `NOSUPPORT`; no in-tree caller is affected. |
| 0.15 | 0.14 (or older) | Legacy 1-byte `STATUS_SEQ`; legacy `0x33`/`0x34` (mV, `BUSY` on overrun, `dropped` unknown); no `BATCH`; 65-byte frames; `P71` may stay input or SWD; `ATTN` never driven. |
| 0.15 | 0.15, `ATTN` not granted (not requested, debugger attached, stub, self-test failed) | `BIG_FRAME` + `STREAM2` + `BATCH`.  Reply timing uses the v0.14 staging gap + ladder + `STATUS_SEQ`.  Reads are scheduled by a host timer at `W / realised_rate`, not by busy-polling. |
| 0.15 | 0.15, `ATTN` granted | Everything.  Per-command fallback to the drain rule on timeout; disable after 3 consecutive faults (§3.17). |

A v0.5 host driving v0.6 firmware (or the reverse) is undetectable by
`major` alone yet corrupts a flashed image (v0.5 firmware reads the v0.6 chunk **length byte** as
image data), the host driver **gates the OTA session on `minor`**: an
OTA cannot start against a peer below `GD32G553_OTA_MIN_PROTOCOL_MINOR`
(6). `gd32g553_ota_begin` / `gd32g553_ota_write_chunk` return
`ALP_ERR_NOSUPPORT` **before any erase or program**, and
`gd32g553_ota_supported()` lets a host check up front (#751). The
REG_ON lines 18/19 of `GPIO_WRITE` are the second minor-gated surface
(`GD32G553_REG_ON_MIN_PROTOCOL_MINOR`, §3.1). Every v0.15 surface is
gated by negotiation (§3.14) instead; an older host simply never sends it.

Version history (pre-1.0): **v0.7** adds `LINK_FEATURES` (0x81) +
the negotiated `STATUS_SEQ` reply stamp (§3.14, §4.1.1) and the
additive `OTA_BEGIN` version triple (§10) — both backward-compatible
by construction (un-negotiated framing and the 8-byte BEGIN are
byte-identical to v0.6).  **v0.8** adds `SE_RESET` (0x41, §3.15) — a
new opcode; older hosts simply never send it.  **v0.9** (#496) lands
the runtime FAC/FFT dispatch inside `bridge_hw_adc_stream_read()` for
the ADC-stream DSP pipeline's already-existing `chain_open` /
`stage_push` / `chain_bind` opcodes (§3.x) — a bound chain now
actually filters or spectralizes the stream instead of the chain
sitting unbound — and adds the new opcode `CMD_ADC_SPECTRUM_READ`
(`0x3A`, §3.x) to pull the FFT terminal's spectrum; a v0.8 host that
never binds a chain sees no behaviour change.  **v0.10**
(`gd32-bridge-firmware` PR #121) makes the ADC-stream DSP chain bind
refuse, up front, a chain the FAC/FFT runtime cannot realise and a
second FFT bind against the single FFT block; before it, both were
accepted and failed only at stream time (`STATUS_OK` with zero
samples forever, or `STATUS_BUSY` forever on both streams).
**v0.11** (firmware `0.2.12`, `gd32-bridge-firmware` PR #244) grows the GPIO
expander pad map from 18 to 20 lines, adding `bt-reg-on` (bit 18,
`PE14`) and `wl-reg-on` (bit 19, `PE15`) for the on-module Murata
LBEE5HY2FY-922 (Infineon CYW55513) Wi-Fi+BT module's REG_ON enables —
both boot OUTPUT LOW; the host drives REG_ON low for >= 10 ms then
high (§3.1). No opcode changed shape; a host below
`GD32G553_REG_ON_MIN_PROTOCOL_MINOR` (11) simply never learns bits
18/19 exist. `GET_VERSION`'s SPI reply for `0.11.0` is `A5 00 00 0B 00 C5 C4`
(`SOF STATUS major minor patch CRClo CRChi`, CRC-16/CCITT-FALSE over
`SOF..PAYLOAD` per §4.2) -- recompute from the algorithm rather than
hand-copying, and cross-check any other hand-copied `GET_VERSION`
vector before relying on it (see `extending-the-gd32-bridge-protocol`'s
note on inlined wire hex going stale across a `PROTOCOL_VERSION`
bump).  **v0.12** adds the
post-COMMIT/ROLLBACK TRIAL-then-confirm boot sequence (§6, §10) — no
new opcode and no framing change, so it is observable only as a
bounded run of `STATUS_BUSY` right after an OTA reset committing a
trial-capable image (firmware release >= 0.2.14, a separate axis from
this wire-protocol version — see §10); a host that already treats
`STATUS_BUSY` as retryable (as `gd32g553_init()` now does) sees no
behaviour change beyond that widened retry window.  **v0.14**
(gh#101) widens `OTA_GET_STATE`'s reply 5 -> 6 bytes, appending the
`err` cause byte documented above (§10) -- additive per the
opcode-derived-length rule, so a host below
`GD32G553_OTA_ERR_MIN_PROTOCOL_MINOR` (14) keeps working unchanged and
simply never learns the 6th byte exists. `GET_VERSION`'s SPI reply
for `0.14.0` is `A5 00 00 0E 00 30 3B` (same field layout as v0.11's
vector above; recompute from the algorithm rather than hand-copying).
**v0.15** is a *negotiated* minor: it adds the 6-byte `LINK_FEATURES`
form and the `BIG_FRAME` / `ATTN` / `ADC_STREAM2` / `BATCH` bits (§3.14),
`BATCH` (`0x04`, §3.16), the `ATTN` data-ready line on `PA14`/`P71`
(§3.17), `ADC_STREAM_BEGIN2` / `ADC_STREAM_READ2` (`0x3B`/`0x3C`, §3.18),
self-delimiting reply framing (§4) and the unconditional I2C opcode
allow-list (§5.3).  Nothing but the I2C policy is observable until a host
negotiates it.  `GET_VERSION`'s SPI reply for `0.15.0` is
`A5 00 00 0F 00 01 08` (CRC from `tests/gen_protocol_vectors.py` in the
firmware repo, wire order low byte first).
The same minor also grows the GPIO mask from 21 to 23 bits -- bit 21 = E1M IO15
(GD32 `PB4`), bit 22 = E1M IO26 (GD32 `PC2`), §3.1 -- and a host below
`GD32G553_IO15_IO26_MIN_PROTOCOL_MINOR` (15) never learns the two bits exist.
**v0.17** adds the I2C3 master proxy opcodes `I2CM_CONFIG` / `I2CM_XFER` /
`I2CM_RESULT` (`0xA0..0xA2`, §3.20; I2C link only, outside `BATCH`) and the four
`CAM_EN_LDO0..3` GPIO bits 23..26 (§3.1).  A host below
`GD32G553_I2CM_MIN_PROTOCOL_MINOR` (17) never sends them; the Linux adapter
refuses transfers with `-EOPNOTSUPP` against an older bridge.

## 9. Reference vectors

The canonical sanity-check for the CRC implementation is the
universally-cited CRC-16/CCITT-FALSE result over the ASCII string
"123456789":

* `crc16_ccitt_false("123456789") == 0x29B1`

The per-opcode wire vectors (SPI `PING` round-trip, I2C `PING`
round-trip, `GET_VERSION` reply for the firmware's declared
version) are generated at firmware build time and stored in
`gd32-bridge-firmware:tests/protocol_vectors.txt` by
`gd32-bridge-firmware:tests/gen_protocol_vectors.py`; the hex is never
hand-computed.  `tests/zephyr/chips/src/test_gd32_bridge.c` covers argument
checks and the `gd32g553_init()` BUSY/IO retry ladder against an `i2c-emul`
fake bridge.  For v0.15, `tests/unit/gd32_protocol_015` drives the real host
driver against a byte-level SPI model of a v0.14 and a v0.15 bridge and pins
the following vectors (bytes before the CRC; CRC-16/CCITT-FALSE appended
low byte first):

| Name | Bytes before CRC |
|------|------------------|
| `spi_get_version_reply_v0_15_0` | `A5 00 00 0F 00` |
| `spi_link_features_ext_request_all` | `A5 81 1F000000 FC00` |
| `spi_link_features_ext_reply_all_seq1` | `A5 10 1F000000 1F000000 FC00` |
| `spi_link_features_ext_reply_stub_seq1` | `A5 10 13000000 13000000 FC00` |
| `spi_reply_inval` (also the v0.14 answer to the 6-byte form) | `A5 01` |
| `i2c_link_features_ext_write` | `00 81 1F000000 FC00` (CRC over `81..`) |
| `i2c_link_features_ext_read` | `00 01000000 01000000 4100` |
| `i2c_adc_read_ch0_4_write_denied` | `00 30 00 04`, read `06` |
| `spi_adc_stream_begin2_s0_ch0_1khz_w256_request` | `A5 3B 00 00 00 00 E8030000 0001 0000` |
| `spi_adc_stream_begin2_reply_1khz_w256` | `A5 00 40420F00 E8030000 FF0F 0807 01 0001 0002` |
| `spi_adc_stream_read2_s0_max121_request` | `A5 3C 00 79` |
| `spi_adc_stream_read2_reply_got3` | `A5 00 00010000 00000000 03 0008 0108 FF0F` |
| `spi_adc_stream_read2_reply_empty` | `A5 00 03010000 00000000 00` |
| `spi_adc_stream_read2_reply_overrun_dropped32` | `A5 00 23010000 20000000 02 0008 0108` |
| `spi_adc_stream_read2_reply_discontinuity` | `A5 00 25010000 FFFFFFFF 00` |
| `spi_batch_request_gpiow_pwmget_read2` | `A5 04 03 11 08 01000000 01000000 21 01 00 3C 02 00 10` |
| `spi_batch_reply_stop_at_first_error` | `A5 00 02 00 00 01 00` |
| `spi_batch_request_nested_rejected` | `A5 04 01 04 00`, reply = `spi_reply_inval` |
| `spi_batch_request_trailing_byte_rejected` | `A5 04 01 00 00 00`, reply = `spi_reply_inval` |
| `spi_ota_write_chunk_over_65_rejected_on_big_link` | `A5 F1` + 66-byte payload, reply = `spi_reply_inval` |

The I2C and rejected-request rows are firmware-side vectors; the host test
covers the rows that exercise the host's own framing and parsing.

## 10. Field upgrades of the bridge firmware

> **Status: Path A implemented (gated, HIL-pending); Path B scaffolded.**

Two upgrade paths.  Per the V2N hardware decision (2026-05-12),
the board routes `GD32_SWDIO` + `GD32_SWCLK` + `GD32_NRST` from
the Renesas host to the GD32; the BOOT0-strap / factory-ISP path
was dropped after the GD32G553 boot ROM was confirmed
USART-only (User Manual Rev1.2 §1.4).

**Path A — Application bootloader over the bridge (preferred
normal upgrade path).**

* A 32 KB bootloader lives at the base of GD32 flash, never
  overwritten by a field upgrade, followed by the A/B metadata pair
  (`0x08008000`) and two 216 KB **slots**: slot A at `0x0800A000` in
  flash bank 0 and slot B at `0x08040000`, the start of bank 1, so an
  upgrade erases only the bank the running image is not executing from.  The active slot
  runs at boot while the inactive slot receives the upgrade;
  roll-back is a metadata flip + reset.  Destructive flashing is
  armed only in `-DBRIDGE_OTA_PARTITIONED` firmware builds — the
  default build answers `STATUS_NOSUPPORT` to the whole range.
* Uses the same SPI / I2C transport as the rest of the protocol --
  no extra wiring beyond what is already in place.

Wire contract (host mirrors in `<alp/chips/gd32g553.h>`,
firmware in `gd32-bridge-firmware:src/ota.c`):

| Op | Name | Request payload | Reply payload |
|------|------|----------------|---------------|
| `0xF0` | `OTA_BEGIN` | `size:u32 expected_crc32:u32` `[fw_major:u8 fw_minor:u8 fw_patch:u8]` (v0.7 additive: the incoming image's version, recorded into the A/B metadata `fw_version[slot]` at COMMIT; legacy 8-byte form ⇒ 0 = unknown, and pre-v0.7 firmware ignores the trailing triple) | `chunk_max:u16 target_slot:u8` |
| `0xF1` | `OTA_WRITE_CHUNK` | `offset:u32 len:u8 data[len]` | `received_bytes:u32` (high-water) |
| `0xF2` | `OTA_VERIFY` | _empty_ | `computed_crc32:u32 verified:u8` |
| `0xF3` | `OTA_COMMIT` | _empty_ | _empty_ (resets on success) |
| `0xF4` | `OTA_ROLLBACK` | _empty_ | _empty_ (resets on success) |
| `0xF5` | `OTA_GET_STATE` | _empty_ | `state:u8 active:u8 pending:u8 boot_count:u16` `[err:u8]` (v0.14 additive: the cause of the most recent `state = ERROR`, gh#101; a peer below protocol minor 14 never appends this byte -- see below) |
| `0xF6` | `OTA_ABORT` | _empty_ | _empty_ |

Value encodings: `state` = 0 IDLE / 1 READY / 2 BUSY / 3 VERIFIED /
4 ERROR; slot bytes = 0 A / 1 B / `0xFF` none-pending.  `WRITE_CHUNK`
offsets must land on 8-byte (FMC doubleword) boundaries -- firmware
advertises `chunk_max` = 56 (the largest multiple of 8 that fits the
envelope), and a misaligned offset answers `STATUS_INVAL` (0x01) with
the session left READY, so the host can resend at a valid offset; the image
CRC-32 is IEEE 802.3 reflected (zlib-compatible) -- host code computes
the `expected_crc32` BEGIN wants (and cross-checks VERIFY's
`computed_crc32`) with `gd32g553_ota_image_crc32()`
(`<alp/chips/gd32g553.h>`), which is hardware-accelerated on a build
that instantiates the Alif Ensemble E8 CRC engine and otherwise falls
back to portable software producing the same value.  `WRITE_CHUNK` and
`VERIFY` without a BEGIN-opened session answer `STATUS_NOT_READY`
(0x02), as does `COMMIT` before a successful `VERIFY`.

`WRITE_CHUNK`'s explicit `len` byte (v0.6) is load-bearing, not
redundant: the slave can capture a request merged with the following
transaction's zero filler (its FMC program window swallows CS edges),
and for a frame whose CRC happens to be byte-palindromic the
zero-extended span still passes the span CRC (CRC-CCITT-FALSE
self-consumption: message + own CRC + zeros hashes to `0x0000` —
silicon-caught 2026-06-04 at the first such chunk of a real image).
A `len`-vs-span mismatch answers `STATUS_INVAL` without disturbing
the session.  Re-sent chunks below the high-water mark are
deduplicated against flash (identical → OK without re-programming;
different → `STATUS_IO`): the ECC flash cannot program a doubleword
twice even with identical data.

Because BEGIN's slot erase, VERIFY's full-image CRC, and
COMMIT/ROLLBACK's reset run inside the request transaction, their
reply transaction can miss — hosts treat an I/O error there as
"issued" and confirm via `OTA_GET_STATE` (or by re-initialising
against the rebooted bridge after COMMIT/ROLLBACK).

**`OTA_GET_STATE`'s `err` byte (v0.14, gh#101).** Before this bump,
every OTA failure collapsed into `state = ERROR` with nothing else on
the wire, so a failed session was unattributable for the host and
indistinguishable on the bench. `err` names the cause (`0` when
`state != ERROR`, or when it is but nothing yet recorded a specific
cause):

| `err` | Name | Meaning |
|-------|------|---------|
| `0x00` | `NONE` | No error recorded. |
| `0x01` | `SESSION_RANGE` | `BEGIN`/`VERIFY`/`COMMIT` image size out of range. |
| `0x02` | `ERASE_FAILED` | Background page erase failed. |
| `0x03` | `CHUNK_RANGE` | Chunk offset/length rejected. |
| `0x04` | `PROGRAM_FAILED` | Flash program failed (PGERR/PGSERR). |
| `0x05` | `VERIFY_CRC` | `VERIFY`'s CRC comparison failed. |
| `0x06` | `COMMIT_FAILED` | `COMMIT`: bootability check or metadata commit failed. |
| `0x07` | `ERASE_TARGET` | Erase target would intersect the running slot. |
| `0x08` | `NOT_TRIAL_CAPABLE` | `COMMIT` refused: candidate image has no valid trial marker. |
| `0x09` | `META_DEMOTE_FAILED` | `BEGIN`: metadata commit demoting the stale target slot failed. |
| `0x0A` | `BELOW_FLOOR` | `COMMIT`/`ROLLBACK` refused: the image's version is below the anti-rollback floor. Answers `STATUS_INVAL`; the active slot is untouched. |

The host mirror is `gd32g553_ota_err_t`
(`<alp/chips/gd32g553.h>`); `gd32g553_ota_get_state()` reads this byte
only against a peer advertising protocol minor >=
`GD32G553_OTA_ERR_MIN_PROTOCOL_MINOR` (14) — an older bridge never
appends it, and reading past what it sent would desync the reply CRC
over a byte that was never on the wire. Against an older peer, the
host-side `err` field always reads `NONE` regardless of the real
cause, same as before this bump. `OTA_ABORT` clears the recorded
cause back to `NONE`.

**TRIAL boot, confirm, and watchdog revert (v0.12, firmware >= 0.2.14).**
`COMMIT` and `ROLLBACK` don't hand control straight to a trusted
image: when the committed image is itself trial-capable (firmware
release >= 0.2.14 -- the release that introduced this whole mechanism;
see §8's version history), the bridge reboots into the newly-active
slot in an unconfirmed **TRIAL** state, and until it decodes its first
CRC-valid frame — of ANY opcode, not just an OTA one — it answers
`STATUS_BUSY` (§6) to everything.  That first valid frame both
confirms the trial AND triggers a **second** reset, this one booting
the now-confirmed image; the link drops again for a few milliseconds
around that second reset before settling into normal operation.  If no
valid frame lands before the bootloader's watchdog (FWDGT) window
expires — nominal ~32.8 s — the bootloader reverts to the
previously-active slot instead, exactly as if the new image had hung.

An `OTA_BEGIN` sent in the legacy 8-byte form (no `fw_version` triple,
§3's table above) is treated as an unknown incoming version, which is
itself sufficient reason for the bridge to boot the result into TRIAL
— this is deliberate: a caller that hasn't wired up version tracking
still gets the safety net.  **This is only the right call for an image
that is itself >= 0.2.14** — an OLDER image committed via the legacy
form (or via an explicit version that happens to be unknown) still
boots into TRIAL, has no confirm logic of its own to run, and is
reverted by the watchdog ~33 s later exactly as if it had hung.  To
install an older image that must actually survive, pass its real,
known version to `OTA_BEGIN` instead: an explicitly known version
**below 0.2.14** is a deliberate **downgrade guard** — the bridge
commits or rolls back to it WITHOUT the TRIAL dance at all (no
`STATUS_BUSY` window, no second reset), since an image that predates
the confirm protocol could never satisfy it.

**Bootloader/app version coupling.** The watchdog-revert protection is
the BOOTLOADER's doing, not the app's. An old bootloader paired with a
new, trial-capable app still runs the TRIAL/`STATUS_BUSY`/confirm
dance (the app side alone drives that), but an old bootloader has no
watchdog-revert logic to fall back on, so a hung new app is never
reverted. Bootloader and app must be built and shipped together for
the watchdog-revert protection to actually apply.

Host contract: send a frame within the watchdog window after COMMIT
or ROLLBACK (any opcode works — a `PING`/`GET_VERSION` re-init is
enough), and treat `STATUS_BUSY` seen right after COMMIT/ROLLBACK as
retryable, not a failure.  `gd32g553_init()`
(`chips/gd32g553/gd32g553.c`) already does this with a bounded (~2 s)
retry ladder gated on: `ALP_ERR_BUSY` is always retried;
`ALP_ERR_IO`/`ALP_ERR_TIMEOUT` are retried only once THIS SAME
`init()` call has already seen at least one `ALP_ERR_BUSY` — so a
plain re-init call is the whole contract for most callers, and an
absent/unflashed bridge (which never answers BUSY at all) still fails
fast rather than paying the full retry budget on every acquire.  A
re-init whose very first frame happens to land inside the
COMMIT/ROLLBACK reset itself (before any BUSY was ever observed) still
fails fast with `IO`/`TIMEOUT` — the caller's own outer retry (already
required for any transient init failure) picks it up on the next
attempt.  `gd32g553_ota_get_state()` does NOT retry on its own — call
it after a successful re-init, not in the trial window itself.

**Bench status:** both the app-side confirm path (BUSY → first valid
frame → second reset → normal operation) AND the bootloader's
watchdog-driven revert path are silicon-validated on E1M-V2M103,
2026-09-26: a deliberately hung image (HXTAL misconfigured) committed
via the legacy `OTA_BEGIN` form was reverted to the previous slot
after ~33 s, `RESET_REASON` read back `WDT`, and the bridge's own
health check cleared the reverted slot's `slot_valid` flag.

**Path B — Host-driven SWD bit-bang (universal recovery).**

* Used when Path A is unreachable (corrupt application bootloader,
  factory first-flash, dev-board bring-up).
* The Renesas host implements a software SWD controller, drives
  `GD32_SWDIO` + `GD32_SWCLK` as GPIOs, optionally asserts
  `GD32_NRST` to halt the GD32 cleanly, then issues SWD packets
  to reflash the entire chip.
* Works **regardless of GD32 firmware state** -- SWD is a hardware
  debug bus and doesn't depend on the boot ROM or any firmware
  layer being intact.
* Strictly more capable than the GD32's factory-ISP route, which
  would have required wiring a USART pair as well as BOOT0 +
  NRST.  Routing SWD is the cleaner design.
* Open-source reference implementations of bit-bang SWD: PyOCD,
  OpenOCD, J-Link OB.  ~500-1000 LOC for the protocol layer +
  GPIO HAL.
* Security note: any V2N firmware with access to the SWD GPIOs
  can reflash the GD32 unconstrained.  Same threat surface as
  Path A's OTA opcodes.

Until either path ships, **GD32 field upgrades go through an
external SWD probe** attached to the V2N module's programming
header.

## 11. Out-of-scope

* OTA of the **bridge firmware** beyond what §10 describes -- the
  device-side OTA contract for the Renesas-side firmware (the
  bigger one Mender drives) lives in
  [`docs/ota.md`](ota.md) +
  [`docs/ota-device-contract.md`](ota-device-contract.md); Hakan
  owns the server side.
* DA9292 fault pins / PMIC alarms — on the current SoM revision the
  `DA9292_INT`/`DA9292_TW` nets reach only the Renesas (P37/P36), so
  the host reads the pin state directly (`da9292_get_fault_pins()`)
  and full PMIC register status over `BRD_I2C` from the Cortex-A55
  (Linux, or U-Boot for DEEPX-rail bring-up), not the CM33, via the
  `chips/da9292` driver; `DA9292_STATUS_FORWARD` answers `0xFF` until
  a HW rev wires the nets to the GD32 (see §3.4).
* Streaming workloads (audio, video) — not in scope; use the
  Renesas direct peripherals for those.

## See also

* `<alp/chips/gd32g553.h>` — host-side public API.
* `gd32-bridge-firmware:README.md` — firmware-tree overview.
* `chips/gd32g553/gd32g553.c` — Renesas-side driver.
* `gd32-bridge-firmware:src/protocol.c` — shared command-handler table.
* `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` — GD32 pad
  allocation.
