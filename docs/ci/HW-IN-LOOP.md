# Hardware-in-loop: the bench-run contract

**CI does not drive the bench.**  There is no self-hosted HIL runner,
public or private.  Two facts make a runner-based flow unlawful/unworkable
here, not just undesirable:

- **The bench is a strictly serial, labgrid-reservation-gated
  resource.**  A GitHub Actions runner queue has no concept of "hold
  this exact board for me"; a queued CI job racing a human on the bench
  for the same reservation is a correctness problem, not a scheduling
  inconvenience.
- **SETOOLS is license-gated and must not be redistributed.**  Alif's
  Security Toolkit (`app-gen-toc`, `app-write-mram`) is required for a
  production MRAM flash and is not shipped by alp-sdk — customers get
  it directly from Alif.  A shared CI runner with SETOOLS installed
  would be exactly the redistribution the license forbids.

Real-silicon verification is instead an **explicitly-invoked bench
run**: a person holds a labgrid reservation, runs
[`tests/hil/run_smoke.py`](../../tests/hil/run_smoke.py) (or the
scripts under [`scripts/bench/`](../../scripts/bench/)) against the
reserved board, and attaches the result to the PR or release that
needs it — a pasted log, a `docs/test-plan.md` row flip, or both.
There is no automated gate this document promises; it documents how to
run one by hand.

(History: this repo shipped a `nightly-aen-hil.yml` skeleton workflow
through v0.13 that assumed a self-hosted `hil-aen` GitHub Actions
runner would eventually come online.  It never did — the two
constraints above mean it never lawfully could — and the workflow was
deleted rather than left as a permanently-red/never-running skeleton.)

## Hardware

- E1M Development Board (UG-E1M-001) — see
  [`docs/boards/e1m-evk.md`](../boards/e1m-evk.md).
- E1M-AEN801 SoM seated in the M.2 / E1M socket (or the matching SoM
  for whichever board dir under `tests/hil/` you're running).
- Power: 12 V via the barrel jack OR USB-C with the host.
- Debug: SEGGER J-Link or Alif's recommended SWD adapter on J2
  (FTSH-105 10-pin).
- Serial: the bench fronts every UART with **labgrid**, not a bare
  USB-serial enumeration.  On the AEN801 bench specifically:
  `/dev/ttyACM0` is the DPS-150 **programmable power supply**, *not* a
  console — do not point a capture tool at it.  The app console is
  UART5, and the SE-UART is a second, separate device.  Neither raw
  `/dev/ttyUSB*` path is durable either: labgrid allocates a ser2net
  port fresh **on reservation acquire**, and it differs per session.
  Resolve the port at run time (`--serial-port` /
  `ALP_HIL_SERIAL_PORT` for `run_smoke.py`; `SE_UART` for the
  `scripts/bench/aen/` helpers) — never hardcode a device path in a
  spec, script, or doc.

## Bench host

- Linux (Ubuntu 22.04 LTS or newer) with the board wired per the
  hardware section above, under an active labgrid reservation.
- Zephyr toolchain at `~/zephyrproject/` (or wherever your workspace
  puts it — see `docs/testing.md`).
- `west`, `pyocd` or SEGGER J-Link Software Pack, as the flash method
  needs.
- `SETOOLS_DIR` exported and pointing at your own Alif-obtained
  SETOOLS install for any Flow A/D (production MRAM) work —
  `scripts/bench/aen/bench-env.sh`'s `bench_require_setools` enforces
  this at the point of use and refuses to guess a path.

## Serial-capture helper

`tests/hil/run_smoke.py`'s real-run mode shells out to a capture
helper (`/opt/alp-hil/capture-serial.sh` by default — a leftover
convention from the retired runner-host layout, still a reasonable
place to keep your own copy) to record UART output for a spec's
`serial.duration_s`.  A minimal implementation:

```bash
#!/usr/bin/env bash
set -euo pipefail
PORT=
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
if [[ -z "$PORT" ]]; then
    echo "capture-serial.sh: --port is required (no default -- the" >&2
    echo "  bench allocates it per labgrid reservation)" >&2
    exit 1
fi
timeout "${DURATION}" python3 -m serial.tools.miniterm \
    --raw --quiet --eol LF "$PORT" 115200 \
    > "$OUTPUT" 2>&1 || true
```

`run_smoke.py` passes a fresh, per-run, per-spec `--output` path (see
its module docstring) — never a single fixed log file — so a capture
that silently fails to write can never be asserted against a
*previous* run's stale log and read as a false PASS.

## Quarantine policy

Flaky specs get quarantined explicitly, same idea as before, just
without a workflow to skip a step in:

1. Open an issue tagged `hil-flaky`.
2. Mark the offending spec (`tags: [flaky]` if/when the schema grows
   that field, or a `pending_hardware_support:` note in the meantime —
   see `tests/hil/README.md`) and skip it in the bench run.
3. Track and unquarantine within the same release cycle.

## Adding board coverage

There is no `hil-<sku>` runner label to register any more.  Adding a
new SoM's bench coverage means:

1. `tests/hil/<sku>-<board>/_runner.yaml` with the board target +
   flash method (no `serial_port:` default — see above).
2. SoM-specific specs alongside it, per
   [`tests/hil/README.md`](../../tests/hil/README.md#adding-a-new-som).
3. A person with bench access runs it under a held reservation and
   attaches the result to the PR that adds the board — that pass is
   what flips the relevant `docs/test-plan.md` rows from `⏳` to `✅`,
   not a green check on the PR itself.

## Future boards

Coverage for V2N / V2N-M1 / i.MX 93 follows the same bench-run
contract above — their board dirs already exist under `tests/hil/`
(`v2n101-x-evk`, `v2n102-x-evk`, `v2m101-x-evk`, `v2m102-x-evk`,
`nx9101-evk`) — no separate runner-label scheme to design; the
constraint (serial, reservation-gated, license-gated tooling) is the
same for every board on this bench.
