### Fixed

- **U-Boot GD32_NRST release is now scoped and identity-checked** -- patch 0011 stays on the whole rzv2n family (0012 and 0016 use its code as context) and writes ACT88760 reg 0x10 only when it reads exactly the early-OTP value 0x88 at RIIC8 0x25, so a plain Renesas EVK is never written to.
- **dx-rt firmware-write warning edit is idempotent** -- a re-run of `do_configure` no longer inserts the calls twice and trips the constructor-count `bbfatal`.
