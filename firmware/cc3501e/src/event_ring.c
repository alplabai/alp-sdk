/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: async-event ring buffer (see event_ring.h).
 *
 * A fixed-slot FIFO of async events (opcode + short payload) that producer
 * threads push into and the SPI-ISR dispatch drains on CMD_GET_PENDING_EVENTS.
 * Concurrency mirrors src/worker.c: the head/tail/count and the slot bytes are
 * touched only inside a short critical section (worker_critical_enter/exit --
 * a no-op on the native build, __disable_irq/restore on the ti backend), so the
 * ISR draining and a thread pushing never observe a half-updated ring.
 */

#include <string.h>

#include "event_ring.h"
#include "alp/protocol/cc3501e.h"

/* Shared with src/worker.c: the ISR-vs-thread critical-section primitives.
 * Weakly defined in worker.c (no-op) and overridden by the ti backend to mask
 * interrupts, so the event ring uses the SAME masking as the worker. */
unsigned long worker_critical_enter(void);
void          worker_critical_exit(unsigned long key);

/* One queued event: opcode + its (already length-clamped) payload. */
struct event_slot {
	uint8_t evt_opcode;
	uint8_t len;
	uint8_t payload[CC3501E_EVENT_PAYLOAD_MAX];
};

/* Single-producer-agnostic FIFO: a classic head/tail ring guarded by the
 * critical section (multiple producer threads are possible -- Wi-Fi + BLE +
 * GPIO -- so this is NOT lock-free SPSC; the short mask keeps it correct). */
static struct {
	struct event_slot slot[CC3501E_EVENT_RING_SLOTS];
	uint16_t          head;  /* next slot to drain (pop)  */
	uint16_t          tail;  /* next slot to fill (push)  */
	uint16_t          count; /* queued entries in [0..SLOTS] */
} ring;

void event_ring_init(void)
{
	const unsigned long key = worker_critical_enter();
	ring.head               = 0u;
	ring.tail               = 0u;
	ring.count              = 0u;
	worker_critical_exit(key);
}

int event_ring_push(uint8_t evt_opcode, const uint8_t *payload, uint8_t len)
{
	if (len > CC3501E_EVENT_PAYLOAD_MAX) {
		len = CC3501E_EVENT_PAYLOAD_MAX; /* clamp -- never overflow a slot */
	}

	const unsigned long key = worker_critical_enter();
	if (ring.count >= CC3501E_EVENT_RING_SLOTS) {
		worker_critical_exit(key);
		return 0; /* full -- drop; the host re-reads the latest state on its next poll */
	}
	struct event_slot *s = &ring.slot[ring.tail];
	s->evt_opcode        = evt_opcode;
	s->len               = len;
	if (len > 0u && payload != NULL) {
		memcpy(s->payload, payload, len);
	}
	ring.tail = (uint16_t)((ring.tail + 1u) % CC3501E_EVENT_RING_SLOTS);
	ring.count++;
	worker_critical_exit(key);
	return 1;
}

size_t event_ring_drain(uint8_t *out, size_t cap)
{
	size_t written = 0u;
	if (out == NULL) {
		return 0u;
	}

	for (;;) {
		const unsigned long key = worker_critical_enter();
		if (ring.count == 0u) {
			worker_critical_exit(key);
			break; /* ring empty */
		}
		const struct event_slot *s    = &ring.slot[ring.head];
		const size_t             need = (size_t)ALP_CC3501E_EVENT_HDR_BYTES + (size_t)s->len;
		if (written + need > cap) {
			/* WHOLE entries only: this one does not fit -- leave it (and the rest)
			 * queued for the next poll.  Do NOT split a payload across replies. */
			worker_critical_exit(key);
			break;
		}
		out[written]      = s->evt_opcode;
		out[written + 1u] = s->len;
		if (s->len > 0u) {
			memcpy(&out[written + ALP_CC3501E_EVENT_HDR_BYTES], s->payload, s->len);
		}
		written += need;
		ring.head = (uint16_t)((ring.head + 1u) % CC3501E_EVENT_RING_SLOTS);
		ring.count--;
		worker_critical_exit(key);
	}
	return written;
}
