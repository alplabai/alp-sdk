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

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

(from an alp-workspace root instead: `tan build --project alp-sdk/examples/peripheral-io/can-loopback`)

board.yaml declares only m55_hp, so tan also builds the SDK's idle alp-stock-shim image for m55_he (build/m55_he-zephyr/); see [docs/heterogeneous-builds.md](../../../docs/heterogeneous-builds.md).

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# writes examples/peripheral-io/can-loopback/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/can-loopback
west build -b native_sim/native/64 examples/peripheral-io/can-loopback \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/can.h>`](../../../include/alp/can.h)
