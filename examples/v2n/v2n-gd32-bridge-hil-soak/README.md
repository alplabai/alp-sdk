# v2n-gd32-bridge-hil-soak

Pass/fail soak of the **whole GD32 bridge command set** over the
25 MHz SPI fast path.  Where [`v2n-gd32-bridge-ping`](../v2n-gd32-bridge-ping)
proves the link, this proves the opcodes: every command the bridge
firmware implements round-trips each cycle with a self-contained
verification — readback compares (PWM, DAC), range sanity (ADC,
reset-reason), monotonicity (counter), entropy (TRNG), exact math
(TMU sqrt/sin), and documented-sentinel asserts (DA9292 `0xFF`,
OTA `NOSUPPORT` on the unarmed image).  No external instruments
needed; the instrument-grade checks (scope on PWM, precision source
on ADC) remain their own HIL-PLAN rows.

## What it exercises per cycle

| Test | Opcodes | Self-verification |
|------|---------|-------------------|
| ping | 0x00 | status OK (4-byte even-parity reply) |
| get_version | 0x01 | matches init version (7-byte odd-parity reply) |
| get_build_id | 0x02 | non-empty + constant across the soak |
| reset_reason | 0x03 | in-range cause |
| gpio | 0x10/0x11 | full-mask read OK; mask=0 write (provable no-op) |
| pwm_set_get | 0x20/0x21 | 1 kHz / 25 % readback within 1 %; parked at 0 % after |
| pwm_single_pulse | 0x26 | status OK (waveform = scope row) |
| pwm_capture | 0x23/24/25 | begin/end OK; read OK or NOSUPPORT (no edges on bench) |
| adc_read | 0x30 | 4 samples ≤ VREF |
| adc_stream | 0x33/34/35 | **>0 samples after 50 ms @ 1 kHz — the stream-DMA DMAMUX regression test** |
| dac | 0x50/0x51 | 1650 mV readback ± 16 mV; parked at 0 after |
| qenc | 0x60/0x61 | reset → position reads exactly 0 |
| counter | 0x70 | strictly increasing across 200 µs |
| trng | 0x80 | two 16-byte pulls: non-constant + distinct |
| tmu | 0x90 | sqrt(4)=2 ± 1e-3, sin(0)=0 ± 1e-3 |
| timer_sync | 0x27 | link TIMER0→TIMER7 (restart) then unlink |
| power_mode | 0x28 | mode 0 ("run") — the documented no-op request |
| da9292_sentinel | 0x40 | **must** answer `0xFF` (no DA9292 net reaches the GD32 this HW rev) |
| ota_get_state | 0xF5 | NOSUPPORT (unarmed build) or a sane state snapshot (armed build) |

One-shot at boot (not per-cycle): `adc_dsp_chain_open` probe — the
4-chain pool has no close opcode yet, so looping it would exhaust the
pool and poison the stats.

**Quarantine history** (resolved — nothing is skipped today; every
row in the test table runs active): the soak's first silicon runs
(2026-06-04) caught six HAL surfaces failing from cycle 1 —
`pwm_capture`, `adc_stream`, `qenc`, `tmu`, `ota_get_state`, `trng` —
and quarantined all six.  Five of them (`pwm_capture`, `qenc`, `tmu`,
`ota_get_state`, `trng`) turned out to be two compounding host/
transport bugs, not HAL defects: a `transport_spi.c` slave-cursor
rewind bug that made a slow handler's reply permanently unreadable,
plus the host masking every error reply's true status behind a
generic `ALP_ERR_IO`.  `trng` additionally had a real, now-handled
hardware condition — the unit latches a fault (`TRNG_STAT = 0x48`)
after an intermittent seed error; firmware now detects the latch and
rebuilds instead of hanging the reply window.  `adc_stream` was
different: a real, separate firmware defect, and its failure mode
was genuinely destructive — a failed `STREAM_END` left the 1 kHz
circular DMA contending with the SPI slave's channels until the link
rotted.  Root cause was `CTL1.DDM` never set (circular DMA stalls by
design without it) plus the `RCU_DMAMUX` clock never explicitly
enabled on the stream path.  Both bug classes were fixed 2026-06-04
(commits `2e30b9d09`, `ffdc66ee5`).

**Validation record**: **253/253 across a 20-row HIL soak** on fw
v0.2.8 + v0.2.9 (2026-06-06, see `docs/test-plan.md`) — every row
above, including `adc_stream` and `trng`, clean with zero
quarantined entries.

## Reading the output

Per cycle: `[hil-soak] cycle N | 19/19 PASS`.  Every 16 cycles a
cumulative per-test table prints, ending in a greppable verdict line:
`SOAK-CLEAN` (zero failures everywhere) or `SOAK-DIRTY`.  Failures
never halt the soak — they print one diagnosable line (test name +
`alp_status_t`), count, and the soak keeps hammering.  If PING fails
two cycles running, the soak re-runs the blocking init (transport
recovery) and continues.

Cold-boot autonomy is inherited from the ping example: init retries
every 200 ms until the GD32 answers, so the soak survives power
cycles with no fixed boot delay anywhere.

## See also

* [`v2n-gd32-bridge-ping`](../v2n-gd32-bridge-ping) — the link-level bring-up demo this builds on.
* [`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) — wire spec.
* [`<alp/chips/gd32g553.h>`](../../../include/alp/chips/gd32g553.h) — host driver API.
