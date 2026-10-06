### Fixed — OPTIGA Trust M register reads: STOP between write and read, bounded wake retry (alplabai/alp-sdk#2507)

Bench, E1M-V2M103 unit 0008, Trust M at `0x30` on BRD_I2C (i2c-8), 2026-10-06:
- `i2ctransfer w1@0x30 0x82 r4@0x30` (register write plus repeated-start read)
  NACKed 130/130. The same two halves as separate messages with a STOP between
  them (`w1@0x30 0x82`, then `r4@0x30`) ACKed 10/10 and returned
  `0x08 0x80 0x00 0x00`.
- The first access after an idle gap (1 s, 10 s, 60 s) always NACKs (0/100,
  0/20, 0/5); back-to-back accesses ACK. Accesses spaced 20 ms or more apart
  NACK again, so the part sleeps within about 20 ms of a NACKed access unless
  it is re-accessed at once. A GD32-driven SE reset does not change this.

`chips/optiga_trust_m` already sent the register address and the data read as
two transfers and retried the pair on a NACK (`PL_POLLING_MAX_CNT` = 200 tries
at 1 ms, about 400 ms bound). It never issued a combined write-read. This change
documents that contract in the probe and locks it with tests in
`tests/unit/optiga_trust_m_idle_reset`: `alp_i2c_write_read` is never called, a
first NACK followed by an ACK succeeds without a reset, and a persistent NACK
returns `ALP_ERR_NOT_READY` after the bound. Public API and error codes are
unchanged. No Linux/Yocto backend or example issues a write-read to `0x30`.
