### Fixed

- `e1m-v2n-som.dtsi` reserves the A55 view of the CM33 OpenAMP window (`0x4f700000`, 9 MiB) as `no-map`. Before, that range was ordinary Linux System RAM, so a CM33 image with IPC enabled would write its resource table and vrings into memory Linux had handed out (#2374). The UIO nodes the A55 RPC backend opens are still not declared; their receive IRQ needs a bench round-trip.
