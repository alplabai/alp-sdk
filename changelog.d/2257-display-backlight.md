### Added — `alp,display-backlight`: a display's default brightness is set by the SDK at boot (#2257)

A shield names its backlight LED (a `pwm-leds` or `gpio-leds` child) and a percentage in an `alp,display-backlight` node, and `src/zephyr/display_backlight.c` calls `led_set_brightness()` once at APPLICATION init, so an application that opens the display needs no backlight code. The RVT121 shield declares its 500 Hz UTIMER3 PWM at 30%. The RK055 shield has no node: its backlight enable is the HX8394 driver's own `bl-gpios`.
