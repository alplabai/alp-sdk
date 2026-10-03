# AMP window from metadata: bench steps (NOT YET RUN)

The OpenAMP shared window now comes from the SoC metadata's `openamp_carveout`
(`metadata/socs/renesas/rzv2n/n44.json`) and the beacon layout from
`include/alp/protocol/amp_beacon.h`. Addresses are unchanged
(A55 `0x4F700000`, `0x900000` bytes, CM33-NS `0x9F700000`, beacon `0x4F700FF0`),
so a build from this branch must behave exactly like the one before it. None of
the steps below has been run.

Ordered steps on an E1M-V2N101 / E1M-V2M101 with the console on the A55:

1. Build and flash the idle shim (`firmware/alp-stock-shim`) as the CM33 image;
   cold power-cycle.
2. On Linux: `devmem 0x4F700FF0 32` reads `0xA10D0683`; `devmem 0x4F700FF4 32`
   reads `0x00000100`; `devmem 0x4F700FF8 32` read twice a few seconds apart
   grows; `devmem 0x4F700FFC 32` reads `0x0`.
3. `cat /proc/iomem | grep -i 4f7` lists the reservation `0x4f700000-0x4fffffff`;
   `ls /sys/bus/platform/devices | grep -E '4f700000.rsctbl|4f701000.mhu-shm'`
   lists both UIO devices.
4. Flash `examples/multicore/rpmsg-v2n/m33_sm` instead; cold power-cycle. Repeat
   step 2: version is `0x2`, magic unchanged, heartbeat grows, epoch is even
   (`0x0`) until a session is open.
5. Run the RPC echo test (`tests/hil/v2m103-x-evk/v2m103-rpmsg-echo-uio.yaml` flow):
   epoch turns odd while the session is open and even after close; a second
   open in the same CM33 boot still succeeds (attach reset).
6. Any difference from steps 2-5 means the generated board `.dts`, the Linux DT
   or `alp_amp_window.h` has drifted from the metadata: run
   `python3 scripts/check_amp_window.py` first.
