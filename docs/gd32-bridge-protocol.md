# GD32 bridge protocol (V2N supervisor MCU)

> **Scope.** This document is the **wire** specification for the
> Renesas RZ/V2N ⇄ GD32G553MEY7TR bridge on the E1M-X V2N / V2N-M1
> SoMs.  Both sides — the host driver under
> `chips/gd32g553/` and the GD32-side firmware under `firmware/gd32-bridge/` —
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
the framing layer differs.

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

Both transports carry the same command set.  Command opcodes are 1
byte; their numeric encoding is:

| Opcode | Name                  | Payload (request)                                  | Reply payload                                      |
|--------|-----------------------|----------------------------------------------------|----------------------------------------------------|
| `0x00` | `PING`                | _empty_                                            | `0x00` (`ALP_OK`)                                  |
| `0x01` | `GET_VERSION`         | _empty_                                            | `major:u8 minor:u8 patch:u8` (`major.minor.patch`) |
| `0x02` | `GET_BUILD_ID`        | _empty_                                            | `build_id:char[20]` (truncated SHA-1, ASCII hex)   |
| `0x03` | `RESET_REASON`        | _empty_                                            | `cause:u8` (`gd32g553_reset_cause_t`)              |
| `0x10` | `GPIO_READ`           | `mask:u32`                                         | `levels:u32` (masked subset)                       |
| `0x11` | `GPIO_WRITE`          | `mask:u32 levels:u32`                              | _empty_                                            |
| `0x20` | `PWM_SET`             | `channel:u8 reserved:u8 period_ns:u32 duty_ns:u32` | _empty_                                            |
| `0x21` | `PWM_GET`             | `channel:u8`                                       | `period_ns:u32 duty_ns:u32`                        |
| `0x30` | `ADC_READ`            | `channel:u8 samples:u8`                            | `mv[samples]:u16` (millivolt, raw averaged)        |
| `0x40` | `DA9292_STATUS_FORWARD` | _empty_                                          | `da9292_faults:u8` (always `0xFF` on this HW rev — see §3.4) |
| `0x41` | `SE_RESET` (v0.8)       | `assert:u8` (`0` = release, `1` = hold in reset) | _empty_ (see §3.15) |
| `0x50` | `DAC_SET`             | `channel:u8 reserved:u8 value_mv:u16`              | _empty_                                            |
| `0x51` | `DAC_GET`             | `channel:u8`                                       | `value_mv:u16`                                     |
| `0x60` | `QENC_READ`           | `encoder:u8`                                       | `position:i32`                                     |
| `0x61` | `QENC_RESET`          | `encoder:u8`                                       | _empty_                                            |
| `0x70` | `COUNTER_READ`        | `counter:u8`                                       | `ticks:u32`                                        |
| `0x22` | `PWM_CONFIGURE`       | `channel:u8 align:u8 dead_time_ns:u32 break_cfg:u8` | _empty_ (defaults only -- see §3.8; non-default `align`/`dead_time_ns`/`break_cfg` return `STATUS_NOSUPPORT`, tracked #495) |
| `0x32` | `ADC_CONFIGURE`       | `channel:u8 reserved:u8 oversample:u16 sample_cycles:u16 resolution:u8` | _empty_ (`sample_cycles` sticky -- see §3.9; non-default `oversample`/`resolution` return `STATUS_NOSUPPORT`, tracked #494) |
| `0x33` | `ADC_STREAM_BEGIN`    | `stream_id:u8 channel:u8 reserved:u8 sample_rate_hz:u32` | _empty_                                      |
| `0x34` | `ADC_STREAM_READ`     | `stream_id:u8 max_samples:u8`                      | `got:u8 mv[max_samples]:u16` (zero-padded)         |
| `0x35` | `ADC_STREAM_END`      | `stream_id:u8`                                     | _empty_                                            |
| `0x80` | `TRNG_READ`           | `len:u8` (1..32)                                   | `random_bytes[len]`                                |
| `0x90` | `TMU_COMPUTE`         | `function:u8 format:u8 reserved:u16 in_a:u32 in_b:u32` | `result:u32`                                  |
| `0x36` | `ADC_STREAM_CONFIGURE_DSP` | _(reserved tombstone -- see §3.x)_             | _(empty; returns `STATUS_NOSUPPORT` permanently -- use `CMD_ADC_DSP_CHAIN_*` instead)_ |
| `0x23` | `PWM_CAPTURE_BEGIN`   | `channel:u8 edge:u8`                                  | _empty_ (see §3.y -- reconfigures the pad as input-capture) |
| `0x24` | `PWM_CAPTURE_READ`    | `channel:u8`                                          | `period_ns:u32 pulse_ns:u32` (see §3.y -- returns `STATUS_NOSUPPORT` ("ring empty") until an edge lands; no edges land on this V2N HW rev pending a pad-routing rework) |
| `0x25` | `PWM_CAPTURE_END`     | `channel:u8`                                          | _empty_                                            |
| `0x26` | `PWM_SINGLE_PULSE`    | `channel:u8 reserved:u8 reserved:u16 pulse_ns:u32`    | _empty_                                            |
| `0x27` | `TIMER_SYNC`          | `master:u8 slave:u8 mode:u8`                          | _empty_                                            |
| `0x28` | `POWER_MODE_SET`      | `mode:u8 reserved:u8 wake_bitmap:u32 wake_after_ms:u32` | _empty_ (see §3.z -- `wake_bitmap` bits `UART_RX`/`USB`/`ETH_LINK` return `STATUS_NOSUPPORT`; no HW path on GD32G5) |
| `0x81` | `LINK_FEATURES` (v0.7) | `features:u8` (wanted; bit0 = `STATUS_SEQ`)          | `features:u8` (granted + armed; see §3.14 / §4.1.1) |

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

Three opcodes own the upload path, and all three are implemented
today: `chain_open` / `stage_push` / `chain_bind` allocate, upload,
validate, and bind a chain (see the per-opcode subsections below).
What has NOT landed yet is the runtime side -- a bound chain's
stages are not applied to the stream's samples.
`CMD_ADC_STREAM_READ` still emits raw mV values from the DMA ring
regardless of any bound chain (tracked #496).  The host-side
standalone API in `<alp/dsp.h>` ships working in v0.5.0 (runs the
chain locally with CMSIS-DSP or the portable C fallback over
in-RAM buffers), so application code can test against the same
chain primitives today and pick up the bridge-offloaded runtime
path once the FFT/FAC dispatch lands inside
`bridge_hw_adc_stream_read()`.

#### `CMD_ADC_DSP_CHAIN_OPEN` (`0x37`)

| Direction | Layout              |
|-----------|---------------------|
| Request   | (empty)             |
| Reply     | `chain_id:u8`       |

Allocates a fresh chain handle from the firmware's pool (size
`GD32G553_BRIDGE_ADC_DSP_MAX_CHAINS`, default 4 chains).  The
opaque `chain_id` is what the host passes to the subsequent
STAGE_PUSH and CHAIN_BIND calls.  Exhausting the pool returns
`STATUS_NOMEM`.  Chains auto-release when the bound stream's
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
`0x33`).  The firmware validates the chain and stores the binding
on both sides (see the failure conditions below), but does not yet
change what `CMD_ADC_STREAM_READ` returns -- the runtime dispatch
that would route the stream's samples through the bound chain
(instead of the raw mV values the DMA ring already emits) has not
landed (tracked #496).  Once it does, the read reply's payload
format is expected to become mode-dependent on the chain's
terminal stage:

* No FFT terminal: filter samples (`i16` for Q31 coefficients,
  `i16` shape-equivalent for F32 -- bridge-mapped to mV
  semantics for compatibility with the legacy raw read path).
* FFT terminal with `output_format == COMPLEX`:  interleaved
  `(re, im)` f32 pairs.
* FFT terminal with `output_format == MAGNITUDE`:  per-bin f32
  magnitudes (half the wire bandwidth of COMPLEX).

`CHAIN_BIND` fails (`STATUS_INVAL`) if:

* `stream_id` is out of range (`>= GD32G553_BRIDGE_ADC_STREAM_COUNT`),
* `chain_id` does not name an open chain,
* the chain has unfinished stages (some `[0, chunk_total_size)`
  range was not covered),
* the chain violates the ordering rules from `<alp/dsp.h>` (FFT
  must be terminal; WINDOW must immediately precede FFT).

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
bit-to-pad mapping is documented in `firmware/gd32-bridge/README.md` and
mirrored in the host driver header.  The host MUST NOT assume that
bit `n` corresponds to GD32 pad `Pxn`.

`GPIO_WRITE` is atomic in the firmware: read-modify-write of the
pad output register is done with interrupts disabled around the
masked update so a concurrent `GPIO_WRITE` on the other transport
cannot interleave a partial state.

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
`BRD_I2C` from the CM33 via `da9292_get_status()` in the
`chips/da9292` driver — see `<alp/chips/da9292.h>`.

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
separate timer PLL), so the achievable resolution is ~4.63 ns LSB
and the longest single-counter period is ~303 us.  `CMD_PWM_GET` reads the live
timer registers (auto-reload + compare) and converts ticks back to
nanoseconds -- it reports what the pad is actually generating, never
an echo of the request.  Two consequences of the hardware truth:
the period is shared per timer (a `PWM_SET` on a sibling channel of
the same timer moves this channel's reported period too), and before
the first `PWM_SET` a channel reports the boot default (65.536 ms
period, 0 duty).  Callers that need exact frequency confirmation
should read back rather than recompute.

**Current GD32 HAL status:** only the default configuration is
applied.  `bridge_hw_pwm_configure()` accepts `align_mode == 0`,
`dead_time_ns == 0`, and `break_cfg == 0` (the reset defaults
`pwm_timer_init()` already programs); any non-default value
returns `STATUS_NOSUPPORT`.  Center-aligned modes, dead-time, and
break-input configuration need a per-timer apply path (these
fields are shared across the sibling channels of a `TIMER0`/
`TIMER7` timer) that has not landed yet (tracked #495).

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
* `sample_cycles` is one of the eight datasheet-defined sample-time
  steps (2/6/12/24/47/92/247/640 cycles per GD32G553 RM §16.4.6);
  0 means "firmware default".  Rounds down.
* `resolution_bits` is 0 (default) / 6 / 8 / 10 / 12 / 14 / 16.
  14- and 16-bit modes require `oversample_ratio >= 4` and `>= 16`
  respectively per the datasheet's effective-resolution table; the
  firmware enforces.  Unsupported widths reply with `STATUS_INVAL`.

The GD32 returns ADC readings as 16-bit millivolts (`ADC_READ` /
`ADC_STREAM_READ` reply payload format unchanged); higher
oversampling improves the effective-resolution / SNR of the
returned values but doesn't widen the on-wire word.

**Current GD32 HAL status:** only `sample_cycles` is sticky.
`bridge_hw_adc_configure()` accepts `oversample_ratio == 1` and
`resolution_bits == 12` (the defaults); any other value returns
`STATUS_NOSUPPORT`.  The oversampling-apply and alternate-
resolution paths have not landed yet (tracked #494).

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
either (a) consult `firmware/gd32-bridge/README.md` for the firmware's
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

### 3.14 Link-feature negotiation (`v0.7+`)

`LINK_FEATURES` (0x81) negotiates opt-in framing upgrades.  The host
sends the feature set it wants (`features:u8`); the firmware grants
the intersection with what it implements, **arms it immediately**,
and echoes the granted set.  A request of `0` disables everything
(idempotent both ways).  Pre-v0.7 firmware answers
`STATUS_NOSUPPORT` through the dispatch default, so a new host
degrades to the legacy framing automatically — and an un-negotiated
v0.7 link is byte-identical to the old wire.

Defined feature bits:

| Bit | Name | Effect when granted |
|-----|------|---------------------|
| 0 | `STATUS_SEQ` | Every **SPI** reply STATUS byte carries a 4-bit slave-side sequence stamp in bits `[7:4]` (§4.1.1).  The reply to the negotiation itself is already stamped — the host takes that stamp as its baseline.  I2C replies are never stamped. |

After a firmware reboot the feature resets to off (replies revert to
unstamped); the host's normal link-recovery path (re-init →
`GET_VERSION`) re-negotiates.

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
| STATUS  | 1     | Reply only.  Bits `[3:0]` = status code (`0x0` = OK, §6).  Bits `[7:4]` = the v0.7 **sequence stamp** — zero until `LINK_FEATURES` negotiates `STATUS_SEQ` (§4.3), so the legacy wire is unchanged. |
| PAYLOAD | N / M | Length is **opcode-derived** — both ends know the byte count from the opcode + status pair.   |
| CRC     | 2     | CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, xor-out 0x0000, **non-reflected**), MSB first |

Length is **not** carried on the wire because a single opcode has a
fixed request-payload width and a status-code-determined reply-payload
width — keeping the envelope at 1+1+N+2 bytes minimises the
fast-path overhead and avoids ambiguity at the SPI boundary
(SPI doesn't have a natural "end-of-frame" token).

Variable-length replies (`ADC_READ`, …) carry the length as the
first byte of the payload (`samples` for `ADC_READ`) — same byte the
host already sent in the request, **echoed back** before the data.

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
Because a stale verdict proves the request was never executed, the
re-send is safe even for non-idempotent opcodes.  The stamp wraps
mod 16; replies re-served across the wrap remain detectable because
detection compares against the last accepted stamp, not zero.
I2C replies are **never** stamped (`STATUS_NO_PENDING` owns bit 7
on that transport, and the hazard is SPI-specific).

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

If the host issues a `read` before any `write` since the last
START, the firmware replies with one byte `STATUS = 0x80` (no
pending command) and an empty payload + CRC.

### 5.1 CRC on I2C

The CRC of an I2C transaction covers `CMD | PAYLOAD` on write and
`STATUS | PAYLOAD` on read — the I2C address byte is NOT included,
matching the convention used by most smart-battery / SMBus PEC
protocols.  The polynomial and parameters are the same as SPI
(CRC-16/CCITT-FALSE), so both transports share one
verification routine in `firmware/gd32-bridge/src/protocol.c`.

### 5.2 Slave-address overlap

The default GD32 slave address `0x70` is **not** occupied by any
chip on the V2N BRD_I2C bus (verified against
`metadata/e1m_modules/E1M-V2N101.yaml`).  When a board
allocates BRD_I2C to a device that conflicts, the firmware is
rebuildable with a different `CONFIG_GD32G553_BRIDGE_I2C_ADDR` —
host code reads back the address from `GET_VERSION` reply payload
in future protocol revisions (TODO; today the host has to know the
configured address out-of-band).

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
| `0x06`        | `ALP_ERR_NOSUPPORT`     | Opcode unknown to this firmware build.                 |
| `0x07`        | `ALP_ERR_NOMEM`         | Reserved.                                              |
| `0x08`        | `ALP_ERR_OUT_OF_RANGE`  | Parameter beyond hardware capability (e.g. PWM freq).  |
| `0x80`        | _(I2C: no pending cmd)_ | I2C read before any write on this START — see §5.      |
| Other         | `ALP_ERR_IO` (mapped)   | Unknown wire status → host returns `ALP_ERR_IO`.       |

Hosts MUST translate the wire byte back to a negative `alp_status_t`
via the table above before returning it from a public API call.

**v0.7 SPI note:** once `STATUS_SEQ` is negotiated (§3.14), the SPI
status byte carries the sequence stamp in bits `[7:4]` — hosts mask
with `0x0F` before the table lookup (every SPI-visible code fits the
low nibble; `0x80 NO_PENDING` is I2C-only, where stamping never
applies).  Masking is safe unconditionally on SPI: legacy firmware
never sets the high nibble there.

## 7. Liveness handshake

`gd32g553_init()` issues:

1. `PING` with a 100 ms timeout — confirms the link physically
   answers and the firmware is not wedged.
2. `GET_VERSION` — caches the major.minor.patch into the driver
   context.  If `major` mismatches `GD32G553_HOST_PROTOCOL_MAJOR`,
   the init returns `ALP_ERR_NOSUPPORT` and refuses to operate on
   incompatible firmware (avoids a host that speaks newer commands
   talking past an older firmware build).
3. `GET_BUILD_ID` (optional, only if the host logs it) — useful in
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
* `minor` is bumped when **opcodes are added** that older hosts
  don't have to know about.
* `patch` is bumped on documentation or non-observable firmware
  changes.

Until the bridge fleet ships in production, `major` stays at `0`
and the host driver insists on **exact** major-number match.
**Pre-1.0 the host and firmware additionally ship in lock-step
including `minor`**: a pre-production minor may revise a
pre-production opcode's payload (v0.6 did this to `OTA_WRITE_CHUNK`),
and the major-only handshake cannot detect that — do not mix a v0.5
host with v0.6 firmware or vice versa.

Version history (pre-1.0): **v0.7** adds `LINK_FEATURES` (0x81) +
the negotiated `STATUS_SEQ` reply stamp (§3.14, §4.1.1) and the
additive `OTA_BEGIN` version triple (§10) — both backward-compatible
by construction (un-negotiated framing and the 8-byte BEGIN are
byte-identical to v0.6).  **v0.8** adds `SE_RESET` (0x41, §3.15) — a
new opcode; older hosts simply never send it.

## 9. Reference vectors

The canonical sanity-check for the CRC implementation is the
universally-cited CRC-16/CCITT-FALSE result over the ASCII string
"123456789":

* `crc16_ccitt_false("123456789") == 0x29B1`

The per-opcode wire vectors (SPI `PING` round-trip, I2C `PING`
round-trip, `GET_VERSION` reply for the firmware's declared
version) are generated at firmware build time and stored in
`firmware/gd32-bridge/tests/protocol_vectors.txt`.  Both the host-side
driver tests under `tests/zephyr/chips/gd32g553/` and the
firmware-side unit tests under `firmware/gd32-bridge/tests/` consume
that file so the two implementations cannot diverge.

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
  overwritten by a field upgrade; two 236 KB **slots** sit in upper
  flash with an A/B metadata pair between them.  The active slot
  runs at boot while the inactive slot receives the upgrade;
  roll-back is a metadata flip + reset.  Destructive flashing is
  armed only in `-DBRIDGE_OTA_PARTITIONED` firmware builds — the
  default build answers `STATUS_NOSUPPORT` to the whole range.
* Uses the same SPI / I2C transport as the rest of the protocol --
  no extra wiring beyond what is already in place.

Wire contract (host mirrors in `<alp/chips/gd32g553.h>`,
firmware in `firmware/gd32-bridge/src/ota.c`):

| Op | Name | Request payload | Reply payload |
|------|------|----------------|---------------|
| `0xF0` | `OTA_BEGIN` | `size:u32 expected_crc32:u32` `[fw_major:u8 fw_minor:u8 fw_patch:u8]` (v0.7 additive: the incoming image's version, recorded into the A/B metadata `fw_version[slot]` at COMMIT; legacy 8-byte form ⇒ 0 = unknown, and pre-v0.7 firmware ignores the trailing triple) | `chunk_max:u16 target_slot:u8` |
| `0xF1` | `OTA_WRITE_CHUNK` | `offset:u32 len:u8 data[len]` | `received_bytes:u32` (high-water) |
| `0xF2` | `OTA_VERIFY` | _empty_ | `computed_crc32:u32 verified:u8` |
| `0xF3` | `OTA_COMMIT` | _empty_ | _empty_ (resets on success) |
| `0xF4` | `OTA_ROLLBACK` | _empty_ | _empty_ (resets on success) |
| `0xF5` | `OTA_GET_STATE` | _empty_ | `state:u8 active:u8 pending:u8 boot_count:u16` |
| `0xF6` | `OTA_ABORT` | _empty_ | _empty_ |

Value encodings: `state` = 0 IDLE / 1 READY / 2 BUSY / 3 VERIFIED /
4 ERROR; slot bytes = 0 A / 1 B / `0xFF` none-pending.  `WRITE_CHUNK`
offsets must land on 8-byte (FMC doubleword) boundaries; the image
CRC-32 is IEEE 802.3 reflected (zlib-compatible).  `WRITE_CHUNK` and
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
  and full PMIC register status over `BRD_I2C` from the CM33 via the
  `chips/da9292` driver; `DA9292_STATUS_FORWARD` answers `0xFF` until
  a HW rev wires the nets to the GD32 (see §3.4).
* Streaming workloads (audio, video) — not in scope; use the
  Renesas direct peripherals for those.

## See also

* `<alp/chips/gd32g553.h>` — host-side public API.
* `firmware/gd32-bridge/README.md` — firmware-tree overview.
* `chips/gd32g553/gd32g553.c` — Renesas-side driver.
* `firmware/gd32-bridge/src/protocol.c` — shared command-handler table.
* `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` — GD32 pad
  allocation.
