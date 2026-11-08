/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SPI controller/peripheral naming that builds on Zephyr v4.4 and v4.5.
 *
 * v4.5 renames SPI_OP_MODE_MASTER/SLAVE to SPI_OP_MODE_CONTROLLER/PERIPHERAL,
 * spi_config.slave to .peripheral and spi_context_is_slave() to
 * spi_context_is_peripheral(), and deprecates the old spellings (a -Werror
 * build fails on them).  v4.4 has only the old names, so map the new ones
 * onto them there.
 */
#ifndef ALP_ZEPHYR_DRIVERS_SPI_ALP_SPI_COMPAT_H_
#define ALP_ZEPHYR_DRIVERS_SPI_ALP_SPI_COMPAT_H_

#include <zephyr/drivers/spi.h>

#ifdef SPI_OP_MODE_CONTROLLER
#define ALP_SPI_CFG_PERIPHERAL_NUM(cfg) ((cfg)->peripheral)
#else
#define SPI_OP_MODE_CONTROLLER          SPI_OP_MODE_MASTER
#define SPI_OP_MODE_PERIPHERAL          SPI_OP_MODE_SLAVE
#define ALP_SPI_CFG_PERIPHERAL_NUM(cfg) ((cfg)->slave)
#endif

/* Operation-word test shared by spi_context_is_slave() / _is_peripheral(). */
#define ALP_SPI_CTX_IS_PERIPHERAL(ctx) (((ctx)->config->operation & SPI_OP_MODE_PERIPHERAL) != 0U)

#endif /* ALP_ZEPHYR_DRIVERS_SPI_ALP_SPI_COMPAT_H_ */
