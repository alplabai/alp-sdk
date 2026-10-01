### Added — bench path for the A55 <-> CM33 RPC round trip over UIO (#2374)

`examples/multicore/rpmsg-v2n/m33_sm` now builds for `alp_e1m_v2m101_m33_sm` as well as V2N101. A new HIL spec, `tests/hil/v2m103-x-evk/v2m103-rpmsg-echo-uio.yaml`, runs the `linux/` consumer and expects four verified `echo_test` round trips through `yocto_uio_drv.c`. The README gains the build, mtd1 flash and run steps.
