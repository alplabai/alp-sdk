### Fixed — `alp_adc_stream_close()` no longer recycles a GD32 stream slot whose STREAM_END was never sent (alplabai/alp-sdk#2466)

`alp_adc_stream_close()` (and the open-path rollback when the handle pool is
exhausted after `STREAM_BEGIN`) sent `STREAM_END` only when the V2N supervisor
acquire succeeded, but freed the host-side stream slot unconditionally. A
busy or timed-out acquire left the GD32 stream running while its slot was
handed back, so the next open reusing it got `STREAM_BEGIN` -> `STATUS_INVAL`
forever.

Changes:
- `STREAM_END` is now retried (bounded: 5 x acquire timeout + 20 ms) and the
  slot is freed only once the GD32 confirmed the stream is stopped.
- If it never gets through, the slot stays reserved and a later
  `alp_adc_stream_open()` reports `ALP_ERR_BUSY` instead of colliding with the
  live stream. `alp_adc_stream_close()` is `void`, so this is documented in
  `<alp/adc.h>` rather than returned.
- New `tests/unit/adc_stream_close_slot` drives the real backend against a
  scripted supervisor and a per-slot GD32 model.
