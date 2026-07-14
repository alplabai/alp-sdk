# Callback self-close: deferred teardown across rpc / mqtt / ble / can / gpio

Design for [#756](https://github.com/alplabai/alp-sdk/issues/756) — residual follow-up to #629.

Status: approved 2026-07-14. Implemented over three branches (see *Sequencing*).

## Problem

A callback that closes its own handle deadlocks on four surfaces. All four were
verified against `origin/dev`; the issue's claims are accurate in every case.

| Surface | Callback site | What is held | Close path that blocks |
| --- | --- | --- | --- |
| MQTT (Yocto + Zephyr) | `src/backends/mqtt/yocto_drv.c:290`, `src/backends/mqtt/zephyr_drv.c:272` | counted op from `alp_mqtt_loop` (`src/mqtt_dispatch.c:194`) | `alp_mqtt_close` drains at `src/mqtt_dispatch.c:213` |
| CC3501E BLE | `src/backends/ble/cc3501e.c:191` | counted op from `alp_ble_scan_start` (`src/ble_dispatch.c:246`) | `alp_ble_close` drains at `src/ble_dispatch.c:148` |
| Yocto CAN | `src/backends/can/yocto_drv.c:189` | `d->lock` (taken at `:184`) | `y_close` reacquires `d->lock` (`:499`), then joins the RX thread (`:513`) |
| Yocto GPIO | `src/yocto/peripheral_gpio.c:344` | `g_irq.mu` (taken at `:329`) | `alp_gpio_irq_disable` (`:431`) and `alp_gpio_close` (`:457`) reacquire `g_irq.mu` |

Two distinct failure modes:

- **Counted-op self-wait** (MQTT, BLE) — the callback runs inside an operation
  the closer is waiting to drain. The thread waits for itself.
- **Callback-under-lock, plus self-join** (CAN, GPIO) — the callback runs while
  the worker holds a mutex that close must reacquire. CAN additionally joins the
  very thread that is running the callback.

The second mode needs more than deferral, which is why it lands last.

## Contract

**Closing a handle from inside its own callback is supported** on `rpc`, `mqtt`,
`ble`, `can` and `gpio`. `close()` returns immediately; teardown completes when
the callback's owning context unwinds. Operations already in flight when close
begins are honoured; fresh operations started after close completes are not
(the POSIX `close(2)` model).

This is not a free choice. Every affected entry point returns `void` —
`alp_rpc_close` (`include/alp/rpc.h:275`), `alp_mqtt_close`
(`include/alp/iot.h:251`), `alp_ble_close` (`include/alp/ble.h:91`),
`alp_can_close` (`include/alp/can.h:222`), `alp_gpio_close`
(`include/alp/peripheral.h:321`) — so there is no channel through which to
report "unsupported". Prohibiting self-close would require an API break on five
public functions.

`<alp/rpc.h>` (lines 242-271) already documents self-close as supported and
race-free. The other four surfaces adopt that same rule rather than introducing
a second, contradictory one.

## Architecture

Self-detection stays in the **backend**, because only the backend knows how its
callbacks are scheduled — a dedicated worker thread
(`pthread_equal(pthread_self(), ch->rx_thread)`, `src/backends/rpc/yocto_drv.c:935`;
`k_current_get() == be->recv_thread`, `src/backends/rpc/zephyr_drv.c:961`) or the
application's own thread inside a counted operation. The dispatch layer stays
thread-agnostic and portable.

This is exactly the split `rpc_ops.h:22-66` already defines. The work is to
extract it, not to invent it.

```
dispatch (thread-agnostic, SHARED)        backend (knows its own threading)
─────────────────────────────────         ────────────────────────────────
alp_X_close(h)
  begin_close: CAS OPEN -> CLOSING
  ops->shutdown(state) ─────────────────► am I the callback thread?
                                            no  -> ALP_CLOSE_DONE
      ┌───────────────────────────────────◄ yes -> ALP_CLOSE_DEFERRED
      │
  DONE│  finalize() inline
      │    drain active_ops, destroy, set UNOPENED, release slot
      │
DEFER │  return immediately
      │    slot stays claimed and CLOSING -> structurally unrecyclable
      │
      └─ callback unwinds ──────────────► epilogue: alp_X_close_finalize(owner)
                                            same shared finalize()
```

The two failure modes converge on one protocol. A dedicated worker (RPC rx, CAN
rx, GPIO irq) finalizes at its loop epilogue. A caller-driven operation (MQTT
`loop`, BLE scan) finalizes at that operation's epilogue. Same states, same
handoff; only the location of the epilogue differs.

### New shared unit

`src/common/alp_deferred_close.{h,c}`, beside `alp_slot_claim.h` and reusing its
`lifecycle` / `active_ops` primitives (`alp_handle_op_enter` at
`src/common/alp_slot_claim.h:149`, `alp_handle_begin_close_blocking` at
`src/common/alp_slot_claim.c:55`).

It owns exactly two things:

- the `DONE | DEFERRED` outcome type and the close-side sequencing, and
- the shared `finalize()` body: drain, destroy, set `UNOPENED`, release slot.

It owns no threading. It is usable from Zephyr, Yocto and bare-metal unchanged.

### Per-surface work

| Surface | Shape | Self-detect | Finalize point |
| --- | --- | --- | --- |
| RPC | worker | already implemented | already implemented — migrate onto the helper |
| MQTT | caller-driven | thread entering `ops->loop` | `alp_mqtt_loop` epilogue |
| BLE | caller-driven | thread entering the scan op | `alp_ble_scan_start` epilogue |
| CAN | worker | `pthread_equal(self, rx_thread)` | `_rx_loop` epilogue; **must not self-join** |
| GPIO | worker | `pthread_equal(self, irq_thread)` | `irq_dispatcher` epilogue |

CAN and GPIO additionally require moving callback invocation **outside** the
`d->lock` / `g_irq.mu` critical section while keeping the object alive across the
call. Deferral alone does not fix them: a self-close still reacquires the mutex.
Equally, moving the callback out of the lock alone does not fix them either —
lifetime and CAN's self-join must be handled together. #756 calls this out
explicitly, and it is why they are sequenced last.

MQTT has a single shared dispatch (`src/mqtt_dispatch.c`) over both backends via
the `mqtt_ops.h` vtable, so one dispatch-side change covers Yocto and Zephyr;
only the self-detect is per-backend.

## Testing

Each surface gets a self-close test modelled on the existing
`tests/yocto/rpc_yocto_self_close.c` and `tests/yocto/rpc_uio_self_close.c`.

**Every test must be shown to hang or fail against the unfixed code before the
fix, and pass after.** A test that passes both ways proves nothing. (This is the
discipline #790 used and #791 documents.)

ThreadSanitizer: `run_sanitized_rpc_tests.sh` already exists and is wired to
ctest as `alp_test_rpc_asan` / `alp_test_rpc_tsan` with `-fsanitize=thread`
(`CMakeLists.txt:239-256`), but is RPC-scoped. Generalize it to cover the Yocto
CAN and GPIO paths, per #756's acceptance criteria.

Public headers gain explicit callback-context and re-entry wording on all five
surfaces, mirroring `<alp/rpc.h>`.

## Sequencing

Three branches, each independently landable and green:

1. **`fix/756-deferred-close-helper`** — extract
   `src/common/alp_deferred_close.{h,c}`; migrate `rpc_dispatch.c` onto it.
   Behaviour-neutral; the existing RPC self-close tests are the safety net.
2. **`fix/756-mqtt-ble-self-close`** — the counted-op surfaces. Adds self-close
   tests and the header contract wording.
3. **`fix/756-can-gpio-self-close`** — the lock-held and self-join surfaces, plus
   the TSan harness generalization.

Splitting this way keeps the risky refactor (1) isolated behind proven tests, and
prevents the hardest surfaces (3) from blocking the straightforward ones (2).

## Out of scope

- Non-callback close races. #629 covered those; this is the residual.
- Any change to the callback-runs-in-ISR rules for GPIO/I2C targets
  (`include/alp/peripheral.h:289-305`, `:459-494`). Those constrain what a
  callback may do; they are orthogonal to whether it may close its own handle.
