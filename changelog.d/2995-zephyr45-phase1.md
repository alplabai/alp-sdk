### Changed — SPI controller/peripheral names build on Zephyr 4.4 and 4.5 (#2995)

Zephyr 4.5 renames `SPI_OP_MODE_MASTER`/`SLAVE` to `SPI_OP_MODE_CONTROLLER`/`PERIPHERAL`, `spi_config.slave` to `.peripheral` and `spi_context_is_slave()` to `spi_context_is_peripheral()`, and deprecates the old names, which fails a `-Werror` build. The SPI backend and the DW-SPI (Alif) and SCI-B (RZ) drivers now use the new names through `zephyr/include/zephyr/drivers/spi/alp_spi_compat.h`, which maps them onto the old spellings on v4.4.1.
