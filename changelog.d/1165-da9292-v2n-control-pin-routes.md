### Fixed — DA9292 control-pin routes on the V2N module recorded from the module schematic (#1165)

`metadata/chips/da9292.yaml` carried `v2n_route: TBD` for six control pins. They now
record the sanitised route: `PB_N` pulled up to the 1.8 V rail (not routed to the host),
`EN1` strapped to the 1.8 V rail (CH1 always enabled), `EN2` driven by the host
`DEEPX_CORE_0P75_EN` line (P64) through a series resistor, `CE` strapped to the 5 V input
rail through a 0 ohm link, and `VSEL1`/`VSEL2` strapped to ground (the `_LO` VOUT register
set). The U-Boot DEEPX rail patch comments that called the `VSEL2` routing undocumented
now match.
