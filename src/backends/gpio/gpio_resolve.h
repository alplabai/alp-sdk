/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private cross-backend seam: alp_z_gpio_resolve() resolves a portable
 * pin_id to a Zephyr gpio_dt_spec via the alp,pin-array DT node.
 *
 * Defined (strong) in src/backends/gpio/zephyr_drv.c; src/backends/spi/
 * zephyr_drv.c carries a `__attribute__((weak))` fallback so the SPI
 * backend still links when CONFIG_GPIO=n (no GPIO backend compiled in --
 * e.g. the gd32-bridge example, which uses SPI without a CS pin).  Both
 * definitions include this header instead of relying on the implicit
 * (K&R-style) declaration-free definition -Wmissing-prototypes flags
 * (issue #634).  NOT a public header.
 */

#ifndef ALP_BACKENDS_GPIO_RESOLVE_H
#define ALP_BACKENDS_GPIO_RESOLVE_H

#include <stdbool.h>
#include <stdint.h>

struct gpio_dt_spec;

bool alp_z_gpio_resolve(uint32_t pin_id, struct gpio_dt_spec *out);

#endif /* ALP_BACKENDS_GPIO_RESOLVE_H */
