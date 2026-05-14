# mproc-mailbox

Per-peripheral example for `<alp/mproc.h>`.  Demonstrates the
M55-HP side of a Cortex-M55-HP ↔ Cortex-M55-HE mailbox roundtrip
on AEN: stage a payload in shared SRAM, signal the peer via the
hardware mailbox, wait for a reply, read the result back.

## What this shows

- Opening a shared-memory region (`alp_shmem_open`) and a
  hardware mailbox (`alp_mbox_open`) by portable instance IDs.
- Staging payload bytes into the shared region via
  `alp_shmem_write_at` -- the backend handles cache-coherency
  on its way out.
- Signalling the peer with `alp_mbox_send` carrying a small
  tuple (offset + length) that points at the staged bytes.
- Receiving a reply with `alp_mbox_recv` blocking up to a
  caller-supplied timeout.
- Reading the peer's echo response back from shared SRAM.

## Build

### native_sim (no peer core; HP-side init only)

```bash
west build -b native_sim/native/64 examples/mproc-mailbox
west build -t run
```

Expected output:

```
[mproc] init mbox + shmem
[mproc] sending payload  "hello-from-HP" (13 bytes)
[mproc]   native_sim: no peer core; skipping reply
[mproc] done
```

### Real silicon (AEN dual-core, requires the peer firmware)

```bash
west alp-build -b alif_e7_dk_rtss_he examples/mproc-mailbox
west flash
```

The peer-side firmware (`examples/mproc-mailbox/peer/`) lands
alongside the v0.4 dual-image build flow in
`alplabai/alp-zephyr-modules`.  Until that lands, real-silicon
runs will time out at `alp_mbox_recv` and the example reports
"reply timeout (peer not running?)".

## Reference

- [`<alp/mproc.h>`](../../include/alp/mproc.h) -- mailbox + shmem
  + hwsem API.
- [`docs/v1.0-readiness.md`](../../docs/v1.0-readiness.md) §4 --
  this example is one of the two v1.0 reference-app flagships
  still ahead (the peer-firmware half is what's left).
