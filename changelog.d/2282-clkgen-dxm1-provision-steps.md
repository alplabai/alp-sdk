### Added

- `provision_som.py run` gains two V2N/V2M steps: `clkgen_verify` reads the
  on-SoM 5L35023B clock generator's OTP image one byte at a time and checks
  it (with U-Boot's per-boot register fixup accounted for) against the
  bench-proven factory image, and records `clkgen_otp_raw` and
  `clkgen_i2c_addr`. `dxm1_npu_flash` programs the V2M DX-M1 NPU's SPI-NAND
  over its UART recovery path and verifies a PCIe link after a cold boot; it
  is **bench-pending** (it has never run on silicon) and skipped unless
  `--enable-dxm1-flash` is passed. See `docs/provisioning-v2n.md`.
