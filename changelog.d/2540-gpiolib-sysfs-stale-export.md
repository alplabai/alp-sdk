### Fixed — writing a stale GPIO number to `/sys/class/gpio/export` no longer oopses the V2N kernel (#2540)

On kernel 6.1.141-cip43, unbinding and rebinding `gpio-gd32-bridge` moves the
chip base (394 -> 372 in the reproduction) while Wi-Fi/BT consumers keep the old
`gpio_device` alive with a NULL chip. Exporting a number from the old range
oopsed in `gpiochip_line_is_valid`. New kernel patch
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0015-gpiolib-sysfs-reject-export-of-a-number-in-a-chipless-gpio_device.patch`
makes `export_store()` return `-ENODEV` instead. It is a local guard, not an
upstream backport, and the check-then-use window against a concurrent chip
removal is not closed. Not build-tested against a kernel tree.
