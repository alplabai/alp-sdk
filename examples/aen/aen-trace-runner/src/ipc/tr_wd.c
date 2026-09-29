/* src/ipc/tr_wd.c -- see tr_wd.h (the rules are numbered there). */
#include "tr_wd.h"

static const char *const why_str[TR_WD_WHY_N] = {
	[TR_WD_NOT_ALIVE]     = "stub not alive yet",
	[TR_WD_PENDING]       = "previous command not consumed",
	[TR_WD_SETTLING]      = "PARKED at boot, waiting for the stub's self-LAUNCH",
	[TR_WD_BACKING_OFF]   = "not running, next LAUNCH not due (backoff)",
	[TR_WD_INITIALISING]  = "renderer initialising, first frame not due",
	[TR_WD_OK]            = "running",
	[TR_WD_PARKING]       = "HALT consumed, waiting to park",
	[TR_WD_LAUNCH_BOOT]   = "PARKED at boot, no self-LAUNCH",
	[TR_WD_LAUNCH_HALTED] = "parked by our HALT",
	[TR_WD_LAUNCH_FAULT]  = "stub recorded a fault",
	[TR_WD_LAUNCH_PARKED] = "renderer parked on its own",
	[TR_WD_HALT_FIRST]    = "no first frame by the first-frame timeout",
	[TR_WD_HALT_STALL]    = "stalled mid-game",
	[TR_WD_HALT_FOREIGN]  = "running a payload this HE does not LAUNCH",
};

const char *tr_wd_why_str(tr_wd_why_t why)
{
	return (unsigned)why < TR_WD_WHY_N ? why_str[why] : "?";
}

void tr_wd_init(tr_wd_t *w, uint64_t now_us)
{
	*w = (tr_wd_t){ .wait_us = now_us, .first = true, .last_cmd = TR_CTRL_NONE };
}

uint64_t tr_wd_deadline_us(const tr_wd_t *w)
{
	uint32_t shift = w->tries > 0u ? w->tries - 1u : 0u;

	if (shift > TR_WD_BACKOFF_MAX_SHIFT) shift = TR_WD_BACKOFF_MAX_SHIFT;
	return w->wait_us + ((uint64_t)TR_WD_FIRST_FRAME_US << shift);
}

uint64_t tr_wd_pending_us(const tr_wd_t *w, uint64_t now_us)
{
	return w->pending ? now_us - w->pending_us : 0u;
}

/* Rules 2 / 7: a command in flight; the clock starts at the first such poll. */
static uint32_t in_flight(tr_wd_t *w, uint64_t now_us, tr_wd_why_t reason, tr_wd_why_t *why)
{
	if (!w->pending) {
		w->pending    = true;
		w->pending_us = now_us;
	}
	*why = reason;
	return TR_CTRL_NONE;
}

bool tr_wd_landed(tr_wd_t *w, uint64_t now_us)
{
	bool first = w->first;

	w->first = false;
	if (!w->running) {
		w->running  = true;
		w->since_us = now_us;
	} else if (now_us - w->since_us >= TR_WD_WINDOW_US) {
		w->tries    = 0; /* healthy for a window: the next failure starts the backoff over */
		w->since_us = now_us;
	}
	return first;
}

static uint32_t send(tr_wd_t *w, uint32_t cmd, uint64_t now_us, tr_wd_why_t why, tr_wd_why_t *out)
{
	*out = why;
	w->sent++;
	w->last_cmd = cmd;
	w->halting  = cmd == TR_CTRL_HALT;
	if (cmd == TR_CTRL_LAUNCH) {
		w->tries++;
		w->first   = true;
		w->wait_us = now_us;
	}
	return cmd;
}

uint32_t tr_wd_poll(tr_wd_t     *w,
                    bool         alive,
                    uint32_t     stub_state,
                    uint32_t     ctrl_cmd,
                    bool         ours,
                    bool         missed,
                    uint64_t     now_us,
                    tr_wd_why_t *why)
{
	if (missed) w->running = false;
	/* Rule 1. */
	if (!alive || (stub_state != TR_STUB_PARKED && stub_state != TR_STUB_RUNNING &&
	               stub_state != TR_STUB_FAULT)) {
		w->pending = false;
		*why       = TR_WD_NOT_ALIVE;
		return TR_CTRL_NONE;
	}
	if (!w->alive) {
		w->alive   = true;
		w->wait_us = now_us; /* the clocks start when the stub is first seen */
	}
	if (stub_state != TR_STUB_RUNNING)
		w->halting = false; /* parked (or faulted): any HALT is done with */
	/* Rule 2. */
	if (ctrl_cmd != TR_CTRL_NONE &&
	    !(ctrl_cmd == TR_CTRL_LAUNCH && stub_state == TR_STUB_RUNNING)) {
		if (ctrl_cmd == TR_CTRL_HALT) w->halting = true; /* ours or a previous HE boot's */
		return in_flight(w, now_us, TR_WD_PENDING, why);
	}
	/* Rule 7. */
	if (w->halting) return in_flight(w, now_us, TR_WD_PARKING, why);
	w->pending = false;
	/* Rule 3. */
	if (stub_state != TR_STUB_RUNNING) {
		if (w->last_cmd == TR_CTRL_HALT)
			return send(w, TR_CTRL_LAUNCH, now_us, TR_WD_LAUNCH_HALTED, why);
		if (w->sent == 0u) {
			if (stub_state == TR_STUB_FAULT)
				return send(w, TR_CTRL_LAUNCH, now_us, TR_WD_LAUNCH_FAULT, why);
			if (now_us - w->wait_us >= TR_WD_SETTLE_US)
				return send(w, TR_CTRL_LAUNCH, now_us, TR_WD_LAUNCH_BOOT, why);
			*why = TR_WD_SETTLING;
			return TR_CTRL_NONE;
		}
		if (now_us >= tr_wd_deadline_us(w))
			return send(w,
			            TR_CTRL_LAUNCH,
			            now_us,
			            stub_state == TR_STUB_FAULT ? TR_WD_LAUNCH_FAULT : TR_WD_LAUNCH_PARKED,
			            why);
		*why = TR_WD_BACKING_OFF;
		return TR_CTRL_NONE;
	}
	/* Rule 6. */
	if (!ours) return send(w, TR_CTRL_HALT, now_us, TR_WD_HALT_FOREIGN, why);
	/* Rule 4. */
	if (w->first) {
		if (now_us >= tr_wd_deadline_us(w))
			return send(w, TR_CTRL_HALT, now_us, TR_WD_HALT_FIRST, why);
		*why = TR_WD_INITIALISING;
		return TR_CTRL_NONE;
	}
	/* Rule 5. */
	if (missed) return send(w, TR_CTRL_HALT, now_us, TR_WD_HALT_STALL, why);
	*why = TR_WD_OK;
	return TR_CTRL_NONE;
}
