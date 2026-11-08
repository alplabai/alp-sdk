### Changed — `mproc-mailbox` example builds with real shmem and mailbox aliases on E1M-AEN801/803 (Refs #2971)

Build-verified on AEN803 (M55-HP + M55-HE) and under native_sim ztest with a mock controller; **not bench-verified**, so treat the AEN mailbox round-trip as untested on silicon.

- `metadata/e1m_modules/E1M-AEN801.yaml`, `E1M-AEN803.yaml`: new `sram_ipc` memory_map row (64 KiB of global SRAM0 at `0x023E0000`, no in-tree user), so a `raw_shmem` `ipc:` entry resolves (`0x023ef000`, 4 KiB) instead of `status: blocked`.
- `examples/multicore/mproc-mailbox`: per-core AEN overlays and `.conf` files define `alp-shmem0`, `alp-mbox1` (MHUv2 sender frame) and `alp-mbox-rx1` (receiver frame) and set `CONFIG_MBOX`/`CONFIG_MBOX_ALIF_MHUV2`, so `CONFIG_ALP_SDK_MPROC=y` takes effect. Both `main.c` use mailbox channel 1, matching `ALP_IPC_ALP_SHMEM0_MBOX_CH`.
- `src/backends/mproc/zephyr_drv.c`: optional `alp-mbox-rxN` alias for one-directional mailbox frames; for a doorbell-only controller (`mbox_mtu_get()` == 0, as the Alif MHUv2) the message rides in a 64-byte slot at the end of `alp-shmem0` and a payload-less doorbell is rung (max 60 bytes, else `ALP_ERR_OUT_OF_RANGE`); controllers with an `mtu` refuse longer messages with `ALP_ERR_OUT_OF_RANGE` instead of passing them on.
- `tests/zephyr/mproc`: new `alp_sdk.mproc.doorbell` scenario (mock controllers) for the doorbell slot, the oversize refusal and the receive alias with its fallback.
