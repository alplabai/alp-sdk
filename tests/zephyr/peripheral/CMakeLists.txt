# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alp_sdk_peripheral_test LANGUAGES C)

# Per-peripheral split landed in §C.16.  main.c keeps the ZTEST_SUITE
# declaration plus the cross-cutting tests (TMU / power / AEN audit
# surfaces / delay / SoC caps / V2N supervisor / TRNG entropy); every
# *.c sibling here registers its ZTESTs against the same suite.
target_sources(app PRIVATE
    src/main.c
    src/i2c.c
    src/spi.c
    src/gpio.c
    src/uart.c
    src/pwm.c
    src/adc.c
    src/dac.c
    src/counter.c
    src/qenc.c
    src/i2s.c
    src/can.c
    src/rtc.c
    src/wdt.c
)
