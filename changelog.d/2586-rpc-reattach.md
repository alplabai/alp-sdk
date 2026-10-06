### Fixed — V2M/V2N UIO RPC: a second A55 attach in one CM33 boot no longer desyncs, and a timed-out call no longer shifts every later reply by one (#2586)

A second `alp_rpc_open()` in the same CM33 boot failed: the first call
timed out (`status=-4`), then every reply arrived one call late. Two
causes, fixed separately:

- **Attach reset.** The A55 backend (`src/backends/rpc/yocto_uio_drv.c`)
  re-creates its OpenAMP virtio driver on every open, which zeroes both
  vrings, while the CM33 (`examples/multicore/rpmsg-v2n/m33_sm`) kept its
  ring indices for its whole boot. The CM33 now publishes whether it is
  bound through the attach-epoch word at `rsctbl+0xFFC` (A55
  `0x4f700ffc`): odd = bound (bumped after `rpmsg_init_vdev()` returned),
  even = waiting (bumped after its virtio device is torn down). Whenever
  the A55 reads an odd epoch, whatever `vdev.status` says (a timed-out
  earlier open leaves it 0), it writes the resource table's
  `vdev.status = 0`, kicks the CM33, and waits up to 500 ms (monotonic
  clock) for the epoch to turn even. The CM33 manager task treats a
  doorbell without `DRIVER_OK` as a reset: it stops the responder, purges
  the echo queue, tears down its virtio device, bumps the epoch and waits
  for the next attach. `cleanup_system()` no longer writes `vdev.status`,
  which could overwrite a `DRIVER_OK` the A55 had already set again. The
  backend also takes `flock(LOCK_EX | LOCK_NB)` on the `rsctbl` UIO node:
  a second process opening while another holds the link gets
  `ALP_ERR_BUSY`. The CM33 beacon version at `rsctbl+0xFF4` goes from `1` to
  `2`. A CM33 still on version 1 cannot reset, so a second open in its
  boot now fails with `ALP_ERR_BUSY` and a message naming the reason. The
  same check fails the open with `ALP_ERR_NOT_READY` when the beacon
  magic `0xA10D0683` is missing (this includes a stock shim without the
  heartbeat beacon) and with `ALP_ERR_NOSUPPORT` when the version is
  `>= 0x100` (an image without RPC, `0x100` = the idle stock shim with
  the heartbeat beacon, which only exists on the unmerged branch
  `feat/cm33-shim-heartbeat`). The `alp_rpc_open()` error list in
  `include/alp/rpc.h` names these. The m33_sm `prj.conf` also sets
  `CONFIG_ALP_SDK=y` so a bare `west build` links the MHU-B glue. An old A55 binary never writes status 0, so its behaviour with
  the new CM33 firmware is unchanged. `cleanup_system()` in the CM33
  example also freed nothing (it passed `rvdev.vdev` after
  `rpmsg_deinit_vdev()` had NULLed it); fixed, since it now runs once per
  re-attach. Not yet bench-verified: keep cold-cycling between runs until
  it is.
- **Poisoned channel after a timeout.** Replies are matched to calls by
  method name only (no sequence id), so a call that timed out left its
  late reply to be returned to the next call. In all three RPC backends
  (`yocto_uio_drv.c`, `yocto_drv.c`, `zephyr_drv.c`) a call that gives up
  after its request went out now poisons the channel: every later
  `alp_rpc_call()` returns `ALP_ERR_NOT_READY` until the channel is
  closed and reopened, and the backend logs why. A reply that lands
  between the timeout and the wait returning is now returned instead of
  being dropped. `zephyr_drv.c` also treated only `-EAGAIN` from
  `k_sem_take()` as a timeout, so a `timeout_ms == 0` call (`-EBUSY`)
  left its call slot staged; any failure now takes the timeout path.
  `alp_rpc_send()` is unaffected.
