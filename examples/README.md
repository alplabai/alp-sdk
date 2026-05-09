# examples/

Reference applications shipped with the ALP SDK.  Each example is a
self-contained Zephyr application that builds against this repo as
a Zephyr module.

## v0.1 status

| Example                                            | Target board               | Status   | Notes                                                                                                  |
|----------------------------------------------------|----------------------------|----------|--------------------------------------------------------------------------------------------------------|
| [`edgeai-vision-aen/`](edgeai-vision-aen/)         | E1M EVK + E1M-AEN701       | skeleton | Camera → Ethos-U inference → OLED.  Pipeline shape lands in v0.1; full implementation in v0.2.        |
| [`iot-connected-camera/`](iot-connected-camera/)   | E1M EVK + E1M-V2N101/102   | skeleton | Camera → DRP-AI3 → MQTT/TLS publish + LVGL UI.  Pipeline shape v0.1; full implementation in v0.3.     |

Versions and acceptance criteria per example live in
[`../VERSIONS.md`](../VERSIONS.md).  CI runs each example's twister
scenario on every PR; the EdgeAI skeleton compiles under
`native_sim/native/64` until the EVK-AEN board file lands.

## Adding a new example

1. Create `examples/<name>/` with a Zephyr-app layout.
2. Add a row to the table above with a one-line summary.
3. Bind it to a `VERSIONS.md` deliverable so the acceptance bar is
   explicit.
4. Wire a `testcase.yaml` so twister picks it up — minimum gate is
   "compiles under `native_sim/native/64`."
