### Fixed — ACT88760 Buck5 / Buck6 rail attribution was swapped in the V2N power tree (#1165)

`metadata/e1m_modules/v2n/power-tree.yaml` had `vdd_core_0p75` on
`buck5` and `vdd08_ddr` on `buck6`, both marked TBD-verify and both
inferred only from CMI sequencing timing. Checked 2026-09-25 against the
module design data and a live register read on an E1M-V2M103 (ACT88760
at `0x25` on BRD_I2C): tile `0xC0` (Buck5) reads VSET `0x0C` = 800 mV,
and tile `0xE0` (Buck6) reads VSET `0x0A` = 750 mV. So `VDD08_DDR` is
Buck5 and `VDD_CORE_0P75` is Buck6. The CMI report numbers these two the
other way round, which is how the earlier attribution went wrong.

This was a runtime bug, not only a label: the generated guard table in
`include/alp/chips/v2n_power_tree.h` gave Buck5 a 725–775 mV window and
Buck6 a 775–825 mV window. Both healthy rails therefore read out of
window. The two channels are swapped back, both nets are now named in the
table instead of `TBD`, and the GPIO7 / GPIO10 role text now names Buck6.

Buck6's tile `0xE4` reads `0x3E`: ON bit clear, AUXIN_EN set. The rail
is held up by the PWEN2 sequencer trigger, not by its ON bit, so
`act8760_rail_set_enable()` (which only writes the ON bit) cannot switch
it. That matches `enable_writable: false` for this rail.
