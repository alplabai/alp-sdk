### Fixed — backends: rtc rebase and shared state, spi fail-closed CS, usb rejects unapplied descriptors (#2930, #2931, #2932, #2979)

lprtc_calendar_shim: shim_open re-seeded the shared state on every open and
wiped a time set through another handle; it now seeds once. get_time tracked
only one 32-bit CCVR wrap (36.4 h); whole elapsed seconds are now folded into
epoch_base on every get/set and by a 6 h k_timer, under a spinlock. get_time,
set_time and the rebase timer read the counter inside _state_lock, so a read
taken before a concurrent set_time/rebase can no longer underflow
(now - tick_snapshot) and jump the clock ~36.4 h.

spi zephyr_drv: open succeeded with no chip select when cs_pin_id could not be
resolved, and ignored the gpio configure result; both now fail the open.

usb zephyr_drv: z_dev_open accepted VID/PID/bcd/strings it never applied;
non-default descriptors now return ALP_ERR_NOSUPPORT.

usb.h documents the Zephyr backend refusing non-default descriptors;
peripheral.h alp_spi_open @return lists the cs_pin_id INVAL and mapped driver
errors.

Tests: native_sim unit tests for the lprtc multi-wrap rebase, the counter read
under lock and open keeping a set time; for the USB descriptor guard; a spi
unresolvable-CS open case.
