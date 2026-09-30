### Fixed — OPTIGA Trust M probe recovers a part that went silent after idle (alplabai/alp-sdk#2507)

On E1M-V2M103 the Trust M answers `I2C_STATE` (write `0x82`, read 4) while it is
accessed about once a second and right after an SE reset, but after more than
about 10 s idle it NACKs every register-address write, however long the host
polls, and only an SE reset brings it back. `optiga_trust_m_init()` then
reported `ALP_ERR_NOT_READY` for a fitted part.

Changes:
- New `optiga_trust_m_init_with_reset()` takes an optional RESET hook. When the
  probe spends the host library's NACK-polling budget (`PL_POLLING_MAX_CNT` tries),
  the driver pulses RESET once (low for the library's `RESET_LOW_TIME_MSEC`,
  then waits its `STARTUP_TIME_MSEC`) and probes again. `ALP_ERR_NOT_READY` now
  means the part stayed silent even after a hardware reset.
  `optiga_trust_m_init()` is unchanged (no hook). The hook is a parameter, not
  a context field, so `optiga_trust_m_t` keeps its layout.
- On the Linux V2N/V2M image the kernel's `alplab,gd32-bridge-gpio` driver owns
  the GD32's BRD_I2C address, and a userspace `I2C_RDWR` bridge frame would
  interleave with it. So SE_RST goes through the driver: `0005-gpio-add-gd32-bridge-expander-driver.patch`
  adds a 21st line, index 20 `se-rst` (named in `e1m-v2n-som.dtsi`
  `gpio-line-names`), whose `.set()` sends `CMD_SE_RESET` (`0x41`, assert byte)
  instead of `GPIO_WRITE`. It is outside the replay mask, so a bridge reset
  leaves the part released. Needs a kernel rebuild to take effect.
- `v2n-secure-element-sign` and `v2n-brd-i2c-bringup` find that line by name on
  the chip labelled `gd32-bridge-gpio` through the GPIO character device
  (`src/se_reset_gpio.h`, raw `GPIO_V2` ioctls, no libgpiod) and pass it as the
  reset hook. On a kernel without the line they pass no hook and probe plainly.
  The example no longer opens the GD32 itself, so `v2n-secure-element-sign`
  drops its `gd32g553` include.
- Not covered: `read_product_info`/`send_apdu` on a context that idles after
  init still have no reset-and-retry (follow-up, #2517).
- New `tests/unit/optiga_trust_m_idle_reset` models the post-idle NACK pattern
  against the real driver and vendored library.

Status: verified with a host mock and a syntax build of both examples; the kernel
patch is checked against the BSP source only. The on-silicon bench check (plain
init returns `-2` after idle, `init_with_reset` returns 0 with one `se-rst`
pulse) is pending. Each `se-rst` set is one bridge I2C frame, so the pulse width
is at least the 2 ms the driver sleeps and may need lengthening if it does not
revive the part.
