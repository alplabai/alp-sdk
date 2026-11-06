### Fixed — make the heterogeneous-offload example run on V2N/V2M silicon (#2916)

The M33-SM image resolved `alp_rpc` to the NOSUPPORT stub (the generated
`alp.conf` carries no RPC, OpenAMP or MBOX Kconfig) and published no AMP
beacon, which `yocto_uio_drv.c` requires before it opens a channel. The M33
slice now copies the raw OpenAMP bring-up of `rpmsg-v2n` (resource table, MHU
doorbell, version-2 beacon with the attach-epoch handshake) and serves the
`fft` method itself on endpoint 1024; the A55 requester dials that endpoint.
The M33 testcase now builds for V2N101/V2M101 only; the native_sim build
coverage of this example is dropped (the slice no longer uses `<alp/rpc.h>`).
Needs the CMSIS-DSP library (build error otherwise). The generic Zephyr RPC
backend on V2N is tracked in #3067.
