### Fixed — Yocto CAN `ext_id=false` filters no longer match 29-bit frames (#2457)

On the Linux backend an `alp_can_filter_t` with `ext_id = false` built a kernel
filter whose mask lacked `CAN_EFF_FLAG`, so a 29-bit frame whose low 11 bits
matched the filter id was delivered to the 11-bit filter. The mask now always
carries `CAN_EFF_FLAG` (`src/backends/can/yocto_drv.c`), matching
`include/alp/can.h` and the Zephyr backend. Covered by
`tests/yocto/can_yocto_filter_ext_id.c`.
