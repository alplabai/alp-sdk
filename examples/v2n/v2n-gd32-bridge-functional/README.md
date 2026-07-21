# v2n-gd32-bridge-functional

Single-pass **functional** validation of the V2N module's GD32G553
supervisor-MCU bridge, followed by a forever PWM7 duty staircase as
a live oscilloscope observable.

Where [`v2n-gd32-bridge-hil-soak`](../v2n-gd32-bridge-hil-soak)
proves the **link** (every opcode answers, forever), this app proves
the **functions**: each test drives a bridge surface with a known
stimulus and asserts the *value* that comes back -- sqrt(4) is 2.0,
sin(pi/2) is 1.0, a TRNG pull has entropy, an invalid ADC
configuration is *rejected*, a PWM setpoint reads back within
tolerance.

Like the soak and [`v2n-gd32-bridge-ping`](../v2n-gd32-bridge-ping),
this is a maintainer bench tool in example form: it exercises the
`gd32g553` chip driver directly, the documented exception to the
portable-API rule for dedicated bridge demos.

## What it tests (one pass, in table order)

1. **TMU math** -- every native CORDIC primitive value-asserted in
   F32 (sqrt, sin, cos, atan, atan2, hypot, log, sinh, cosh) plus a
   Q31 sqrt; tan/exp/tanh must answer `ALP_ERR_NOSUPPORT` (no native
   mode -- a wrong *value* here would be a defect).
2. **TRNG** -- two 16-byte pulls with the documented one-retry
   fault-recover tolerance; asserts non-constant bytes and that the
   second pull differs.
3. **PWM** -- 1 kHz / 50 % setpoint on channel 7 with readback
   within one tick; `pwm_configure` defaults must answer OK while
   the center-aligned knob may answer `NOSUPPORT` (documented v0.3
   partial).
4. **ADC** -- the illegal 14-bit / no-oversampling combination must
   be *rejected* (`INVAL`/`NOSUPPORT`), then all 8 channels are
   swept against the physical ceiling.
5. **DSP chain pool** -- lifecycle open, accepting the documented
   pre-wave-2 `NOSUPPORT` contract.
6. **Identity** -- `GET_VERSION` twice (stable), and the DA9292
   status forward returns the 0xFF "no nets on this HW rev"
   sentinel.

## Reading the verdict (no console on this SoM)

The M33 target has no console; results publish to a RAM verdict
block.  Find the `func_results` symbol in `zephyr.map` and read it
over the DAP:

| Word    | Meaning                                                     |
|---------|-------------------------------------------------------------|
| `[0]`   | `0xF07C7E57` magic                                          |
| `[1]`   | state: 1 = testing, 2 = staircase running (tests done), `0xDEAD` = link never came up |
| `[2]`   | pass count                                                  |
| `[3]`   | fail count                                                  |
| `[4+i]` | per-test result: 0 = PASS, `0x7E` = value assertion failed (status was OK), other = the failing `alp_status_t` |
| `[40]`  | staircase: current duty per-mille (live)                    |
| `[41]`  | staircase: step counter (liveness)                          |

## The scope observable

After the suite the app parks in a forever **PWM7 duty staircase**:
a 1 kHz carrier whose duty walks 10 % -> 90 % in 10-point steps,
2 s per step, then wraps.  PWM7 doubles as the EVK LED pad, so a
scope probe shows the high time visibly widening every 2 s -- and
the naked eye sees a brightness ramp.  Every step is a fresh
`PWM_SET` + `PWM_GET` pair over the bridge, so the staircase doubles
as a slow link soak.

## Build

`board.yaml` targets the E1M-V2M101 SoM's M33 core on the
`e1m-x-evk` preset (SPI is the 25 MHz fast path under test; the
suite passes `i2c = NULL`):

```bash
tan build examples/v2n/v2n-gd32-bridge-functional
```

The twister row is `build_only` on `native_sim/native/64` -- there
is no GD32 to answer off-silicon, so CI proves compile + link and
the value assertions run on the bench.

## See also

* [`v2n-gd32-bridge-ping`](../v2n-gd32-bridge-ping) -- minimal liveness probe.
* [`v2n-gd32-bridge-hil-soak`](../v2n-gd32-bridge-hil-soak) -- full-opcode forever soak.
* [`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) -- wire spec.
* [`docs/gd32-bridge.md`](../../../docs/gd32-bridge.md) -- firmware tree overview.
* [`<alp/chips/gd32g553.h>`](../../../include/alp/chips/gd32g553.h) -- host driver API.
