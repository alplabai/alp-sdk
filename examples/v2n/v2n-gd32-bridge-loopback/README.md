<!--
Copyright 2026 Alp Lab AB
SPDX-License-Identifier: Apache-2.0
-->

# v2n-gd32-bridge-loopback

**Jumpered Tier-B loopback validation of the GD32 supervisor bridge.**

Where the sibling `v2n-gd32-bridge-functional` example drives each
bridge surface against a known-exact software answer and reads ADC pads
that float on the bench, this tier closes the loop in **copper**: three
physical jumpers on the **E1M-X V2** carrier route a bridge *output*
back into a bridge *input*, so the analog and timer signal paths get
validated end to end on real silicon.

It runs the three tests **once**, publishes a verdict block the V2N DAP
reads over SWD (there is no console on this SoM), and then idles
forever. Every stimulus is parked at 0 on the way out of its test.

> This is a maintainer bench tool in example form. Like the functional
> tier it exercises the `gd32g553` chip driver **directly** -- the
> documented exception to the portable-API rule for dedicated bridge
> demos. On an **unwired** board every value assertion fails; that is
> expected. The suite is only meaningful on a board jumpered per the
> table below.

---

## Bench wiring (E1M-X V2 carrier, wired 2026-06-04)

> _Wiring photo placeholder:_ add a labelled bench photo here showing
> the three jumper wires seated on the header pins below
> (`docs/img/v2n-gd32-bridge-loopback-jumpers.jpg`, not yet captured).

| Jumper | From (header.pin) | To (header.pin) | Signal path |
| ------ | ----------------- | --------------- | ----------- |
| **A** | **raw `DAC0` net** (E1M-X pin A19; tapped at the OPA189 U23 input pad after lifting the op-amp's input -- see safety note 1) | `P7.1` (CK_ANA) | **Direct 1:1** analog loopback -> raw passthrough to E1M-X pin **A17 = ANA_S0 = bridge ADC channel 0** (GD32 `PD9`, ADC3_CH12, via the SoM's 22R series R273). The carrier's J15.2 `DAC0_OUT` buffer is NOT usable on this rev: both OPA189s are supplied from the **VAU2 rail, which has no source** (next-rev item). |
| **B** | `J26.14` (CK_PWM1) | `J18.7` (ENC1_X) | **LSF0108** pass-FETs on both sides (bidirectional, transparent). PWM bridge **ch1** (`PB1`, TIMER0_MCH2) -> encoder **index 1** X input (`PC6`, TIMER2 CH0). Y (`PC7`) floats with firmware pull-up = static **HIGH**. The 22R series R102 is benign. |
| **C** | `J26.10` (CK_PWM2) | `J26.8` (CK_PWM3) | Both B-side ports of the **same LSF0108 (U18)** -- no contention. PWM bridge **ch2** (`PB14`, TIMER0_MCH1) output -> PWM bridge **ch3** (`PC5`, TIMER0_MCH3) rebound as input capture. |

---

## ⚠️ Safety -- read before plugging anything in

1. **The OPA189 DAC buffers are dead on this carrier rev** -- they ride
   the **VAU2** rail, which is not connected to any source. A dead
   op-amp's input ESD clamp loads the raw DAC0 net to ~0.3 V, so the
   buffers' **inputs must be lifted off the net** before the direct tap
   is usable (bench rework 2026-06-04). With the rework in place the
   loopback is same-rail 1.8 V -> 1.8 V and physically cannot overdrive
   the ADC pad; `DAC_MAX_SAFE_MV` (1500 mV) in `src/main.c` is a
   linearity bound (stay off the rail-clip region), not an electrical
   cap. If a future rev powers VAU2 and the buffered J15.2 path
   returns, the x2 gain makes everything above ~850 mV an over-rail
   hazard again -- re-derive the bound before rewiring.

2. **No physical rotary encoder may be plugged into `J18`** during the
   qenc test. Jumper B drives `ENC1_X` from the PWM bridge; an external
   encoder would contend the line.

---

## The three tests

### 1. `t_dac_adc_loopback` (Jumper A)
For each setpoint in `{150, 450, 900, 1350}` mV: command DAC0, settle
3 ms, then read ADC channel 0 (a burst of 4 independent samples; the
assertion takes the first, the burst makes a noisy connection visible
in the forensics). Expected reading **equals the command** (direct 1:1
wiring, both converters on the same 1.8 V VREF). Tolerance is **±(25 mV + 2% of expected)** --
offset/INL of the converter pair plus scale error; tighter than a
buffered path because no external gain resistors remain in the loop.
The DAC is parked at 0 on every exit path, including failures.

### 2. `t_pwm_capture_loopback` (Jumper C)
Drive PWM ch2 at **200 Hz, 50 % duty** (5 ms period, 2.5 ms high),
rebind ch3 as a **both-edges** input-capture source, settle 10 ms, then
read in a **tight poll loop** (up to 80 reads, no inter-read delay) that
treats `ALP_ERR_NOSUPPORT` as the **documented "no fresh edge yet, poll
again" sentinel**. Asserts pulse width in `[2400000, 2600000]` ns
(2.5 ms ± 100 us).

Two choices make this robust on the **shared-timer** loopback (ch2 and
ch3 both ride TIMER0):
- **50 % duty** -- the both-edges machine measures the delta between
  *adjacent* edges; at 50 % the high and low times are equal
  (period/2 = 2.5 ms), so the pulse width is the same regardless of
  which edge armed the capture (no phase ambiguity).
- **slow rate + tight polling** -- at 200 Hz the edges are 2.5 ms apart,
  far wider than one bridge transaction (~150 us), so the host catches
  three *consecutive* edges. (At the old 1 kHz with a 5 ms retry ladder
  the three samples were non-consecutive edges and the delta was
  meaningless -- the bug that made this read 0 before fw v0.2.7.)

**The period is deliberately not asserted.** Stimulus (ch2) and capture
(ch3) share TIMER0, so the same-edge "period" delta is exactly one
counter wrap and reads ~0 -- a documented degeneracy, not a fault. The
raw `period_ns` and `pulse_width_ns` are still recorded for forensics.
The firmware (fw v0.2.7) takes all edge deltas modulo the counter
period so the wrap underflow no longer poisons the pulse-width reading.

### 3. `t_pwm_qenc_stimulus` (Jumper B)
Reset encoder 1, drive `ENC1_X` with a 1 kHz 50% square from PWM ch1,
wait 100 ms, read `pos1`, wait 10 ms, read `pos2`, park ch1. Asserts all
statuses OK **and** `|pos1| ≤ 8` **and** `|pos2| ≤ 8`.

In X4 quadrature decode with Y held static HIGH, a lone toggling X
cannot accumulate net position -- each X edge with an unchanging Y is an
ambiguous transition the decoder treats as **±1 dither** about the
origin. The bound is deliberately **loose** for this first silicon pass:
it exists to catch the failure mode this loopback guards against -- a
genuinely floating ENC input free-ran to *thousands* of counts. The raw
`pos1`/`pos2` are recorded so the bound can be tightened from silicon
truth.

---

## Verdict block

A static `volatile uint32_t loopback_results[32]` the bench reads over
SWD. Each word:

| Slot | Meaning |
| ---- | ------- |
| `[0]` | magic `0xB10CBAC4` (sanity-check the symbol + image before trusting the rest) |
| `[1]` | state: `0` = init, `1` = running, `2` = done (idle forever after); `0xDEAD` = SPI never opened |
| `[2]` | pass count |
| `[3]` | fail count |
| `[4..11]` | per-record code (cursor order: 4x DAC setpoints, then capture, then qenc): `0` = PASS, `0x7E` = transport OK but value assertion failed, anything else = the failing `alp_status_t` (two's complement) |
| `[12..15]` | the four raw DAC->ADC readings (mV), in `{150, 450, 900, 1350}` setpoint order |
| `[16]` | raw capture `period_ns` (forensics; **not** asserted -- shared-timer wrap degeneracy) |
| `[17]` | raw capture `pulse_width_ns` |
| `[18]` | raw qenc `pos1` (cast to u32 from int32_t) |
| `[19]` | raw qenc `pos2` (cast to u32 from int32_t) |
| `[20..31]` | reserved (0) |

### Reading the block over J-Link

There is no console on this SoM, so the verdict block is read over SWD.
The block's address is **not fixed** -- pull it from the build's ELF:

```sh
arm-none-eabi-nm build/zephyr/zephyr.elf | grep loopback_results
# -> <addr> B loopback_results
```

Then read 32 words from `<addr>` with your J-Link script (e.g.
`mem32 <addr> 32` in the J-Link Commander, or the bench's
burst-sampler). Confirm `[0] == 0xB10CBAC4` and `[1] == 2` (done)
before trusting `[2]`/`[3]` and the raw forensics slots.

---

## Build

```sh
# from the repo root, native_sim build-only (CI artifact; bench-only at runtime)
west build -b native_sim/native/64 examples/v2n/v2n-gd32-bridge-loopback
```

This example is **V2N-only and jumpered-bench-only**. It builds clean
everywhere as a CI artifact (`build_only: true`), but only produces
meaningful results on an E1M-X V2 carrier wired per the table above.
