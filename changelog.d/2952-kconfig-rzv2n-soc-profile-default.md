### Fixed — kconfig: the SoC capability profile defaults to `ALP_SOC_RENESAS_RZV2N_N44` on `SOC_SERIES_RZV2N` (#2952)

The "Active SoC capability profile" choice had defaults for the E8 and E4 series only, so a plain `west build` for a V2N/V2M CM33 board resolved to `ALP_SOC_NONE` and quietly dropped the GD32-routed pwm/adc backends, the V2N supervisor and the external power backend. The choice now defaults to `ALP_SOC_RENESAS_RZV2N_N44` for `SOC_SERIES_RZV2N`.
