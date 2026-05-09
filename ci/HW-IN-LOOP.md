# Hardware-in-loop runner setup

The `nightly-aen-hil` workflow requires a self-hosted GitHub
Actions runner attached to a real **E1M EVK** populated with an
**E1M-AEN701** SoM.  This document is the runner's installation
contract — when it changes, the workflow's expectations change
with it.

## Hardware

- E1M Development Board (UG-E1M-001) — see
  [`docs/boards/e1m-evk.md`](../docs/boards/e1m-evk.md).
- E1M-AEN701 SoM seated in the M.2 / E1M socket.
- Power: 12 V via the barrel jack OR USB-C with the host.
- Debug: SEGGER J-Link or Alif's recommended SWD adapter on J2
  (FTSH-105 10-pin).
- Serial: USB-C from the EVK exposes the board's UART as
  `/dev/ttyACM0` on the runner host.

## Runner host

- Linux (Ubuntu 22.04 LTS or newer).
- GitHub Actions runner registered with the `alpCaner/alp-sdk`
  repo, labelled `self-hosted`, `linux`, and `hil-aen`.
- Zephyr toolchain at `~/zephyrproject/` (cached across runs).
- `west`, `pyocd`, `J-Link Software Pack` installed.
- `/dev/ttyACM*` and the J-Link USB device readable by the runner
  user (udev rule recommended).

## Runner-side helpers

The workflow expects two scripts at `/opt/alp-hil/`:

| Script                    | Purpose                                                              |
|---------------------------|----------------------------------------------------------------------|
| `check-aen-attached.sh`   | Exits non-zero unless an Alif dev kit is enumerated on USB.  Fails fast before a flash cycle if the board's unplugged. |
| `capture-serial.sh`       | Wraps `pyserial-miniterm` (or similar) to capture the EVK's UART for a given duration.  Used to assert ztest pass strings. |

Both ship as part of the `alp-hil` runner-bundle repo (TBD —
v0.1.x setup deliverable).  Until that repo exists, copy the
bash sketches below as a starting point.

### `check-aen-attached.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
# Alif Ensemble dev-kit J-Link USB IDs (verify with `lsusb`):
JLINK_VID="1366"
JLINK_PID="1015"
if ! lsusb -d "${JLINK_VID}:${JLINK_PID}" >/dev/null; then
    echo "no Alif dev kit detected on USB" >&2
    exit 1
fi
```

### `capture-serial.sh`

```bash
#!/usr/bin/env bash
set -euo pipefail
PORT=/dev/ttyACM0
DURATION=30
OUTPUT=hil-output.log
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)     PORT="$2";     shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --output)   OUTPUT="$2";   shift 2 ;;
        *) echo "unknown arg $1" >&2; exit 1 ;;
    esac
done
timeout "${DURATION}" python3 -m serial.tools.miniterm \
    --raw --quiet --eol LF "$PORT" 115200 \
    > "$OUTPUT" 2>&1 || true
```

## Runner labels

The workflow uses `runs-on: [self-hosted, linux, hil-aen]`.
Adding more boards (e.g. V2N) means adding a runner with
`hil-v2n` and a parallel `nightly-v2n-hil.yml` workflow.

## Quarantine policy

Flaky HIL tests have to be quarantined explicitly — there is no
"retry on failure" in the workflow.  When a real flake surfaces:

1. Open an issue tagged `hil-flaky`.
2. Skip the offending ztest with `tags: -alp-hil-quarantine` in
   `testcase.yaml`.
3. Track and unquarantine within the same release cycle.

## Future runners

- `hil-v2n` — E1M EVK + E1M-V2N101 SoM.  Targets v0.2.
- `hil-v2n-m1` — E1M EVK + E1M-V2N-M1 SoM (DRP-AI3 + DEEPX DX-M1).
  Targets v0.3.
- `hil-aen-e3` and `hil-aen-e8` — coverage of the cheaper / pricier
  Ensemble variants.  Targets v0.4 once the v0.1.x AEN flow is
  battle-tested.
