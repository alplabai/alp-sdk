### Added — SoC die temperature on Linux: `alp_temperature_read_soc_milli_c()` (audit SYS-08) (#2660)

`<alp/temperature.h>` gains a second entry that reports the processor's own
junction temperature in signed milli-degrees Celsius, kept separate from the
on-module ambient sensor (issue #2066). The Yocto backend
(`src/yocto/temperature_yocto.c`) reads `/sys/class/thermal/thermal_zoneN/temp`
for every zone whose `type` starts with `cpu-thermal` (the RZ/V2N's two TSU
units) and returns the hottest; zones are chosen by type, never by index.
Zephyr and baremetal builds return `ALP_ERR_NOSUPPORT`. New example
`examples/v2n/v2n-soc-temperature`. Not yet run on hardware.
