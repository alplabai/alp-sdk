### Added — `alp_uart_open()` reaches Renesas SCIF `/dev/ttySC<N>` on Linux (#2458)

The Yocto UART backend only mapped `port_id` to `ttyS`, `ttyAMA` and `ttyUSB`,
so none of the RZ/V2N on-module SCIF ports were reachable. `port_id` 300..399
now opens `/dev/ttySC<port_id - 300>`. The `ttyUSB` range narrows to 200..299
and `port_id >= 400` returns `ALP_ERR_INVAL`. See `docs/soms/v2n.md`.
