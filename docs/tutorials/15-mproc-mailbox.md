<!-- Last verified: 2026-05-18 against slice-3b state. -->

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
> [`examples/multicore/mproc-mailbox/`](../../examples/multicore/mproc-mailbox/) --
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

A hardware doorbell that signals one core from another with a
wake-up IRQ.  On AEN the IP is the **ARM MHUv2**; alp-sdk drives
it with an in-tree Zephyr MBOX-class driver under the compatible
`alif,mhuv2-mbox`
([`zephyr/drivers/mbox/mbox_alif_mhuv2.c`](../../zephyr/drivers/mbox/mbox_alif_mhuv2.c)).
AEN exposes two MHU pairs (MHU0 + MHU1); each frame carries **32
doorbell bits** (channel-window 0), and the SDK opens MBOX
channel 0 (doorbell bit 0) for the HP↔HE control channel.

The MHUv2 runs in **doorbell (signalling) mode** -- the MHU only
rings a per-bit doorbell, it carries no payload; the actual bytes
ride shared SRAM via the OpenAMP static-vrings (so `mbox_mtu_get`
is 0 and `alp_mbox_send` passes the data through shmem, not the
MHU).  Sends are explicit; **receives arrive via a registered
callback** -- there is no blocking `*_recv` call.

> The AEN MHUv2 driver is **vendor-ext** and bench-validated for the
> dual-M55 RPMsg path on E1M-AEN801 (#225, 2026-06-19).  A32<->M55
> Linux remoteproc overlays are endpoint-specific.

```c
alp_mbox_t *mbox = alp_mbox_open(&(alp_mbox_config_t){
    .channel = 0u,                 // MBOX channel 0
    .peer    = ALP_CORE_M55_HE,    // counterpart core
});
alp_mbox_set_callback(mbox, on_peer_msg, NULL);  // inbound -> on_peer_msg()
alp_mbox_send(mbox, payload, payload_len, /*timeout_ms*/ 1000);  // wakes peer
```

Payload size: 32 bytes per message (AEN MHU limit).  For
larger transfers, pass an offset+length tuple via the
mailbox + put the actual bytes in shared SRAM.

### Shared memory (`alp_shmem_*`)

A region of cache-coherent SRAM both cores can read/write.
@ref alp_shmem_view hands back the mapped base pointer + size;
the caller reads and writes through that pointer directly
(`memcpy`, struct stores).  Opening the region non-cacheable
(`.cacheable = false`) lets the backend keep the two cores
coherent without the caller issuing DSB / barrier instructions.

```c
alp_shmem_t *shmem = alp_shmem_open(&(alp_shmem_config_t){
    .name      = "alp_shmem0",     // DT-anchored region label
    .size      = 4096u,
    .cacheable = false,
});
void  *base = NULL;
size_t size = 0u;
alp_shmem_view(shmem, &base, &size);       // map it once
memcpy(base, payload, payload_len);        // core A writes
/* ... peer reads the same bytes through its own view ... */
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
alp_hwsem_lock(sem, /*timeout_ms*/ 100);   // or alp_hwsem_try_lock(sem)
// critical section: modify shared structure
alp_hwsem_unlock(sem);
```

## 2. The HP-side application

From `examples/multicore/mproc-mailbox/src/main.c`:

The reply arrives asynchronously, so HP registers a callback
before it sends.  The callback decodes the peer's (offset,
length) tuple and reads the echoed bytes straight out of the
shared region's mapped base pointer.

```c
static uint8_t *shmem_base;        /* set once from alp_shmem_view() */

/* Inbound-mailbox callback: HE has replied. */
static void on_peer_msg(uint32_t channel, const void *data, size_t len,
                        void *user) {
    (void)channel; (void)len; (void)user;
    const uint8_t *reply = data;
    uint32_t echo_off = u32_le(reply);
    uint32_t echo_len = u32_le(reply + 4);

    char echo_buf[64];
    memcpy(echo_buf, shmem_base + echo_off, echo_len);  /* read echoed data */
    echo_buf[echo_len] = '\0';
    printf("HE echoed: %s\n", echo_buf);
}

int main(void) {
    alp_shmem_t *shmem = alp_shmem_open(&(alp_shmem_config_t){
        .name = "alp_shmem0", .size = 4096u, .cacheable = false,
    });
    alp_mbox_t  *mbox  = alp_mbox_open(&(alp_mbox_config_t){
        .channel = 0u, .peer = ALP_CORE_M55_HE,
    });

    size_t shmem_size = 0u;
    alp_shmem_view(shmem, (void **)&shmem_base, &shmem_size);
    alp_mbox_set_callback(mbox, on_peer_msg, NULL);

    const char *payload = "hello-from-HP";
    const size_t plen   = strlen(payload);

    /* 1. Stage in shmem (write through the mapped base pointer). */
    memcpy(shmem_base + 0u, payload, plen);

    /* 2. Notify peer with the (offset, length) tuple. */
    uint8_t mbox_msg[8] = {
        0,0,0,0,            // offset = 0 (LE u32)
        plen,0,0,0,         // length = plen
    };
    alp_mbox_send(mbox, mbox_msg, sizeof(mbox_msg), /*timeout_ms*/ 1000);

    /* 3. on_peer_msg() fires from the mailbox thread when HE replies. */
    for (;;) { k_msleep(100); }
}
```

## 3. The HE-side peer (sketch -- v0.4 dual-image)

The peer firmware lives at `examples/multicore/mproc-mailbox/peer/src/main.c`
(TBD until the dual-image build flow lands).  Pattern:

The peer is symmetric: it opens the same region + channel and
does its echo work from the inbound-mailbox callback.

```c
static uint8_t *shmem_base;
static alp_mbox_t *mbox;

/* Inbound: HP has staged a payload + sent its (offset, length) tuple. */
static void on_hp_msg(uint32_t channel, const void *data, size_t len,
                      void *user) {
    (void)channel; (void)len; (void)user;
    const uint8_t *msg = data;
    uint32_t in_off = u32_le(msg);
    uint32_t in_len = u32_le(msg + 4);

    /* Read incoming data through the mapped base pointer. */
    char buf[64];
    memcpy(buf, shmem_base + in_off, in_len);

    /* Build the echo: "echo: " + payload. */
    char out[64];
    int n = snprintf(out, sizeof(out), "echo: %.*s", (int)in_len, buf);

    /* Write back at offset 64 to avoid colliding with HP's data. */
    memcpy(shmem_base + 64u, out, n);

    /* Reply with the (offset, length) tuple. */
    uint8_t reply[8] = {
        64, 0, 0, 0,
        n,  0, 0, 0,
    };
    alp_mbox_send(mbox, reply, sizeof(reply), /*timeout_ms*/ 1000);
}

int main(void) {
    alp_shmem_t *shmem = alp_shmem_open(&(alp_shmem_config_t){
        .name = "alp_shmem0", .size = 4096u, .cacheable = false,
    });
    mbox = alp_mbox_open(&(alp_mbox_config_t){
        .channel = 0u, .peer = ALP_CORE_M55_HP,
    });

    size_t shmem_size = 0u;
    alp_shmem_view(shmem, (void **)&shmem_base, &shmem_size);
    alp_mbox_set_callback(mbox, on_hp_msg, NULL);

    for (;;) { k_msleep(100); }  /* echo work happens in on_hp_msg() */
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
ipc:
  - kind: raw_shmem
    endpoints: [m55_hp, m55_he]
    carve_out_kb: 64
    name: he_offload                # -> ALP_IPC_<NAME>_ADDR / _SIZE / ... macros
```

Once the v0.4 `extras-v04` group lands the upstream MaJerle/lwrb
+ nanopb/nanopb packs, the framing flips from "placeholder
hand-rolled" to "nanopb-generated codec".  That future switch needs
a schema-backed emitted block before it belongs in `board.yaml`; the
application code above does not change; the framing is transparent inside
`alp_mbox_send`.

## 5. Build + flash

```bash
# HP-side:
west alp-build -b alif_e7_dk_rtss_he examples/multicore/mproc-mailbox
# Outputs build/zephyr/zephyr.bin for HP.

# HE-side (TBD, dual-image flow):
# west alp-build -b alif_e7_dk_rtss_he examples/multicore/mproc-mailbox \
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
- [`examples/multicore/mproc-mailbox/`](../../examples/multicore/mproc-mailbox/) --
  the reference app (HP-side; HE-side TBD with v0.4 dual-
  image).
- [`tests/fuzz/mproc_frame_fuzz.c`](../../tests/fuzz/mproc_frame_fuzz.c)
  -- fuzz harness for the envelope framing.
- [`src/common/proto/alp_mproc_frame.h`](../../src/common/proto/alp_mproc_frame.h)
  -- the framing implementation.
