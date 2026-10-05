### Fixed

- **U-Boot GD32_NRST release is now scoped and identity-checked** -- patch 0011 applies only on the ALP E1M machines (not a plain Renesas EVK) and writes ACT88760 reg 0x10 only when it reads exactly the early-OTP value 0x88.
- **dx-rt firmware-write warning edit is idempotent** -- a re-run of `do_configure` no longer inserts the calls twice and trips the constructor-count `bbfatal`.
