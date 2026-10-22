### Added — AEN STOP wake from the RV-3028 EVI pin (#2811)

`ALP_POWER_WAKE_EXT_EVENT` (0x00000100u) wakes the E8 from STOP on the RV-3028 external-event input, served through the existing INT to P15_0 path. The backend sets EIE for the cycle, reports EVF as its own wake source, and stops EVI state on the next sleep entry. `examples/aen/aen-power-stop` gains variant E (EVI cycle). Bench-pending: not yet run on silicon.
