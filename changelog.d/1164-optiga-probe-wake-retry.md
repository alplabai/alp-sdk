### Fixed

- `optiga_trust_m_init()` retries its `I2C_STATE` probe (up to 10 tries, 1 ms apart) instead of giving up on the first NACK. The Trust M NACKs the first access after its idle sleep and ACKs the next one, so a single probe reported a fitted part as absent (bench: E1M-V2M103 2026W38-0001, `0x30` on BRD_I2C). New `fake_optiga` emulator in the chips ztest covers the wake NACK and the bounded give-up.
