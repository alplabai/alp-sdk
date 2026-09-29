### Fixed

- `<alp/can.h>` on V2N-family Linux opened the wrong port: the Yocto backend bound `can<bus_id>`, but `rcar_canfd` names netdevs in channel probe order, so `can0` is SoC channel 2 (E1M_X_CAN1, U16) and `can1` is channel 3 (E1M_X_CAN0, U15). A kernel patch now reports the channel in `dev_port`, the new `alp-canfd-udev` rule renames the netdevs to `can_e1m0` / `can_e1m1` by channel, and the backend opens `can_e1m<N>` first, falling back to `can<N>` where no such netdev exists (hosts, vcan, other SoMs).
