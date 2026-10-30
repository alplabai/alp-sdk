### Fixed — blocks,docs: button_led callback race, pdm_mic stub tracking, shim README (#2987)

button_led: set_press_callback stored press_cb and cb_user as two plain
stores while the GPIO edge IRQ could be live, so a press in between paired
one registration's callback with another's user pointer. The IRQ is now
masked before the pair is updated, with a conformance test for the race. The
header no longer claims all three OS targets (only the Zephyr module builds
blocks/).

pdm_mic: documented as a permanent stub with an @par Tracking issue; the
Kconfig help no longer says it waits for alp_i2s_*.

alp-stock-shim README: drop the E1M-V2M103 ship-image claim and state that
only the V2N101 / V2M101 boards have release conf and overlay files.
