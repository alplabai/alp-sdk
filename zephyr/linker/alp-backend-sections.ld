/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend-registry linker sections for Zephyr ld-script targets.
 *
 * ALP_BACKEND_REGISTER (include/alp/backend.h) places each backend
 * row in a custom input section `alp_backends_<class>`, and every
 * class dispatcher's ALP_BACKEND_DEFINE_CLASS references the GNU-ld
 * bound symbols `__start_alp_backends_<class>` /
 * `__stop_alp_backends_<class>`.  On the host (native_sim) the
 * default linker script leaves those sections orphans, GNU ld
 * auto-places them and auto-emits the bound symbols, and everything
 * just works.  Real boards are different: Zephyr links with
 * `--orphan-handling=warn` AND `--fatal-warnings`, so every orphan
 * `alp_backends_*` section is a hard link error -- the sections must
 * be declared explicitly, and once declared explicitly ld no longer
 * auto-emits the __start/__stop symbols, so they are defined by hand
 * here.
 *
 * The snippet is pulled into the rodata region (between
 * __rodata_region_start and __rodata_region_end) via
 * zephyr_linker_sources(ROM_SECTIONS ...) in zephyr/CMakeLists.txt:
 * the rows are const and the selector only ever reads them, so they
 * live in ROM/flash on XIP builds and cost no RAM.
 *
 * Entries pack contiguously: every row is
 * aligned(__alignof__(alp_backend_t)) with a sizeof that is a
 * multiple of that alignment, so the `++cursor` walk in
 * src/backend.c reads no inter-entry padding (see the alignment note
 * on ALP_BACKEND_DEFINE_CLASS).
 *
 * MAINTENANCE: one ALP_BACKEND_SECTION_ROM line per backend class.
 * Adding a class (a new ALP_BACKEND_DEFINE_CLASS) means adding one
 * line here, or ARM links break with the same orphan-section error
 * this file exists to prevent.  Keep the list in sync with
 * `grep -rh "ALP_BACKEND_DEFINE_CLASS(" src/`.
 */

#define ALP_BACKEND_SECTION_ROM(class) \
	SECTION_PROLOGUE(alp_backends_##class,,) \
	{ \
		__start_alp_backends_##class = .; \
		KEEP(*(alp_backends_##class)) \
		__stop_alp_backends_##class = .; \
	} GROUP_LINK_IN(ROMABLE_REGION)

/* The class-range table walked by alp_backend_select(). */
SECTION_PROLOGUE(alp_backend_classes,,)
{
	__start_alp_backend_classes = .;
	KEEP(*(alp_backend_classes))
	__stop_alp_backend_classes = .;
} GROUP_LINK_IN(ROMABLE_REGION)

/* One per-class row section per ALP_BACKEND_DEFINE_CLASS. */
ALP_BACKEND_SECTION_ROM(adc)
ALP_BACKEND_SECTION_ROM(audio)
ALP_BACKEND_SECTION_ROM(ble)
ALP_BACKEND_SECTION_ROM(camera)
ALP_BACKEND_SECTION_ROM(can)
ALP_BACKEND_SECTION_ROM(counter)
ALP_BACKEND_SECTION_ROM(dac)
ALP_BACKEND_SECTION_ROM(display)
ALP_BACKEND_SECTION_ROM(dsp)
ALP_BACKEND_SECTION_ROM(gpio)
ALP_BACKEND_SECTION_ROM(gpu2d)
ALP_BACKEND_SECTION_ROM(i2c)
ALP_BACKEND_SECTION_ROM(i2s)
ALP_BACKEND_SECTION_ROM(inference)
ALP_BACKEND_SECTION_ROM(jpeg)
ALP_BACKEND_SECTION_ROM(mproc)
ALP_BACKEND_SECTION_ROM(mproc_boot)
ALP_BACKEND_SECTION_ROM(mqtt)
ALP_BACKEND_SECTION_ROM(power)
ALP_BACKEND_SECTION_ROM(power_profile)
ALP_BACKEND_SECTION_ROM(pwm)
ALP_BACKEND_SECTION_ROM(qenc)
ALP_BACKEND_SECTION_ROM(rpc)
ALP_BACKEND_SECTION_ROM(rtc)
ALP_BACKEND_SECTION_ROM(security)
ALP_BACKEND_SECTION_ROM(soc_info)
ALP_BACKEND_SECTION_ROM(spi)
ALP_BACKEND_SECTION_ROM(storage)
ALP_BACKEND_SECTION_ROM(tmu)
ALP_BACKEND_SECTION_ROM(uart)
ALP_BACKEND_SECTION_ROM(update_log)
ALP_BACKEND_SECTION_ROM(usb)
ALP_BACKEND_SECTION_ROM(wdt)
ALP_BACKEND_SECTION_ROM(wifi)
