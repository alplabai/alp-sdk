# can-loopback

Per-peripheral example for `<alp/can.h>`.  Brings up CAN0 in
loopback mode, sends a frame, demonstrates the receive-callback
contract.

## What this shows

- Opening a CAN bus by portable bus ID (`ALP_E1M_CAN0`) with
  `loopback = true`.
- Frame construction with `alp_can_frame_t` (11-bit ID, 8 byte
  payload).
- Filter installation + receive callback dispatch.

## Build

```bash
west build -b native_sim/native/64 examples/peripheral-io/can-loopback \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/can.h>`](../../../include/alp/can.h)
