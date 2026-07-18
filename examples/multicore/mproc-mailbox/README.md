# mproc-mailbox

Per-peripheral example for `<alp/mproc.h>`.  Demonstrates the
M55-HP side of a Cortex-M55-HP ↔ Cortex-M55-HE mailbox roundtrip
on AEN: stage a payload in shared SRAM, signal the peer via the
hardware mailbox, wait for a reply, read the result back.

## What this shows

- Opening a shared-memory region (`alp_shmem_open`) and a
  hardware mailbox (`alp_mbox_open`) by portable instance IDs.
- Resolving a raw pointer view of the shared region with
  `alp_shmem_view()`, then staging payload bytes by `memcpy`
  through that pointer (the surface hands back a base pointer +
  size and trusts the caller to write through it; the backend
  handles cache-coherency for `cacheable = false` regions).
- Signalling the peer with `alp_mbox_send` carrying a small
  tuple (offset + length) that points at the staged bytes.
- Receiving the reply through an inbound callback registered with
  `alp_mbox_set_callback()` -- it fires on the SDK's mbox thread
  with the peer's (offset, length) tuple, which `main()` then
  drains.
- Reading the peer's echo response back from shared SRAM via the
  same pointer view.

## Build

### native_sim (no peer core; HP-side init only)

```bash
west build -b native_sim/native/64 examples/multicore/mproc-mailbox
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
tan build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_he examples/multicore/mproc-mailbox
west flash
```

The peer-side firmware lives at
[`examples/multicore/mproc-mailbox/peer/main.c`](peer/main.c) -- HE-side
image that waits on the same mbox, reads the staged shmem
payload, and writes back an echo via reverse send.

Until the v0.4 dual-image build flow in
`alplabai/alp-zephyr-modules` lands, the two halves build
separately:

```bash
# HP side -- builds + runs the application.
tan build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_hp examples/multicore/mproc-mailbox

# HE side -- builds the peer image manually.  Sysbuild picks
# this up automatically once the v0.4 dual-image flow ships.
west build -b ensemble_e8_dk/ae402fa0e5597le0/rtss_he examples/multicore/mproc-mailbox/peer
```

Flash both into the matching SoC partitions (HP -> RTSS-HP slot,
HE -> RTSS-HE slot) and the roundtrip completes:

```
[mproc] init mbox + shmem
[mproc] sending payload  "hello-from-HP" (13 bytes)
[mproc-peer] request offset=0 len=13
[mproc-peer] payload  "hello-from-HP"
[mproc-peer] replied "echo: hello-from-HP" (19 bytes)
[mproc] HE replied via mbox callback
[mproc] HE replied       "echo: hello-from-HP" (19 bytes)
[mproc] done
```

## Reference

- [`<alp/mproc.h>`](../../../include/alp/mproc.h) -- mailbox + shmem
  + hwsem API.
- [`docs/v1.0-readiness.md`](../../../docs/v1.0-readiness.md) §4 --
  this example is one of the two v1.0 reference-app flagships
  still ahead (the peer-firmware half is what's left).
