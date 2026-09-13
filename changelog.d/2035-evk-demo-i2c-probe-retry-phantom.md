### Fixed - `aen-evk-demo`'s I2C presence probe couldn't tell a NACK from a bus fault, and only BMI323 could reach `PHANTOM` (#2035)

`i2c_addr_acked()` (`examples/aen/aen-evk-demo/src/main.c`) called
`alp_i2c_read()` once and reported `ABSENT` on any failure.
`alp_i2c_read()`'s documented return set has exactly one failure code for
this probe shape, `ALP_ERR_IO`, covering both "the address NACKed" and "the
transfer itself failed" -- the portable layer cannot tell them apart, so a
transient bus fault on a genuinely fitted part was silently reclassified as
`ABSENT`, which this app's phases never gate on. The probe now retries up
to three times before concluding `ABSENT`; a genuinely absent address NACKs
every attempt, so the retry only costs time on the rarer fault case it
exists to rescue.

Separately, `PHANTOM` (address ACKs, a register read afterward also fails)
was reachable only via BMI323's dedicated post-init diagnostic, even though
nothing about the underlying failure mode -- a part held in reset,
unpowered, or answering with a phantom bus ACK -- is BMI323-specific.
ICM-42670 and BMP581 now re-probe their own `WHO_AM_I`/`CHIP_ID` register
after a failed `init()` (both accessors are safe to call on a context whose
`init()` already failed) and INA236 now re-probes its `MFG_ID` register
(`0x3E`) directly, so all four sensor/rail families can now report `OK` /
`ABSENT` / `BROKEN` / `PHANTOM` instead of three of the four states folding
`PHANTOM` into `BROKEN`.
