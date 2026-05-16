# Tutorial 15: Multi-proc mailbox (M55-HP ↔ M55-HE)

**Target audience:** developers building AEN firmware that
needs to offload work from the application core (M55-HP) onto
the high-efficiency core (M55-HE) -- audio DSP, sensor polling,
ML pre-processing.

**Prerequisites:**

- Tutorial [01](01-first-build.md) completed.
- An AEN E7 EVK (dual M55) or any AEN SoM with both cores.

**Outcome:** a working HP↔HE mailbox roundtrip.  HP stages a
payload in shared SRAM, signals HE via a hardware mailbox, HE
echoes the payload back with a prefix.  Understand the SDK's
`<alp/mproc.h>` surface (mailbox + shmem + hwsem) + the
v0.4-prep nanopb framing.

**Time:** 30 minutes (after the EVK is on the bench and the
peer firmware is built).

> Companion example:
> [`examples/mproc-mailbox/`](../../examples/mproc-mailbox/) --
> the HP-side firmware this tutorial walks through.  The peer-
> side (HE) firmware lands alongside the v0.4 dual-image build
> flow in `alplabai/alp-zephyr-modules`.

---

## Why split work across cores?

AEN E7 ships two Cortex-M55 cores:

- **M55-HP** ("high performance") -- runs at 400 MHz, has the
  Ethos-U55 NPU + ITCM + cache.  Hosts the application:
  Wi-Fi, MQTT, UI, primary control loop.
- **M55-HE** ("high efficiency") -- runs at 160 MHz, no NPU,
  lower static power.  Designed for offloadable always-on
  tasks: PDM/audio DSP, slow sensor polling, inference pre/
  post-processing.

Splitting work across cores buys:

- **Lower power**: HE runs the low-rate sensor loop while HP
  sleeps between Wi-Fi packets.
- **Latency isolation**: critical real-time work (audio DSP,
  CAN ISR) runs on HE without HP's MQTT publish blocking it.
- **Throughput**: parallel inference where the model has a
  pre-process step that's CPU-bound and a model step that's
  NPU-bound.

The shape that makes this work: a hardware mailbox + a region
of cache-coherent shared SRAM + a hardware semaphore.

## 1. The three primitives

### Mailbox (`alp_mbox_*`)

A hardware FIFO that delivers a small fixed-size message from
one core to another with a wake-up IRQ.  AEN E7 has two
mailboxes (MHU0 + MHU1); the SDK opens MBOX0 for the HP↔HE
control channel.

```c
alp_mbox_t *mbox = alp_mbox_open(0u);     // MBOX0
alp_mbox_send(mbox, payload, payload_len);  // wakes peer
size_t got;
alp_mbox_recv(mbox, reply, sizeof(reply), &got, /*timeout_ms*/ 1000);
```

Payload size: 32 bytes per message (AEN MHU limit).  For
larger transfers, pass an offset+length tuple via the
mailbox + put the actual bytes in shared SRAM.

### Shared memory (`alp_shmem_*`)

A region of cache-coherent SRAM both cores can read/write.
Cache flush + invalidate is the backend's responsibility
inside `alp_shmem_write_at` / `_read_at` -- the caller doesn't
issue DSB / barrier instructions.

```c
alp_shmem_t *shmem = alp_shmem_open(0u);   // region 0
alp_shmem_write_at(shmem, /*offset*/ 0u, payload, payload_len);
alp_shmem_read_at(shmem, /*offset*/ 0u, buffer, buf_len);
```

Region size + layout is configured in devicetree; default
slot for the HP↔HE shared region is 4 KiB at
`0x60000000` (the AEN E7 SRAM2 reserved range).

### Hardware semaphore (`alp_hwsem_*`)

A multi-core mutual-exclusion primitive backed by the AEN
HWSEM peripheral.  Used when both cores write the same shared
region concurrently.

```c
alp_hwsem_t *sem = alp_hwsem_open(0u);
alp_hwsem_take(sem, /*timeout_ms*/ 100);
// critical section: modify shared structure
alp_hwsem_give(sem);
```

## 2. The HP-side application

From `examples/mproc-mailbox/src/main.c`:

```c
alp_shmem_t *shmem = alp_shmem_open(0u);
alp_mbox_t  *mbox  = alp_mbox_open(0u);

const char *payload = "hello-from-HP";
const size_t plen   = strlen(payload);

/* 1. Stage in shmem. */
alp_shmem_write_at(shmem, 0u, payload, plen);

/* 2. Notify peer with the (offset, length) tuple. */
uint8_t mbox_msg[8] = {
    0,0,0,0,            // offset = 0 (LE u32)
    plen,0,0,0,         // length = plen
};
alp_mbox_send(mbox, mbox_msg, sizeof(mbox_msg));

/* 3. Wait for the reply tuple. */
uint8_t reply[8];
size_t  reply_len;
alp_mbox_recv(mbox, reply, sizeof(reply), &reply_len, 1000);

/* 4. Decode the reply tuple + read the echoed data. */
uint32_t echo_off = u32_le(reply);
uint32_t echo_len = u32_le(reply + 4);
char echo_buf[64];
alp_shmem_read_at(shmem, echo_off, echo_buf, echo_len);
echo_buf[echo_len] = '\0';
printf("HE echoed: %s\n", echo_buf);
```

## 3. The HE-side peer (sketch -- v0.4 dual-image)

The peer firmware lives at `examples/mproc-mailbox/peer/src/main.c`
(TBD until the dual-image build flow lands).  Pattern:

```c
alp_shmem_t *shmem = alp_shmem_open(0u);
alp_mbox_t  *mbox  = alp_mbox_open(0u);

while (1) {
    uint8_t msg[8];
    size_t  got;
    if (alp_mbox_recv(mbox, msg, sizeof(msg), &got, K_FOREVER) != ALP_OK)
        continue;

    /* Read incoming data. */
    uint32_t in_off = u32_le(msg);
    uint32_t in_len = u32_le(msg + 4);
    char buf[64];
    alp_shmem_read_at(shmem, in_off, buf, in_len);

    /* Build the echo: "echo: " + payload. */
    char out[64];
    int n = snprintf(out, sizeof(out), "echo: %.*s", (int)in_len, buf);

    /* Write back at offset 64 to avoid colliding with HP's data. */
    alp_shmem_write_at(shmem, 64u, out, n);

    /* Reply with the (offset, length) tuple. */
    uint8_t reply[8] = {
        64, 0, 0, 0,
        n,  0, 0, 0,
    };
    alp_mbox_send(mbox, reply, sizeof(reply));
}
```

## 4. v0.4 nanopb framing (when ready)

Pre-v0.4, the offset+length tuple is hand-encoded as 8 raw
bytes.  v0.4-prep merged a nanopb-based framing helper at
`src/common/proto/alp_mproc_frame.c` that wraps every mbox
payload in a 12-byte envelope (magic / sequence / length).  Opt
in via:

```yaml
# board.yaml -- v2 shape: ipc: is a top-level array of carve-outs
# the orchestrator allocates from the SoM preset's memory_map.
# The nanopb framing rides per-app via an extra Kconfig overlay
# (CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING=y) emitted by the loader
# when `features.ipc.framing: nanopb` is set.
ipc:
  - kind: raw_shmem
    endpoints: [m55_hp, m55_he]
    carve_out_kb: 64
    name: alp_mproc_he_offload

features:
  ipc:
    framing: nanopb              # v0.4-prep; placeholder framing
                                  # rides automatically when this is unset
```

Once the v0.4 `extras-v04` group lands the upstream MaJerle/lwrb
+ nanopb/nanopb packs, the framing flips from "placeholder
hand-rolled" to "nanopb-generated codec".  The application code
above doesn't change; the framing is transparent inside
`alp_mbox_send`.

## 5. Build + flash

```bash
# HP-side:
west alp-build -b alif_e7_dk_rtss_he examples/mproc-mailbox
# Outputs build/zephyr/zephyr.bin for HP.

# HE-side (TBD, dual-image flow):
# west alp-build -b alif_e7_dk_rtss_he examples/mproc-mailbox \
#     --sysbuild --domain m55-he
# Outputs build/m55-he/zephyr/zephyr.bin for HE.

# Flash HP to primary slot; HE flashes via sysbuild to its own
# slot.
west flash
```

Expected output on the UART:

```
[mproc] init mbox + shmem
[mproc] sending payload  "hello-from-HP" (13 bytes)
[mproc] HE woke up, payload visible in shmem
[mproc] HE replied       "echo: hello-from-HP" (19 bytes)
[mproc] HP read reply OK
[mproc] done
```

## 6. native_sim behaviour

native_sim has no peer core; the HP-side example exercises
just the init + framing-encode path then exits.  Output:

```
[mproc] init mbox + shmem
[mproc] sending payload  "hello-from-HP" (13 bytes)
[mproc]   native_sim: no peer core; skipping reply
[mproc] done
```

CI's Twister scenario asserts on the `[mproc] done` line, so
the framing-encode path is regression-tested without real
silicon.

## See also

- [`<alp/mproc.h>`](../../include/alp/mproc.h) -- the public API.
- [`examples/mproc-mailbox/`](../../examples/mproc-mailbox/) --
  the reference app (HP-side; HE-side TBD with v0.4 dual-
  image).
- [`tests/fuzz/mproc_frame_fuzz.c`](../../tests/fuzz/mproc_frame_fuzz.c)
  -- fuzz harness for the envelope framing.
- [`src/common/proto/alp_mproc_frame.h`](../../src/common/proto/alp_mproc_frame.h)
  -- the framing implementation.
