### Added — `tcal9538_set_polarity_inversion()` (#2035)

New public symbol on the TCAL9538/TCA9538 I/O-expander driver
(`include/alp/chips/tcal9538.h`, `chips/tcal9538/tcal9538.c`):
`alp_status_t tcal9538_set_polarity_inversion(tcal9538_t *ctx, uint8_t mask)`.
It writes only the polarity-inversion register (`0x02`) -- bit N of `mask`
XORs pin N's electrical level before it lands in the input port register
(`0x00`); it never touches the output port (`0x01`) or the configuration
register (`0x03`), so it is safe to call regardless of a pin's configured
direction. ABI snapshot: hash `b97b24bb3c8ffe20`, the single new entry.

Added so `aen-evk-demo`'s I/O-expander phase can prove a genuine register
round-trip through the chip (write the mask, confirm the input read-back
actually inverted, then restore it) without ever driving or reconfiguring
any output pin.
