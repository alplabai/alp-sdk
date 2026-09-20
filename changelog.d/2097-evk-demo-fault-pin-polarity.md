### Fixed - `aen-evk-demo`'s TAS2563 amp-fault pin print was inverted (#2097)

`phase_sound()` (`examples/aen/aen-evk-demo/src/main.c`) configures
`AMP_FAULT_PIN` (`IRQ_N`, P5_0) plain `GPIO_INPUT` -- no `GPIO_ACTIVE_LOW` --
so `gpio_pin_get()` returns the raw electrical level, not `IRQ_N`'s logical
sense. `IRQ_N` is open-drain, active-low, so raw `1` (pulled high by `R124`)
is idle and raw `0` is the fault. Both baseline and post-shutdown prints had
this backwards: raw `0` printed `"high (no fault)"` and raw `1` printed
`"LOW (asserted!)"`. A healthy, idle-high board printed the opposite word.

Bench evidence (SWD, read-only, E1M-AEN803 serial 2026W36-0002): `EXT_PORTA` (`0x49005050`)
read `0x000000C1`, pad `P5_0` electrically high, both amps' latched-fault
words `0x00000000` -- the line was idle-high throughout; there was never a
fault.

Fixed by swapping the print's interpretation to match the pin's real
polarity, and printing the raw level alongside the verdict so a reader can
always tell which is which: `raw=%d (%s)`, `raw=1` -> `"high, no fault
(IRQ_N idle)"`, `raw=0` -> `"LOW, fault asserted (IRQ_N active-low)"`.

Not routed through `tas2563_fault_asserted()` (`chips/tas2563/tas2563.c`,
already correct): that call needs an `alp_gpio_t *` from `alp_gpio_open()`,
which only resolves pin ids present in this app's `alp,pin-array`
devicetree node (a 3-entry array for the SoM-internal CC3501E nets only --
see the overlay's Phase-11 header for why `AMP_FAULT`/`AMP_ENABLE` reach
`gpio5` directly instead of through that array). Extending the array to
reach this pad was explicitly avoided when the phase was written, to skip
~50 unused placeholder entries; this fix does not reverse that.

Review follow-up: the mapping itself is pulled into a new
`examples/aen/aen-evk-demo/src/amp_fault_verdict.h` (`amp_fault_pin_verdict()`),
same pure-predicate-in-a-header pattern as `bmp581_verdict.h`/
`sound_verdict.h` in that directory, with three new cases in
`tests/zephyr/chips/src/test_audio.c` (`raw=1` idle, `raw=0` asserted, a
negative-errno read failure counted as neither) -- this exact ternary was
inverted once already; a unit test now fails before a bench run does.
