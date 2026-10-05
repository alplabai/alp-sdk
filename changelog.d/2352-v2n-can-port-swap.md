### Fixed — V2N Linux: `<alp/can.h>` opened the swapped CAN port (#2352)

On the RZ/V2N-family E1M-X SoM (E1M-V2N101/102/103, E1M-V2M101/102/103),
`alp_can_open(E1M_X_CAN0)` drove the CAN1 pins and vice versa.
`src/backends/can/yocto_drv.c`'s `y_open()` (pre-fix `snprintf` call site)
previously resolved the SocketCAN netdev straight from the literal `bus_id`
(`can<bus_id>`), but `rcar_canfd` names netdevs in channel PROBE order and
the E1M-X pin routing (`metadata/pinmux/v2n.yaml`'s CAN0H/L, CAN1H/L rows)
puts `E1M_X_CAN0` on CANFD channel 3 (U15, probes second -> `can1`) and
`E1M_X_CAN1` on CANFD channel 2 (U16, probes first -> `can0`) — fixed by
this SDK's own kernel DT overlay
(`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`, #2332).

New `src/backends/can/yocto_drv.c:407` (`_can_netdev_index`) swaps portable bus ids 0/1
(`E1M_X_CAN0`/`E1M_X_CAN1`) before building the netdev name, so
`alp_can_open(E1M_X_CAN0)` now opens `can1` (the netdev that actually
drives the CAN0 pins) and `E1M_X_CAN1` opens `can0`. Unconditional rather
than silicon-gated: `CONFIG_ALP_SOC_*` is a Zephyr/M33 Kconfig symbol never
defined for this Linux/A55 build, and every Linux CAN target today is this
same swapped family. Any other `bus_id` (raw netdev access, not through the
portable E1M enum) passes through unchanged.

Regression-covered by `tests/yocto/can_yocto_netdev_swap.c` (asserts
`_can_netdev_index(0)==1`, `(1)==0`, and pass-through for other bus ids).

Found while addressing the #2341 review; refs #2332.
