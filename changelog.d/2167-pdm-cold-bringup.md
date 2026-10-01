### Fixed — Alif PDM refused the first START with `-EIO` after a warm boot (#2167)

`alif_pdm` inherited the previous image's `PDM_INTERRUPT` enables (`0x303`) and
sticky `PDM_ERROR_IRQ`/`PDM_WARN_IRQ` status across a Flow C or warm boot, and
`irq_config()` armed the NVIC lines on top of them, so `pdm_error_handler()` set
`overrun` and `DMIC_TRIGGER_START` failed. Cold init now masks `PDM_INTERRUPT`,
read-clears both status registers, pulses `FIFO_CLR` and clears `overrun`
before `irq_config()`. The DesignWare I2S driver already quiesces IER/IMR
before arming its NVIC line (#2179, #2205), so it needed no change.
