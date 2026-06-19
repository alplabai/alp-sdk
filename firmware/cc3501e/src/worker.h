/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: async-job worker (the submit/poll seam).
 *
 * ===================== WHY THIS EXISTS (P0-4) =====================
 * The SPI-slave dispatch + reply build (protocol.c / transport_*.c) run
 * SYNCHRONOUS + FAST in the driver ISR/SwiP -- a blocking command's reply
 * must therefore ALWAYS be a fast read of in-memory state, never a direct
 * call into a blocking Wlan_* / psa_fwu_* body (those can take seconds).
 * This worker is that decoupling: the ISR SUBMITS a job and reads back its
 * cached result; the slow HAL body runs OUTSIDE the ISR, on the main
 * loop / bringup_task, via worker_run_pending() (the drain).  While the
 * drain blocks (Wi-Fi init can take seconds), the SPI ISR keeps answering
 * the host's poll re-issues from the shared state -- that is the point.
 *
 * Single in-flight job is sufficient for v0.2 (the META + Wi-Fi getters
 * the host polls one at a time).  The shared `state` lives in a `volatile`
 * struct so the ISR (poll) and the drain (run) observe each other's
 * writes; the few multi-field transitions are made under a short critical
 * section (see worker.c) so the ISR never reads a half-updated result.
 *
 * SILICON-FREE: this TU pulls in NO TI SDK.  The blocking bodies it calls
 * are the cc3501e_hw_* HAL shims (hal/cc3501e_hw.h); on the stub/native
 * backend those are NOTIMPL and the drain runs the job SYNCHRONOUSLY from
 * worker_submit() so host-side ztests stay deterministic.
 * =================================================================
 */

#ifndef CC3501E_BRIDGE_WORKER_H
#define CC3501E_BRIDGE_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "alp/protocol/cc3501e.h"

/* Lifecycle of the single in-flight job.  IDLE -> QUEUED (submit) ->
 * RUNNING -> DONE/ERR (drain), then back to IDLE once the host has polled
 * the result out (protocol.c resets it). */
enum worker_state {
	WORKER_IDLE = 0, /* no job; ready to accept a submit                 */
	WORKER_QUEUED,   /* submitted, awaiting the drain                    */
	WORKER_RUNNING,  /* drain is executing the blocking HAL body         */
	WORKER_DONE,     /* result ready (result[]/result_len valid)         */
	WORKER_ERR,      /* job failed; err carries the CC3501E_HW_* code    */
};

/* Initialise the worker to IDLE (called once from transport init / main). */
void worker_init(void);

/*
 * worker_submit -- queue a job for the drain.  @p cmd is the opcode the
 * job services (e.g. ALP_CC3501E_CMD_GET_MAC).  v0.2 jobs are argument-
 * free getters, so there are no args yet; future jobs (WIFI_CONNECT etc.)
 * will copy their args into a worker-owned buffer here.
 *
 * Returns 1 if the job was accepted (state IDLE -> QUEUED), 0 if the
 * worker was busy (a different/earlier job is still in flight).
 *
 * On the SILICON-FREE / stub backend (no CC3501E_WIFI) the job is run
 * SYNCHRONOUSLY here so the result is immediately available on the next
 * poll -- this keeps native_sim ztests deterministic with no main loop.
 */
int worker_submit(uint8_t cmd);

/*
 * worker_poll -- read back a completed job's result WITHOUT blocking.
 * Safe to call from the SPI ISR.  Only succeeds when a DONE/ERR job
 * matching @p cmd is present.
 *
 *   cmd      -- the opcode the caller expects the in-flight job to be.
 *   out      -- buffer for the result bytes (DONE only).
 *   out_cap  -- capacity of @p out.
 *   out_len  -- [out] bytes written to @p out.
 *   err      -- [out] the CC3501E_HW_* code (ERR only; 0 on DONE).
 *
 * Returns:
 *   WORKER_DONE  -- result copied into out/out_len (err = 0).  The caller
 *                   MUST then reset the worker (worker_reset) so the next
 *                   command can submit.
 *   WORKER_ERR   -- job failed; err set.  Caller resets too.
 *   WORKER_QUEUED/WORKER_RUNNING -- still in flight (caller replies BUSY).
 *   WORKER_IDLE  -- no job, or a job for a DIFFERENT cmd is in flight
 *                   (caller treats both as "submit/BUSY", see protocol.c).
 */
enum worker_state
worker_poll(uint8_t cmd, uint8_t *out, size_t out_cap, size_t *out_len, int8_t *err);

/* Return the worker to IDLE after the host has consumed a DONE/ERR result
 * (or to abandon a job).  Called from protocol.c once a GET_MAC poll has
 * copied the result into the reply. */
void worker_reset(void);

/*
 * worker_run_pending -- THE DRAIN.  Runs OUTSIDE the ISR, from main()'s
 * loop / bringup_task.  If a job is QUEUED it transitions it to RUNNING,
 * calls the (possibly blocking) HAL body, stores the result, and sets
 * DONE/ERR.  No-op when nothing is queued.  On the stub backend the job
 * has already completed synchronously in worker_submit(), so this is a
 * cheap no-op there.
 */
void worker_run_pending(void);

#endif /* CC3501E_BRIDGE_WORKER_H */
