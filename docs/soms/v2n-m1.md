# E1M-X V2N-M1 family

> V2N + on-module **DEEPX DX-M1** NPU.  AI-accelerator variant.

## SKUs

| SKU            | Memory                                | Status     |
|----------------|---------------------------------------|------------|
| `E1M-V2M101`   | 32 Gbit LPDDR4X + 32 Gbit eMMC + DX-M1| production |
| `E1M-V2M102`   | 64 Gbit LPDDR4X + 128 Gbit eMMC + DX-M1| production |
| `E1M-V2M103`   | 32 Gbit LPDDR4X + 128 Gbit eMMC + DX-M1| production |

## What's different from V2N base

V2N-M1 inherits the full V2N base module (see [`v2n.md`](v2n.md))
and adds:

| Component                | Where + how                                                |
|--------------------------|------------------------------------------------------------|
| **DEEPX DX-M1 NPU**      | On-module, PCIe                                            |
| `M1_RESET`               | Renesas-side GPIO controlling DX-M1 reset (active-low)     |
| 2 × PI3DBS12212A muxes   | Switch PCIe routing between DEEPX and the E1M edge         |
| 0.75 V DEEPX rail        | DA9292 CH2 (disabled on V2N base; brought up by U-Boot on M1, over RIIC8/BRD_I2C -- Cortex-A55/Linux-exclusive) |
| 3 × TPS628640 bucks      | DDR5/LPDDR rails for DEEPX (`0x44` / `0x4F` / `0x48`, all bench-confirmed, see below) |

## DEEPX bring-up

Four-step sequence, run **after** the Renesas side boots and
**before** the Linux kernel attempts to open the PCIe device.  Steps
1, 3, and 4 run in **U-Boot's `board_late_init()`**
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/
0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch` for step 1;
`0001-rzv2n-dev-EEPROM-gated-DEEPX-DX-M1-PCIe-bring-up.patch` for
steps 3-4) -- no application firmware (CM33 or Linux) writes or
re-runs them.  Step 2 (below) is not implemented by either patch; it
remains a bench-diagnostic check, not an automated bring-up step:

1. **Enable the 0.75 V DEEPX rail** via the secondary PMIC's CH2, over
   RIIC8/BRD_I2C.  This bus is Cortex-A55/Linux-exclusive
   (`metadata/e1m_modules/v2n/core-ownership.yaml`); U-Boot runs on
   the A55 before Linux starts, so it -- not the CM33 -- is the sole
   writer.
2. **ACK-probe** the DEEPX TPS628640 instances at `0x44` / `0x4F` /
   `0x48` to confirm population (self-regulating).  `deepx_lpddr_0v85`
   (`0x48`) only ACKs after step 1 drives `P64` high -- see the strap
   note below.
3. **Route the PCIe muxes** to the DEEPX path with the PI3DBS12212A
   driver (PD pin on Renesas `P80`, SEL pin on `P95`).
4. **Release `M1_RESET`** (Renesas `PA6`; active-low) -- ONLY once
   step 1 confirms the rail is power-good.

### `deepx_lpddr_0v85` strap is resolved: `0x48` (#1163, #1845)

The third DEEPX buck (`tps628640`, role `deepx_lpddr_0v85`) is
`address_7bit: "0x48"` on the V2M pair --
`metadata/e1m_modules/E1M-V2M101.yaml` / `E1M-V2M102.yaml` /
`E1M-V2M103.yaml`.  **Bench-measured 2026-09-24 on E1M-V2M103:**
`0x48` ACKs on `BRD_I2C` only once `P64` (`DEEPX_CORE_0P75_EN`) is
driven high (step 1 above), and its VOUT reads `0x5A` (= 0.85 V:
0.4 V + 90 x 5 mV) -- matching the role.  This is the same address the chip's
own default strap gives, but that was NOT sufficient on its own to
resolve the strap (see the now-superseded collision history below);
the bench measurement is what confirms it.

Superseded history: this address was `TBD` because of an apparent
collision with `tmp112` (also nominally strappable to `0x48`..`0x4B`)
-- that premise no longer held once `tmp112` was maintainer-confirmed
at `0x40` (all six V2N-family SKUs, one shared PCB, one ADD0 net; see
`metadata/chips/tmp112.yaml`), and the bench measurement above then
confirmed `0x48` directly rather than inferring it from the
non-collision. See
[#1163](https://github.com/alplabai/alp-sdk/issues/1163) and
[#1845](https://github.com/alplabai/alp-sdk/issues/1845) for the full
history.

The `chips/deepx_dxm1/` driver wraps steps 3-4 into a single
[`deepx_dxm1_bring_up(&ctx, DEEPX_DXM1_DEFAULT_BOOT_US)`](../../include/alp/chips/deepx_dxm1.h)
call, for platforms where a portable caller owns `M1_RESET`.  On
V2N-M1, U-Boot implements steps 3-4 itself with raw register pokes
(not this driver) so it can gate them on step 1's rail check; nothing
calls `deepx_dxm1_bring_up()` here.  Step 1 similarly has its own
driver API (`chips/da9292/`), which U-Boot does not call either (same
reason: U-Boot builds standalone against upstream sources, not
alp-sdk) -- see `docs/bring-up-v2n-m1.md` §2.

Walk-through with code: [`docs/bring-up-v2n-m1.md`](../bring-up-v2n-m1.md).

## DEEPX runtime

The DEEPX silicon's userland API (`libdxrt.so`) is upstream at
[`github.com/DEEPX-AI/dx_rt`](https://github.com/DEEPX-AI/dx_rt).
The Yocto layer that brings it into your image is wired in
`meta-alp-sdk/conf/machine/e1m-v2m101-a55.conf` and references
`github.com/DEEPX-AI/meta-deepx-m1`.

Integration cross-link: [`vendors/deepx-dxm1/README.md`](../../vendors/deepx-dxm1/README.md).

## DEEPX DX-M1 bring-up (Yocto image, bench-verified)

**Bench-verified 2026-09-26 on E1M-V2M103**, after a DEEPX-supplied
DX-M1 firmware update, with a V2M103 image carrying `dx-driver` 1.8.0
+ `dx-rt` 3.2.0 installed from `meta-deepx-m1` (the pins in
`conf/machine/include/e1m-v2m-deepx.inc`).

### What the image provides

* The DX-M1 flash-boots firmware 2.4.0 and enumerates as PCIe
  `1ff4:0000` Gen3 x2.
* `dx_dma` + `dxrt_driver` autoload at boot
  (`dx_dma_pcie ... Probe Done!!`, `dxrt_driver_cdev_init: 1 devices`).
* `/dev/dxrt0` is `0660 root:video`, via the `99-dx-dma.rules` udev
  rule this layer's `dx-driver_%.bbappend` tightens (see "What's
  different from V2N base" above).
* `/usr/bin/{dxrt-cli,dxrtd,dxtop,run_model,parse_model,dxbenchmark}`
  come from the `dx-rt-cli` sub-package, which the image installs.

### Verifying the bring-up

```
dxrt-cli -s
```

should return rc `0` and report: `DXRT v3.2.0`, `RT driver v1.8.0`,
`PCIe driver v1.6.0`, `FW v2.4.0`, LPDDR5x `6000` Mbps / `3.92` GiB,
Board `M.2 Rev 0.2`, NPU0-2 at `1000` MHz. Any mismatch here (wrong
firmware version, a device stuck in reset, an unprogrammed NAND)
surfaces before an inference is ever attempted.

### Inference smoke test

DEEPX's public model zoo publishes `.dxnn` models compiled per firmware
release (`https://sdk.deepx.ai/modelzoo/dxnn/2_4_0/<model>.dxnn` for FW
2.4.0). They are licensed for evaluation and development only. Run them with
`run_model`:

```
run_model -m mobilenetv2_224x224.dxnn -s -l 50 -v   # single request, per-inference timing
run_model -m mobilenetv2_224x224.dxnn -b -l 300     # throughput, all three NPUs
```

Measured on E1M-V2M103 (FW 2.4.0, DX-RT 3.2.0, PCIe Gen3 x2):

| Model | NPU time | End-to-end latency (`-s`) | Throughput (`-b`, 3 NPUs) |
|---|---|---|---|
| `mobilenetv2_224x224` | 0.68 ms | 1.16 ms | 2781 FPS |
| `resnet18_224x224` | 0.86 ms | 1.34 ms | -- |
| `yolov5-s_640x640` | 4.09 ms | 62.1 ms | 33.8 FPS |

YOLOv5s end-to-end latency is dominated by host-side output handling, not by
the NPU. Use a loop count (`-l`): the time-bounded mode (`-t <s>`) stalled
after its warm-up runs on this setup. `dxrt-cli -s` reports NPU voltage as
`0 mV` with the no-PMIC firmware, because there is no PMIC read-out.

### DX-RT service mode (`dxrtd`) -- enabled (#2398)

DX-RT 3.x has an optional, CMake-time "service mode"
(`USE_SERVICE`) where `dxrtd` runs as a background daemon so more
than one process can share the DX-M1 concurrently. The `dx-rt_3.2.0`
recipe this layer pins (`PREFERRED_VERSION_dx-rt = "3.2.0"`, which
selects `dx-rt_3.2.0.bb`, not the `-1` suffix) built with
`USE_SERVICE=OFF` -- upstream's own default -- and shipped no `dxrtd`
unit at all: two processes using the DX-M1 at once hung, and a client
killed mid-request wedged the NPU until reboot.

`meta-alp-sdk`'s `dx-rt_%.bbappend`
(`meta-alp-sdk/dynamic-layers/meta-deepx-m1/recipes-runtime/dx-rt/`)
now flips `EXTRA_OECMAKE` to `-DUSE_SERVICE=ON` and installs upstream's
`dxrt.service` as an enabled systemd unit in `dx-rt-cli` (`inherit
systemd`; upstream's own `dx-rt_3.2.0-1.bb` variant ships the same
unit but commented out, and this image runs systemd rather than
SysVinit, so the bbappend stages it directly rather than re-pinning).
The `PREFERRED_VERSION_dx-rt = "3.2.0"` pin is unchanged (firmware
lockstep). `run_model` and `dxrt-cli` still open the device directly
and work unchanged against the running `dxrtd`. The real limit is
**at most 3 distinct NPU core sets live on one DX-M1 at a time**: the
kernel driver keeps one hardware queue per distinct core set
(`DX_NORMAL_QUEUE_MAX = 3`), engines on the same set share a queue,
and "all cores" counts as a set. A fourth set is refused by the
driver (`-EBUSY`) and dx-rt aborts the process (or kills `dxrtd`)
instead of failing cleanly; the limit is unchanged in driver
v2.6.0. Bench evidence (#2398): three processes on `CORE_0`,
`CORE_0`, `CORE_0` work; three on `CORE_0`/`CORE_1`/`CORE_2` hit
`Failed to set NPU bound 3 ... ret: -16` and killed `dxrtd`
(hypothesis, not confirmed: the engine the SDK used to build on all
cores before rebinding took a queue -- `alp_deepx_inference_open()`
now builds the engine on the requested cores directly). Inside one
process the SDK refuses a fourth distinct set with `ALP_ERR_BUSY`;
across processes nothing guards it. See `<alp/ext/deepx/inference.h>`
and #2398.

To run two models on the DX-M1 side by side, open them with
`alp_deepx_inference_open(cfg, cores)` (e.g. `ALP_DEEPX_NPU_CORES_01`
and `ALP_DEEPX_NPU_CORE_2`). To run one model on the DX-M1 and one on
the on-die DRP-AI3 at the same time, see
`examples/v2n/v2n-two-models/` (not bench-verified).

### Running DX-M1 next to other code on a V2M

- **`libdxrt` installs crash handlers at load.** A static initialiser in
  `libdxrt` registers handlers for `SIGSEGV`, `SIGBUS` and `SIGABRT` that
  call `exit(1)`. Because `libalp_sdk` links `libdxrt` on V2M, this
  affects every SDK application there, DX-M1 users or not: a crash or
  `abort()` in the app, the DRP-AI runtime or an `assert` ends as a silent
  exit code 1 with no core dump, the `exit()` runs from inside a signal
  handler (not async-signal-safe, can hang), and any `SIGSEGV`/`SIGABRT`
  handler the app installed before the library loaded is replaced.
  Handlers installed after load win, so an app that needs core dumps
  reinstalls `SIG_DFL` at start-up. Read from the library's behaviour and
  symbols; the hang risk and the DRP-AI-only-process case are unverified
  on the bench.
- **`ALP_INFERENCE_BACKEND_AUTO` means DX-M1 here.** On a build with both
  NPU backends, AUTO resolves to DEEPX at compile time. A model for the
  on-die DRP-AI3 must name `.backend = ALP_INFERENCE_BACKEND_DRPAI`.
- **One model on each NPU at the same time** (DRP-AI3 + DX-M1) is what
  `examples/v2n/v2n-two-models/` does. The two stacks share no driver,
  device node, memory carve-out or IRQ; they do share the 4 A55 cores and
  DDR. Not bench-verified, and its DRP-AI half needs a RUHMI-enabled
  `libalp_sdk` (see `docs/bring-up-drpai-v2n.md`).

### DX-M1 NAND firmware provisioning

The DX-M1's application firmware lives on its own SPI NAND, separate
from the Yocto image, and is provisioned once, out of band, over
DEEPX's UART boot path (`fw_update_uart` from DEEPX's own tooling)
using firmware images obtained **from DEEPX** -- alp-sdk does not
redistribute them. If an update is interrupted or the DX-M1 wedges
afterward, cold-power-cycle the module (a warm reset is not
sufficient) before retrying.

### Hardware prerequisites

The bring-up above depends on these being true of the SoM:

* the DX-M1's boot straps select mode 0 (flash boot);
* the DX-M1's crystal oscillator has its bias resistor populated;
* SoMs without the DEEPX reference PMIC run DEEPX's no-PMIC firmware
  variant on the DX-M1;
* the DX-M1's NAND has been programmed at least once over its UART
  boot path (see above).

## Example apps targeting V2N-M1

All V2N examples apply.  DEEPX-specific examples land separately
as the NPU integration matures.

## Common gotchas

| Symptom                                              | Cause + fix                                                            |
|------------------------------------------------------|------------------------------------------------------------------------|
| U-Boot logs `ALP: DA9292 ...` abort / `DEEPX rail not enabled` / `DEEPX rail disabled` | 0.75 V plane shorted, or another check named in the printed register bytes failed; probe the rail directly.  See `docs/bring-up-v2n-m1.md` §2. |
| U-Boot logs `... DEEPX rail not enabled (CH2 may require EN2/P64 high before PG -- see bring-up doc)` | CH2_EN was written over I2C but PG never asserted -- possible EN2/P64 hardware gating (P64 only goes high after PG in the current sequence). See `docs/bring-up-v2n-m1.md` §2 and #2045. |
| DEEPX rails up but PCIe link never trains            | `M1_RESET` polarity wrong -- the driver default is active-low; board may need override via `deepx_dxm1_set_reset_polarity`. |
| PCIe link trains but kernel driver reports BAR errors| PCIe muxes on the wrong path -- check `PI3DBS_STATE_PATH_0` matches your board's silk-screen. |
| `dxrt::InferenceEngine` construction fails          | Check the DEEPX kernel driver (`dx_rt_npu_linux_driver`) is loaded.    |

## See also

* [`v2n.md`](v2n.md) -- the base SoM.
* [`../bring-up-v2n-m1.md`](../bring-up-v2n-m1.md) -- bench bring-up.
