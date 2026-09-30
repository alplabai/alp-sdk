### Fixed — V2N on-module CAN-FD rows carry a real E1M-X pad crosswalk (#2331)

`metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`'s four `CANFD2_*`/
`CANFD3_*` rows (the on-module TCAN1044 CAN-FD transceivers, #2332) still
carried `e1m_pad`/`e1m_function: "TBD"` in `metadata/pinmux/v2n.yaml` after
#2405 filled the other 40 directly-routed V2N pads — the TSV's `pad_first`
shape supports the optional `e1m_pad`/`e1m_function` columns #2405 added,
but those four rows were left without them.

Filled from #2332's schematic-sourced transceiver-to-channel mapping
(CANFD3/U15 on P86/P87 is carrier CAN0, CANFD2/U16 on P84/P85 is carrier
CAN1) plus the public e1m-spec E1M-X pinout (`pinout/x-v1.json`, ball ids
`B21`/`B22`/`B24`/`B25`) — no private netlist data was needed or copied
into this repo. `metadata/pinmux/v2n.yaml` regenerated via
`gen_pinmux_capability.py`; route coverage (`check_e1m_route_capability.py`)
is unchanged at 33/48.
