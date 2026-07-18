# heterogeneous-offload

> `[UNTESTED]` -- v0.6 paper-correct.  Builds against the same V2N
> memory_map as rpmsg-v2n (no TBD addresses).  HiL bring-up gates
> on v0.8.

**The "why heterogeneous compute?" flagship.**  The A-cluster
delegates a 1024-point FFT to its M-class peer over RPMsg and reads
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
    ├── CMakeLists.txt
    ├── prj.conf
    └── src/main.c
```

## The wire contract

Method names + payload shapes (both sides agree on these via the
shared `imu_fft.h`-style declaration inline in each main.c):

| Method | Request                       | Response                       |
|--------|-------------------------------|--------------------------------|
| `fft`  | 1024 × `float32` (PCM samples)| 513 × `float32` (magnitudes)   |

The A55 calls `alp_rpc_call(ch, "fft", samples, sizeof samples, mags,
&mags_len, /* timeout */ 1000u)` and blocks until the M33-SM has
returned the magnitude spectrum.

## Memory map

Same layout as [`rpmsg-v2n`](../rpmsg-v2n/).  The 512 KiB
`alp_default_rpmsg` carve-out lives in `ocram_low` at
`0x00010000`; deterministic + non-cacheable.

## Boot order

Identical to `rpmsg-v2n`: A55 → systemd → remoteproc loads the M33
firmware → both sides discover `alp_default_rpmsg`.

## Build

```bash
tan build alp-sdk/examples/multicore/heterogeneous-offload
```

Fan-out:

- `build/a55_cluster-yocto/` (bitbake against `MACHINE = e1m-v2n101-a55`).
- `build/m33_sm-zephyr/` (Zephyr against `BOARD = alp_e1m_v2n101_m33_sm`).

Iterate on the M-side worker only:

```bash
tan build alp-sdk/examples/multicore/heterogeneous-offload --core m33_sm
```

## What you'll see

After remoteproc brings up the M33 firmware:

```
[m55_sm] FFT worker ready; waiting for 'fft' calls
[a55]    generated 1024-sample @ 440 Hz sine, calling fft...
[m55_sm] processing 1024-sample frame
[a55]    fft returned; dominant bin=23 (~430.7 Hz)
[a55]    generated 1024-sample @ 880 Hz sine, calling fft...
[m55_sm] processing 1024-sample frame
[a55]    fft returned; dominant bin=46 (~861.3 Hz)
[heterogeneous-offload] done
```

## Reference

- [`<alp/rpc.h>`](../../../include/alp/rpc.h) -- the `alp_rpc_call`
  surface that makes the offload ergonomic.
- [`examples/multicore/rpmsg-v2n/`](../rpmsg-v2n/) -- the simpler async event
  pattern this builds on.
- [`docs/heterogeneous-builds.md`](../../../docs/heterogeneous-builds.md)
  -- end-to-end walk-through.
