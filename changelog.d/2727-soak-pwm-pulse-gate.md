### Fixed — GD32 soak image no longer fails `pwm_single_pulse` on pre-0.17 firmware (#2727)

On bridge firmware below protocol minor 17 the soak cannot release a
TIMER7 channel a previous CM33 image left running (`gd32g553_pwm_stop`
needs minor 17), so `pwm_single_pulse` answered `ALP_ERR_BUSY` every
cycle. That case is now reported as a skip, not a failure. On protocol
0.17.0 (fw `0.3.1+9329d0e09322ed`, E1M-V2M103) the soak ran 134 cycles
with 0 failures and `fail_mask` 0.

`docs/gd32-bridge-protocol.md` now says a single-shot `ADC_READ` on a
streaming converter answers `STATUS_BUSY` in the stream section too,
matching the guard description.
