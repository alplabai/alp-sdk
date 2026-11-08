# heterogeneous-offload

> `[UNTESTED]` -- builds against the same V2N memory_map as rpmsg-v2n
> (no TBD addresses).  The underlying rpmsg-v2n transport is
> bench-proven (#697, #2374); the FFT offload itself has not been
> benched.

**The "why heterogeneous compute?" flagship.**  The A-cluster
delegates a 64-point FFT to its M-class peer over RPMsg and reads
back the magnitude spectrum.  Each core does what it's best at:

| Side                    | Why this core?                                          |
|-------------------------|---------------------------------------------------------|
| **A55 / Yocto (Linux)** | ALSA + libcamera + standard Linux audio plumbing.       |
| **M33-SM / Zephyr**     | CMSIS-DSP, deterministic timing, hardware FPU.          |

The same SoM (E1M-V2N101) runs both halves -- one die, two
operating systems, one declarative project.

```
examples/multicore/heterogeneous-offload/
├── board.yaml          (v2; declares a55_cluster + m33_sm + ipc)
├── README.md           (this file)
├── CMakeLists.txt      (multi-slice marker)
├── linux/              (a55_cluster's Yocto slice -- the audio capture
│   ├── CMakeLists.txt   + FFT requester via alp_rpc_call)
│   └── src/main.c
└── m33_sm/             (m33_sm's Zephyr slice -- the FFT worker)
    ├── CMakeLists.txt  (builds zephyr/drivers/ipc/resource_table.c)
    ├── prj.conf
    └── src/main.c
```

## The wire contract

Method names + payload shapes (both sides agree on these via the
shared `imu_fft.h`-style declaration inline in each main.c):

| Method | Request                       | Response                       |
|--------|-------------------------------|--------------------------------|
| `fft`  | 64 × `float32` (PCM samples)  | 33 × `float32` (magnitudes)    |

The A55 calls `alp_rpc_call(ch, "fft", samples, sizeof samples, mags,
&mags_len, /* timeout */ 1000u)` and blocks until the M33-SM has
returned the magnitude spectrum.

The transform is deliberately 64 points: one RPC frame (method name +
NUL + payload) must fit a single RPMsg buffer (496 B usable on the V2N
carve-out; the SDK TX cap `CONFIG_ALP_SDK_RPC_TX_FRAME_MAX` is 1024).
The request is 256 B and the response 132 B.  A 1024-point FFT (4096 B
request) cannot go in one frame -- chunk it across several calls.

## Why the M33-SM side is raw OpenAMP, not `<alp/rpc.h>`

The A55 requester goes through `<alp/rpc.h>` (`alp_rpc_call()` ->
`src/backends/rpc/yocto_uio_drv.c`).  The V2N M33-SM slice does **not**: it
copies the bench-proven raw OpenAMP bring-up of
[`rpmsg-v2n`](../rpmsg-v2n/) (`m33_sm/src/main.c`, `resource_table.[ch]`)
and serves the `fft` method itself, with the same `<method>\0<payload>`
framing on the fixed endpoint address 1024.  Reasons:

- The generic Zephyr backend (`src/backends/rpc/zephyr_drv.c`) rides on
  `ipc_service` with its own generated resource table and has no V2N
  bring-up (resource table in DDR, MHU doorbell).  It also publishes no AMP
  beacon.
- `yocto_uio_drv.c` refuses to open a channel until it reads the version-2
  beacon (`<alp/protocol/amp_beacon.h>`) and completes its attach-epoch
  handshake, so the M33 image must publish it.  The copied bring-up does.

So the board.yaml `ipc:` carve-out is only a paper entry here, and
`prj.conf` sets `CONFIG_MBOX`, `CONFIG_OPENAMP` and
`CONFIG_OPENAMP_RSC_TABLE=n` by hand, as `rpmsg-v2n` does.  Build the M33
image with the generated `alp.conf` layered on (as `testcase.yaml` does),
otherwise CMSIS-DSP is missing and the build stops with an `#error`.

## Memory map

Same layout as [`rpmsg-v2n`](../rpmsg-v2n/).  The tan-resolved 512 KiB
`alp_default_rpmsg` carve-out lives in `ocram_low` at
`0x00010000`; deterministic + non-cacheable.  That is the address the
generated `<alp/system_ipc.h>` carries.  The M33 slice's real OpenAMP
transport (resource table, vrings, shared buffers) sits in the SoC's
`openamp_carveout` instead: 9 MiB at `0x4f700000` (A55) =
`0x9f700000` (CM33-NS), see
[`rpmsg-v2n`'s memory map](../rpmsg-v2n/README.md#memory-map).

## Boot order

A55 → systemd → remoteproc attaches to the already-running M33 firmware
(booted by BL22 before Linux).  The M33 publishes the AMP beacon first,
creates its rpmsg endpoint at the fixed address 1024 once the A55 has
attached, and the A55's `alp_rpc_open()` dials that address; there is no
`alp_default_rpmsg` discovery on this link.

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

Fan-out:

- `build/a55_cluster-yocto/` (bitbake against `MACHINE = e1m-v2n101-a55`).
- `build/m33_sm-zephyr/` (Zephyr against `BOARD = alp_e1m_v2n101_m33_sm`).

`tan build` has no per-slice `--core` flag -- it rebuilds every slice
on each invocation.  To iterate on the M-side worker only, just
re-run the same command: the already-built Yocto slice is reused
(bitbake short-circuits an up-to-date tree) while the Zephyr slice
rebuilds incrementally in seconds. See
[`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md#iterating-on-one-slice).

## What you'll see

After remoteproc brings up the M33 firmware:

M33 console (Zephyr log, timestamps omitted):

```
heterogeneous-offload/m33_sm: starting
heterogeneous-offload/m33_sm: bringing up the rpmsg virtio device
heterogeneous-offload/m33_sm: FFT worker started
```

A55 requester:

```
[a55] heterogeneous-offload requester coming up
[a55]    generated 64-sample @ 400 Hz sine, calling fft...
[a55]    fft returned; dominant bin=4 (~400.0 Hz)
[a55]    generated 64-sample @ 800 Hz sine, calling fft...
[a55]    fft returned; dominant bin=8 (~800.0 Hz)
[heterogeneous-offload] done
```

## Reference

- [`<alp/rpc.h>`](../../../include/alp/rpc.h) -- the `alp_rpc_call`
  surface that makes the offload ergonomic.
- [`examples/multicore/rpmsg-v2n/`](../rpmsg-v2n/) -- the simpler async event
  pattern this builds on.
- [`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md)
  -- end-to-end walk-through.
