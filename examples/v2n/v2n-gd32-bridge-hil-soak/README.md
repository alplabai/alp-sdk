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
| gpio | 0x10/0x11 | routed-mask read OK (bit 8 = E1M IO24 unrouted, excluded); mask=0 write (provable no-op) |
| pwm_set_get | 0x20/0x21 | 1 kHz / 25 % readback within 1 %; parked at 0 % after |
| pwm_single_pulse | 0x26 | status OK on PWM4 (TIMER7; PWM0..3 share a timer an earlier row holds -> BUSY) (waveform = scope row) |
| pwm_capture | 0x23/24/25 | begin/end OK; read OK, NOT_READY (empty ring) or NOSUPPORT (no edges on bench) |
| adc_read | 0x30 | 4 samples ≤ VREF |
| adc_stream | 0x33/34/35 | two-read paced assertion: read 1 (after 50 ms @ 1 kHz) returns exactly the 32-sample cap; read 2, taken immediately after, returns 1–30 leftover samples — 32 again would mean free-running (rate ignored), 0 would mean a dead stream |
| adc_stream_guard | 0x33/35 + 0x30 | while `adc_stream` owns a converter (ch0, ADC3), a single-shot `adc_read` on its sibling channel (ch1) must be refused (`ALP_ERR_BUSY`); a different converter's channel (ch4, ADC1) must still succeed; after `STREAM_END` the sibling read must succeed again (guard not sticky) |
| dac | 0x50/0x51 | 600 mV readback ± 16 mV (jumper-safe under the 1.8 V rail — DAC0 sits jumpered straight to the ADC0 pad on the X-EVK Tier-B bench); parked at 0 after |
| qenc | 0x60/0x61 | reset OK, read OK — no encoder is attached on the bench, so the A/B inputs float and the position value isn't asserted, only the reset/read round-trip |
| counter | 0x70 | strictly increasing across 200 µs |
| trng | 0x80 | two 16-byte pulls: non-constant + distinct |
| tmu | 0x90 | sqrt(4)=2 ± 1e-3, sin(0)=0 ± 1e-3 |
| timer_sync | 0x27 | link TIMER0→TIMER7 (restart) then unlink |
| power_mode | 0x28 | mode 0 ("run") — the documented no-op request |
| da9292_sentinel | 0x40 | **must** answer `0xFF` (no DA9292 net reaches the GD32 this HW rev) |
| ota_get_state | 0xF5 | NOSUPPORT (unarmed build) or a sane state snapshot (armed build) |
| adc_stream2 | 0x3B/0x3C/0x35 | v0.15, self-gating on the `ADC_STREAM2` grant: `BEGIN2` reports the realised rate exactly (1 MHz / 1000 ticks, full scale 4095); two `READ2` calls 50 ms apart at 1 kHz are contiguous (`first_index` advances by `got`, `dropped` 0, codes <= full scale), and the driver counts zero accounting gaps |
| batch | 0x04 | v0.15, self-gating on the `BATCH` grant: `PING` + full-mask `GPIO_READ` + `COUNTER_READ` in one transaction pair, all three OK with their fixed 4-byte payloads |

## Protocol v0.15 and the ATTN line

`gd32g553_init_ex()` negotiates the v0.15 link features with a bridge
that reports minor >= 15: `STATUS_SEQ`, `BIG_FRAME` (256-byte frames),
`ADC_STREAM2` and `BATCH`.  A v0.14 bridge keeps the legacy 1-byte
`STATUS_SEQ` form, so the same binary soaks both: the two rows above pass on
a link that did not grant their feature and the 20 legacy rows run
unchanged.

`ATTN` -- the GD32's data-ready output on `PA14`, wired to Renesas `P71` -- is
**not** requested by this soak.  The pad ids that name it
(`GD32G553_PAD_ID_*`) are reserved for the V2N supervisor singleton and the
SWD driver: the portable `alp_gpio_open()` refuses them, because `P71` is
also the GD32's `SWCLK` and the neighbouring `NRST` is a net shared with the
PMIC.  A chip-driver soak therefore runs on the staging-gap path; `ATTN`
rides under every portable `alp_pwm` / `alp_adc` / ... call through the
supervisor, whose hook time-stamps the edges the driver accepts.

Link telemetry for the SWD reader (no console) is in
`v015_forensics`: granted feature word, `ATTN` active (always 0 here),
replies delivered on an edge, lost edges, stuck-high readings, `READ2`
accounting gaps.

## Reading the verdict from Linux (no J-Link)

SRAM0 is readable only through a CM33 J-Link.  The same verdict is also
published as a compact, versioned 20-word record in the `rsctbl` window,
A55 `0x4F700F00` (CM33-NS `0x9F700F00`), right below the liveness beacon
at `0x4F700FF0` that provisioning's `cm33_running` reads.  Layout and
field meanings: `include/alp/protocol/gd32_bridge_results.h`.  On the
A55 (root, `/dev/mem`):

```bash
python3 read_gd32_results.py          # scripts/bench/v2n/read_gd32_results.py
python3 read_gd32_results.py --fault  # the fatal-error block, if the CM33 died
python3 read_gd32_results.py --no-live  # skip the 1.5 s heartbeat check
```

By default the beacon heartbeat is sampled twice, 1.5 s apart, so a frozen
CM33's last words are not mistaken for a current result.  Output includes
`fail_rows` (indices of failed rows) and `fail_row_names`.  Exit codes: `0`
valid (and heartbeat advancing), `2` no valid record, `3` `/dev/mem`
unreadable, `4` stale record (not a result-publishing image), `5` STALLED
(heartbeat not advancing), `6` `--fault` and no fault block recorded.

`tests/hil/v2m103-x-evk/v2m103-gd32-bridge-results.yaml` asserts it.
The soak refreshes the record once per cycle; `soak_cycles`, `soak_errors`, `soak_timeouts` (lost `ATTN` edges) and `soak_elapsed_s` are the soak counters, `tests_skip` counts self-gating v0.15 rows skipped on a bridge that did not grant the feature.


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

Per cycle: `[hil-soak] cycle N | 22/22 PASS`.  Every 16 cycles a
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
