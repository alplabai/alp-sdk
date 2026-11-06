### Fixed — `mproc-mailbox` example now builds as a real dual-core mailbox on E1M-AEN801/803 (Refs #2971)

- `metadata/e1m_modules/E1M-AEN801.yaml`, `E1M-AEN803.yaml`: new `sram_ipc` memory_map row (64 KiB of global SRAM0 at `0x02010000`, reachable from both M55 cores), so a `raw_shmem` `ipc:` entry no longer resolves `status: blocked`; it now allocates `0x0201f000`.
- `examples/multicore/mproc-mailbox`: per-core AEN overlays and `.conf` files define `alp-shmem0`, `alp-mbox0` (MHUv2 sender frame) and `alp-mbox-rx0` (receiver frame) and set `CONFIG_MBOX`/`CONFIG_MBOX_ALIF_MHUV2`, so `CONFIG_ALP_SDK_MPROC=y` takes effect. Build-verified on AEN803 HP + HE; not bench-verified.
- `src/backends/mproc/zephyr_drv.c`: optional `alp-mbox-rxN` alias for boards whose mailbox frames are one-directional; send uses `alp-mboxN`, the receive callback uses `alp-mbox-rxN` (falls back to the same device).
