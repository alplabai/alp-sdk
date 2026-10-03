### Added — SoC die temperature on Linux: `alp_temperature_read_soc_milli_c()` (audit SYS-08)

`<alp/temperature.h>` gains a second entry that reports the processor's own
junction temperature in signed milli-degrees Celsius, kept separate from the
on-module ambient sensor (issue #2066). The Yocto backend
(`src/yocto/temperature_yocto.c`) reads `/sys/class/thermal/thermal_zoneN/temp`
for every zone whose `type` starts with `cpu-thermal` (the RZ/V2N's two TSU
units) and returns the hottest; zones are chosen by type, never by index.
Zephyr and baremetal builds return `ALP_ERR_NOSUPPORT`. New example
`examples/v2n/v2n-soc-temperature`. Not yet run on hardware.

### Fixed — V2N camera ISP `configure_isp` no longer reports success without writing hardware (audit MM-06)

`src/backends/camera/v2n_n44_isp.c` latched the ISP config and returned
`ALP_OK` although no register is written, and cited the wrong manual section.
It now returns `ALP_ERR_NOSUPPORT` and points at section 9.8 of the RZ/V2N
Hardware User's Manual (R01UH1071EJ0120); CRU/CSI-2/ISP are A55-owned.
