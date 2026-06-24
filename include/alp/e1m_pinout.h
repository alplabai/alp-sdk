/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file e1m_pinout.h
 * @brief Global, fixed pin/instance map derived from the E1M
 *        standard.
 *
 * The E1M open standard (`alplabai/e1m-spec`) fixes the board-side
 * pinout: which peripheral instances exist (`I2C0`, `SPI0`, …) and
 * which physical pads carry their default function plus the
 * GPIO-secondary IO numbers (`IO0`, `IO1`, …, `PWM0`–`PWM7`,
 * `ENC0`–`ENC3`).  Because that mapping is **silicon- and
 * board-agnostic**, the integers the SDK consumes can be baked
 * here as portable C macros — every E1M-conformant board
 * resolves them identically.
 *
 * Derived from `alplabai/e1m-spec/pinout/v1.json` (E1M v1.0,
 * 35 × 35 mm, 312 pads).  Pinned to `e1m-spec` v1.0.
 *
 * ## Per-board feature names
 *
 * Names like `USER_LED_RED` or `ENCODER_SW` are **not** in E1M —
 * they're board-specific.  Those live in
 * `<alp/boards/<board>.h>` and reference the macros below for
 * their underlying pin/instance integers.
 *
 * ## Devicetree / overlay invariant
 *
 * The order of entries in a board's `alp,pin-array` `gpios`
 * property MUST match the GPIO-pad enumeration here:
 *
 *   1. `IO0`–`IO25` in numeric order                  (indices 0..25)
 *   2. `PWM0`–`PWM7` in numeric order                 (indices 26..33)
 *   3. `ENC0_X`, `ENC0_Y`, `ENC1_X`, `ENC1_Y`, … `ENC3_Y` (34..41)
 *   4. `ADC0`–`ADC7` in numeric order                 (indices 42..49)
 *   5. `DAC0`, `DAC1`                                 (indices 50..51)
 *
 * Boards that don't route a particular pad still leave its slot
 * in `alp,pin-array` (with `status = "disabled"` on the GPIO node)
 * so the indices stay stable.  `alp_gpio_open()` for a non-routed
 * pad returns `NULL`.
 *
 * ## Pin-as-GPIO fallback
 *
 * Every analog/timer-default pad has a parallel `E1M_GPIO_<class><N>`
 * GPIO index.  An app that doesn't need the ADC/PWM/QENC/DAC
 * function of a pad can claim it as a digital GPIO by opening the
 * matching `E1M_GPIO_<class><N>` index -- the board's devicetree
 * muxes that pad to its GPIO node, and the SDK's GPIO backend
 * configures the pre-routed GPIO (it does not itself tear down the
 * analog/timer block).  Don't route the same pad to both its
 * analog/timer function and GPIO at once; the silicon is shared.
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.1 + 2026-05-14 prefix-rename pre-v1.0; pinned by e1m-spec.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef E1M_PINOUT_H
#define E1M_PINOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Peripheral instance IDs                                             */
/* ================================================================== */

/* I2C — bus_id passed to alp_i2c_open(). */
#define E1M_I2C0 0u
#define E1M_I2C1 1u

/* SPI — bus_id passed to alp_spi_open(). */
#define E1M_SPI0 0u
#define E1M_SPI1 1u

/* UART — port_id passed to alp_uart_open(). */
#define E1M_UART0 0u
#define E1M_UART1 1u

/* I2S */
#define E1M_I2S0 0u
#define E1M_I2S1 1u

/* I3C (single instance on E1M v1) */
#define E1M_I3C0 0u

/* PDM microphone (digital) */
#define E1M_PDM0 0u
#define E1M_PDM1 1u

/* CAN (single instance routed on E1M v1) */
#define E1M_CAN0 0u

/* Ethernet (single MAC routed on E1M v1; ETH1 lives on E1M-X) */
#define E1M_ETH0 0u

/* MIPI CSI / DSI (single instance routed on E1M v1) */
#define E1M_CSI0 0u
#define E1M_DSI0 0u

/* Parallel-camera, SDIO, USB */
#define E1M_PARCAM0 0u
#define E1M_SDIO0   0u
#define E1M_USB0    0u
#define E1M_USB2    0u

/* Camera LDOs (per-channel power feedback) */
#define E1M_CAM0 0u
#define E1M_CAM1 1u

/* DAC outputs */
#define E1M_DAC0 0u
#define E1M_DAC1 1u

/* Single-ended ADC channels (ANA_S0..ANA_S7 ⇒ ADC0..ADC7) */
#define E1M_ADC0 0u
#define E1M_ADC1 1u
#define E1M_ADC2 2u
#define E1M_ADC3 3u
#define E1M_ADC4 4u
#define E1M_ADC5 5u
#define E1M_ADC6 6u
#define E1M_ADC7 7u

/* PWM channel IDs — passed as `channel_id` to alp_pwm_open().
 * E1M reserves eight PWM channels at fixed pads
 * (PWM0=A6, PWM1=B6, PWM2=A5, …, PWM7=B3). */
#define E1M_PWM0 0u
#define E1M_PWM1 1u
#define E1M_PWM2 2u
#define E1M_PWM3 3u
#define E1M_PWM4 4u
#define E1M_PWM5 5u
#define E1M_PWM6 6u
#define E1M_PWM7 7u

/* Quadrature-encoder IDs — passed as `encoder_id` to alp_qenc_open().
 * E1M reserves four encoders (ENC0..ENC3), each routed as a
 * complementary pad pair (ENCn_X / ENCn_Y). */
#define E1M_ENC0 0u
#define E1M_ENC1 1u
#define E1M_ENC2 2u
#define E1M_ENC3 3u

/* DAC channel IDs: use the pin-granular E1M_DAC0 / E1M_DAC1
 * defined above in the peripheral-instance section.  The earlier
 * `E1M_DAC_CH0` / `_CH1` aliases (same values) were removed
 * pre-1.0 -- callers don't pick "DAC peripheral 0 channel 0",
 * they pick the pin. */

/* Counter / generic-timer instance IDs — passed as `counter_id` to
 * alp_counter_open(). */
#define E1M_COUNTER0 0u
#define E1M_COUNTER1 1u
#define E1M_COUNTER2 2u
#define E1M_COUNTER3 3u

/* Watchdog instance IDs — passed as `wdt_id` to alp_wdt_open(). */
#define E1M_WDT0 0u
#define E1M_WDT1 1u

/* RTC instance IDs — passed as `rtc_id` to alp_rtc_open(). */
#define E1M_RTC0 0u

/* CAN bus IDs — passed as `bus_id` to alp_can_open().
 * E1M reserves a single CAN-FD pad pair (CAN0); higher indices map
 * to vendor-specific extensions where the SoC provides them. */
/* (E1M_CAN0 is defined above in the peripheral instance section.) */

/* JTAG (single instance) */
#define E1M_JTAG0 0u

/* PCIe — not routed on E1M (35 × 35); present on E1M-X. */

/* ================================================================== */
/* E1M-spec instance counts (the portability bound)                   */
/* ================================================================== */
/*
 * Every E1M-conformant SoM SHALL route at least these many instances
 * of each peripheral class.  An app that uses only `E1M_<CLASS><N>`
 * for `N < E1M_<CLASS>_COUNT` stays cross-SoM portable; apps that
 * use higher indices are tapping vendor-specific extensions exposed
 * through the SDK's loose upper bound (e.g. `peripheral_can.c`
 * accepts up to 6 channels because the V2N has 6, but only CAN0 is
 * portable).
 *
 * These constants come from the `alplabai/e1m-spec` v1.0 pinout —
 * see `docs/e1m-pinout.md` for how they thread through the studio's
 * pin allocator.
 */
#define E1M_I2C_COUNT     2u
#define E1M_SPI_COUNT     2u
#define E1M_UART_COUNT    2u
#define E1M_I2S_COUNT     2u
#define E1M_I3C_COUNT     1u
#define E1M_PDM_COUNT     2u
#define E1M_CAN_COUNT     1u
#define E1M_ETH_COUNT     1u
#define E1M_CSI_COUNT     1u
#define E1M_DSI_COUNT     1u
#define E1M_PARCAM_COUNT  1u
#define E1M_SDIO_COUNT    1u
#define E1M_USB_COUNT     1u
#define E1M_USB2_COUNT    1u
#define E1M_DAC_COUNT     2u
#define E1M_ADC_COUNT     8u
#define E1M_PWM_COUNT     8u
#define E1M_ENC_COUNT     4u
#define E1M_GPIO_IO_COUNT 26u /**< IO0..IO25; some "Reserved — not present on v1.0". */

/* ================================================================== */
/* GPIO indices (`pin_id` passed to alp_gpio_open)                    */
/* ================================================================== */

/* General-purpose IOs (silkscreen IO0..IO25 per E1M v1.0).
 * Indices 0..25 — the canonical numeric order. */
#define E1M_GPIO_IO0  0u  /**< E1M pad L2  */
#define E1M_GPIO_IO1  1u  /**< E1M pad L1  */
#define E1M_GPIO_IO2  2u  /**< E1M pad W2  */
#define E1M_GPIO_IO3  3u  /**< E1M pad AG2 */
#define E1M_GPIO_IO4  4u  /**< E1M pad AG16 */
#define E1M_GPIO_IO5  5u  /**< E1M pad AH18 */
#define E1M_GPIO_IO6  6u  /**< E1M pad AG18 */
#define E1M_GPIO_IO7  7u  /**< E1M pad AG34 */
#define E1M_GPIO_IO8  8u  /**< E1M pad AG33 */
#define E1M_GPIO_IO9  9u  /**< E1M pad AH34 */
#define E1M_GPIO_IO10 10u /**< E1M pad AH33 */
#define E1M_GPIO_IO11 11u /**< E1M pad A18  */
#define E1M_GPIO_IO12 12u /**< Reserved — not present on v1.0 (kept for ABI stability) */
#define E1M_GPIO_IO13 13u /**< E1M pad E3   */
#define E1M_GPIO_IO14 14u /**< Reserved — not present on v1.0 */
#define E1M_GPIO_IO15 15u /**< E1M pad F3   */
#define E1M_GPIO_IO16 16u /**< E1M pad G3   */
#define E1M_GPIO_IO17 17u /**< E1M pad H3   */
#define E1M_GPIO_IO18 18u /**< E1M pad I3   */
#define E1M_GPIO_IO19 19u /**< E1M pad J3   */
#define E1M_GPIO_IO20 20u /**< E1M pad K3   */
#define E1M_GPIO_IO21 21u /**< E1M pad L3   */
#define E1M_GPIO_IO22 22u /**< E1M pad M3   */
#define E1M_GPIO_IO23 23u /**< E1M pad N3   */
#define E1M_GPIO_IO24 24u /**< E1M pad O3   */
#define E1M_GPIO_IO25 25u /**< E1M pad P3   */

/* PWM-capable pads (silkscreen PWM0..PWM7).  Indices 26..33. */
#define E1M_GPIO_PWM0 26u /**< E1M pad A6 */
#define E1M_GPIO_PWM1 27u /**< E1M pad B6 */
#define E1M_GPIO_PWM2 28u /**< E1M pad A5 */
#define E1M_GPIO_PWM3 29u /**< E1M pad B5 */
#define E1M_GPIO_PWM4 30u /**< E1M pad A4 */
#define E1M_GPIO_PWM5 31u /**< E1M pad B4 */
#define E1M_GPIO_PWM6 32u /**< E1M pad A3 */
#define E1M_GPIO_PWM7 33u /**< E1M pad B3 */

/* Quadrature-encoder pads (silkscreen ENCx_X / ENCx_Y).
 * GPIO secondary; the default function is the hardware encoder.
 * Indices 34..41 in (X,Y) pairs per encoder unit. */
#define E1M_GPIO_ENC0_X 34u /**< E1M pad A10 */
#define E1M_GPIO_ENC0_Y 35u /**< E1M pad B10 */
#define E1M_GPIO_ENC1_X 36u /**< E1M pad A9  */
#define E1M_GPIO_ENC1_Y 37u /**< E1M pad B9  */
#define E1M_GPIO_ENC2_X 38u /**< E1M pad A8  */
#define E1M_GPIO_ENC2_Y 39u /**< E1M pad B8  */
#define E1M_GPIO_ENC3_X 40u /**< E1M pad A7  */
#define E1M_GPIO_ENC3_Y 41u /**< E1M pad B7  */

/* Single-ended ADC pads as GPIO indices (silkscreen ANA_S0..ANA_S7).
 * Default function is the analog input; opening these via
 * `alp_gpio_open()` uses them as digital GPIOs when the board's
 * devicetree routes the pad's GPIO node (the backend configures the
 * pre-routed GPIO; it does not itself disable the ADC channel).
 * Indices 42..49.
 *
 * Note: the peripheral-instance ID `E1M_ADC<N>` (0..7) passed to
 * `alp_adc_open()` and the GPIO-index `E1M_GPIO_ADC<N>` (42..49)
 * passed to `alp_gpio_open()` refer to the SAME physical pad in
 * two different namespaces.  Don't open both -- they share silicon.
 * See `docs/board-config.md` "Pin-as-GPIO fallback" for the
 * convention. */
#define E1M_GPIO_ADC0 42u
#define E1M_GPIO_ADC1 43u
#define E1M_GPIO_ADC2 44u
#define E1M_GPIO_ADC3 45u
#define E1M_GPIO_ADC4 46u
#define E1M_GPIO_ADC5 47u
#define E1M_GPIO_ADC6 48u
#define E1M_GPIO_ADC7 49u

/* DAC output pads as GPIO indices (silkscreen ANA_OUT0 / OUT1).
 * Same pin-as-GPIO fallback as ADC above.  Indices 50..51. */
#define E1M_GPIO_DAC0 50u
#define E1M_GPIO_DAC1 51u

/** Total number of GPIO-capable indices in this header.
 *  Boards' `alp,pin-array` arrays must list this many entries. */
#define E1M_GPIO_COUNT 52u

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* E1M_PINOUT_H */
