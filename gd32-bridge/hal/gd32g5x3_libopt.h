/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gd32-bridge's project-level standard-peripheral selector.  Both the
 * GD32G5x3 CMSIS header (`gd32g5x3.h`) and any peripheral source that
 * includes it pull in this file by name -- the vendor archive's
 * Examples follow the same convention, each example carrying its own
 * libopt.h selector to keep the per-image flash footprint minimal.
 *
 * Pattern: each time `hal/bridge_hw_gd32.c` (or any peer that uses
 * the standard-peripheral library) starts calling into a new
 * peripheral driver, add the matching `gd32g5x3_<periph>.h` include
 * here so the driver's prototype + register layouts are visible to
 * the translation unit.
 *
 * Today the file lists every header the eventual real-hook
 * implementations enumerated in `hal/bridge_hw_gd32.c`'s top comment
 * will need.  Including a header that isn't yet referenced is cheap
 * (header-only -- the driver's .c lives in
 * `vendors/gd32_firmware_library/upstream/Firmware/.../Source/`,
 * pulled into the static library by the vendor wrapper's CMake);
 * the build flags `-ffunction-sections -fdata-sections` +
 * `-Wl,--gc-sections` drop the unreferenced symbols at link time.
 */

#ifndef GD32G5X3_LIBOPT_H
#define GD32G5X3_LIBOPT_H

/* Core / clock tree / interrupts / power. */
#include "gd32g5x3_rcu.h"
#include "gd32g5x3_misc.h"
#include "gd32g5x3_pmu.h"

/* GPIO + EXTI for the bridge's IO surface. */
#include "gd32g5x3_gpio.h"
#include "gd32g5x3_exti.h"
#include "gd32g5x3_syscfg.h"

/* Timers (advanced + general purpose + low-power) for PWM / capture
 * / single-pulse / encoder / free-running counter. */
#include "gd32g5x3_timer.h"
#include "gd32g5x3_lptimer.h"

/* Analog: ADC + DAC + comparator + reference. */
#include "gd32g5x3_adc.h"
#include "gd32g5x3_dac.h"
#include "gd32g5x3_cmp.h"
#include "gd32g5x3_vref.h"

/* DMA + trigger routing for ADC streaming + FFT/FAC pipelines. */
#include "gd32g5x3_dma.h"
#include "gd32g5x3_trigsel.h"

/* DSP acceleration: TMU (CORDIC), FFT, FAC. */
#include "gd32g5x3_tmu.h"
#include "gd32g5x3_fft.h"
#include "gd32g5x3_fac.h"

/* Security: TRNG + CAU (AES/DES). */
#include "gd32g5x3_trng.h"
#include "gd32g5x3_cau.h"

/* Serial buses (host bridge + DA9292 + carrier I2C). */
#include "gd32g5x3_spi.h"
#include "gd32g5x3_i2c.h"
#include "gd32g5x3_usart.h"

/* Flash controller for OTA + factory provisioning. */
#include "gd32g5x3_fmc.h"

/* Watchdogs + RTC for system supervision. */
#include "gd32g5x3_fwdgt.h"
#include "gd32g5x3_wwdgt.h"
#include "gd32g5x3_rtc.h"

#endif /* GD32G5X3_LIBOPT_H */
