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

`src/common/alp_deferred_close.{h,c}`, beside `alp_slot_claim.h` and building on
its `lifecycle` / `active_ops` representation (`alp_handle_op_enter` at
`src/common/alp_slot_claim.h:149`, `alp_handle_begin_close_blocking` at
`src/common/alp_slot_claim.c:55`).

It owns exactly two things:

- the `ALP_CLOSE_DONE | ALP_CLOSE_DEFERRED` outcome type, and
- **split-phase close**: a CAS-only `begin` and a separate sleep-drain, so a
  caller can CAS, run `ops->shutdown()`, and only then decide whether to drain
  and finalize (external close) or return immediately (self-close).

The split is the actual new primitive. Today `alp_handle_begin_close_blocking`
**fuses** the CAS and the drain into one call, which is precisely why a deferred
self-close cannot be expressed with it: by the time it returns, the caller has
already waited for itself.

It owns no threading. It is usable from Zephyr, Yocto and bare-metal unchanged.

### Why RPC keeps `chan_word`

**The SDK already decided this**, and the decision is recorded in-tree.
`alp_slot_claim.h:200-201` documents `alp_handle_begin_close_blocking()` as

> "the generalisation of rpc_dispatch.c's `_rpc_begin_close()`/`_rpc_drain()`
> (GHSA-xhm8-7f87-93q5) to the shared handle-pool helpers"

So `alp_slot_claim.h` **is** the generalisation of RPC's close protocol. RPC is
its origin, not a duplicate of it to be eliminated. What that generalisation did
not carry across is the *split phase*: `alp_handle_begin_close_blocking()` fuses
the CAS and the drain into one call, which is exactly why a deferred self-close
cannot be expressed with it today. This design finishes the existing
generalisation rather than starting a new one.

Representation footprint, measured (not estimated): `lifecycle` + `active_ops`
is used by **32 translation units** under `src/`; the packed `chan_word` by
exactly two (`src/rpc_dispatch.c`, `src/backends/rpc/rpc_ops.h`).

RPC's divergence is also deliberate and documented: `src/rpc_dispatch.c:208-214`
rejected `alp_handle_begin_close()` because that helper's precondition — every
drained op is a short synchronous backend call — does not hold for
`alp_rpc_call()`, which can block up to `UINT32_MAX` ms. (That specific
objection is now largely obsolete: `alp_handle_begin_close_blocking()` sleeps
rather than spins, and the split-phase helper addresses the rest.)

Migrating RPC onto the shared representation is therefore **possible but not
required**, and is out of scope here. It would rewrite the one
adversarially-reviewed, working lock-free close protocol — the subject of
advisory **GHSA-xhm8-7f87-93q5** — for uniformity rather than function. It is
tracked as a follow-up (see *Follow-ups*), not carried by this work.

RPC remains the reference for the protocol *shape* (`rpc_ops.h:22-66`) without
sharing the *representation*.

### Follow-ups (not in scope)

- **Sleep-tick duplication.** The portable sleep-a-tick primitive exists
  verbatim in both `src/rpc_dispatch.c:223-228` and `src/common/alp_slot_claim.c:45-53`
  (the latter's comment admits it is "copied from rpc_dispatch.c"). Real
  duplication, small and safe to fix, but orthogonal to the deadlocks.
- **RPC `chan_word` migration.** Optional. If ever done, it must be gated on the
  existing RPC close coverage — `tests/yocto/rpc_dispatch_close_race.c`,
  `rpc_yocto_self_close.c`, `rpc_uio_self_close.c`, `tests/unit/rpc_zephyr_backend/`
  — run under `run_sanitized_rpc_tests.sh` (ASan + TSan). That coverage exists
  today, so the migration is defensible; it is simply not what fixes #756.

### Per-surface work

| Surface | Shape | Self-detect | Finalize point |
| --- | --- | --- | --- |
| RPC | worker | already implemented | unchanged — keeps `chan_word` (above) |
| MQTT | caller-driven | thread holding the counted op | `alp_mqtt_loop` epilogue, after `alp_handle_op_leave` |
| BLE | caller-driven | thread holding the counted op | `alp_ble_scan_start` epilogue, after `alp_handle_op_leave` |
| CAN | worker | `pthread_equal(self, rx_thread)` | `_rx_loop` epilogue; **must not self-join** |
| GPIO | worker | `pthread_equal(self, irq_thread)` | `irq_dispatcher` epilogue — **lock-scope fix, not lifecycle** |

For MQTT and BLE the callback runs on the *application's* thread inside a counted
op, so there is no backend worker to host the deferred finalize; the dispatcher's
own op epilogue hosts it. Self-detect is "the closing thread is the one currently
holding a counted op on this handle" — the handle records the op-owning thread
around `ops->loop` / `ops->scan_start`.

That form is safe here because `mosquitto_loop_start` is **not used** anywhere in
`src/backends/mqtt/` — libmosquitto runs in synchronous mode, so no callback can
fire from a library-internal thread. If threaded mode is ever adopted, this
self-detect must become a thread-identity compare instead.

MQTT has a single shared dispatch (`src/mqtt_dispatch.c`) over both backends via
the `mqtt_ops.h` vtable, so one dispatch-side change covers Yocto and Zephyr;
only the self-detect is per-backend.

**CAN and GPIO are not primarily lifecycle fixes.** Both invoke callbacks while
holding a mutex that close must reacquire, so deferral alone does not help; and
moving the callback out of the lock alone does not help either, because lifetime
and CAN's self-join remain. Both halves must land together — #756 says this
explicitly.

GPIO is the extreme case: `src/yocto/peripheral_gpio.c` **counts no ops at all**
(zero `alp_handle_op_enter` call sites). Its deadlock is purely `g_irq.mu` held
across `p->irq_cb` (`:329-347`) versus reacquisition at `:431` / `:457`. The fix
is to drop the mutex across callback invocation (the dispatcher already
re-validates slots at `:335`) and honour a callback-requested detach/release via
a flag after the callback returns. Whether GPIO also adopts the shared helper is
a marginal-cost judgement at implementation time — it is not forced.

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

Two branches, each independently landable and green:

1. **`fix/756-mqtt-ble-self-close`** — add the split-phase helper
   (`src/common/alp_deferred_close.{h,c}`) together with its first adopters, the
   counted-op surfaces MQTT and BLE. Adds their self-close tests and the public
   header contract wording.
2. **`fix/756-can-gpio-self-close`** — the lock-held and self-join surfaces, plus
   the TSan harness generalisation.

The helper ships with its first adopters rather than alone: with RPC not being
migrated, a helper landed by itself would have no caller and nothing to prove it.

This keeps the hardest surfaces (2) — where the real work is lock-scope and
self-join, not lifecycle — from blocking the straightforward ones (1).

## Out of scope

- Non-callback close races. #629 covered those; this is the residual.
- Any change to the callback-runs-in-ISR rules for GPIO/I2C targets
  (`include/alp/peripheral.h:289-305`, `:459-494`). Those constrain what a
  callback may do; they are orthogonal to whether it may close its own handle.
