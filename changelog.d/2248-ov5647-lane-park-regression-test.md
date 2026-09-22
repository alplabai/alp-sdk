### Added — runtime regression test locking in the OV5647 lane-park fix (#2248)

`zephyr/drivers/video/ov5647.c`'s header requires the vendored, upstream-
pending driver to be deleted the moment the alp-sdk Zephyr pin advances past
a revision containing upstream PR zephyrproject-rtos/zephyr#119301 -- but not
before the lane-park fix from #2248 is confirmed re-applied or already
present. `tests/zephyr/video_sensors` previously covered OV5647 as
compile-only (no emulator); it now also gets a real runtime ztest against a
new I2C emulator, `ov5647_emul.c`, mirroring the OV9281 pair
(`ov9281_emul.c` / `ov9281_test.c`) already in that suite. Both now sit on
native_sim's real `zephyr,i2c-emul-controller` (`&i2c0`), at their existing
addresses (OV5647 `0x36`, OV9281 `0x60`); OV5647 keeps its 2-lane endpoint.

The suite covers only the lane park -- not `OV5647_PIXEL_RATE` /
`VIDEO_CID_PIXEL_RATE`, a previous "fix" to which was investigated,
disproven and reverted, so pinning that value here would cement a number
nobody has validated. Coverage: the chip-ID probe; the parked quartet
(`0x0100`/`0x4800`/`0x4202`/`0x300d` = `0x01`/`0x25`/`0x0f`/`0x01`) after
`ov5647_init()`; that `0x0100` is written running BEFORE the three park
registers, not just that the four end up at the right values -- parking
while still in software standby writes the identical final register content
but never reaches LP-11 (Stop state), a bug a values-only check cannot see;
unpark on `video_stream_start()`; re-park on `video_stream_stop()`; a
stop-then-start cycle leaving the sensor unparked and streaming; and that
`video_set_format()` leaves the sensor parked again afterward, not stuck in
the software standby `ov5647_set_fmt()` drops into before reprogramming the
PLL bit-mode field.

The ordering assertion is the point of the new emulator: past the register
map it already shares with `ov9281_emul.c`'s I2C-transaction model, it also
records every register WRITE in issue order at
`tests/zephyr/video_sensors/src/ov5647_emul.c:93`
("static void ov5647_emul_log_write(struct ov5647_emul_data *data, uint16_t reg, uint8_t value)"),
and `ov5647_test.c` walks the tail of that log against an expected
register/value sequence at
`tests/zephyr/video_sensors/src/ov5647_test.c:102`
("assert_write_sequence_tail(const struct ov5647_emul_write *expect, size_t n, const char *why)")
rather than only reading back final register contents.

Verified both ways: `west twister -T tests/zephyr/video_sensors -p
native_sim -p native_sim/native/64` passes 15/15 test cases on each platform
(8 OV9281 + 7 new OV5647), and reordering the writes inside
`ov5647_lane_park()` (temporarily, reverted before commit) fails the new
ordering test while leaving the final-register-value test green -- proving
the log-based check catches what a values-only one would miss.
