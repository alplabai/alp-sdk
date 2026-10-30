### Fixed — gpio,pwm: keep irq state on failed disable, enforce duty reset on set_period (#2920, #2977)

alp_gpio_irq_disable cleared the cached edge/callback even when the
backend disable_irq failed, so a retry returned ALP_OK while the
interrupt stayed armed. State is now cleared only on backend success.

alp_pwm_set_period documents a reset of the duty to 0 % but only
zephyr_drv did so; the GD32 bridge re-sent the old duty with the new
period. The dispatcher now drops the duty to 0 before the period write
for every backend.
