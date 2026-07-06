/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: async-event ring buffer (the EVT_* queue).
 *
 * ===================== WHY THIS EXISTS (task #17) =====================
 * The firmware learns of asynchronous events (Wi-Fi connect/disconnect, BLE
 * connection, a GPIO edge) on its radio/host-driver threads, but there is NO
 * slave->master attention line on this HW rev (the CC35 GPIO17 -> Alif P2_6
 * wire is a BODGE not routed on the stock EVK), so it cannot PUSH them to the
 * host.  Instead the host POLLS: it periodically issues
 * CMD_GET_PENDING_EVENTS, and handle_get_pending_events() DRAINS this ring
 * into the reply.
 *
 * The ring decouples the event PRODUCERS (thread context: the Wi-Fi event
 * callback / connect body, BLE, GPIO) from the CONSUMER (the SPI-ISR dispatch
 * that runs handle_get_pending_events).  Both push and drain are IRQ-safe --
 * short critical sections (the same worker_critical_enter/exit the worker uses,
 * a no-op on native / __disable_irq on the ti backend), so the ISR can drain
 * inline while a producer thread pushes.  The drain is a bounded memcpy, safe
 * to run in the ISR (unlike the seconds-long radio ops, which the worker
 * routes off-ISR).
 *
 * SILICON-FREE: this TU pulls in NO TI SDK; it is compiled into every backend
 * (stub + ti) and the native_sim transport test.
 * =====================================================================
 */

#ifndef CC3501E_BRIDGE_EVENT_RING_H
#define CC3501E_BRIDGE_EVENT_RING_H

#include <stddef.h>
#include <stdint.h>

/* Ring depth (number of queued events) and the per-entry payload cap.  16 slots
 * absorb a burst of connect/disconnect/GPIO events between two host polls; 16
 * payload bytes cover the largest EVT_* payload the firmware enqueues (the
 * 8-byte alp_cc3501e_gpio_event_t, with headroom).  Overflow drops the NEWEST
 * event (the ring never blocks a producer). */
#define CC3501E_EVENT_RING_SLOTS  16u
#define CC3501E_EVENT_PAYLOAD_MAX 16u

/* Initialise the ring to empty (called once from main() / worker_init). */
void event_ring_init(void);

/*
 * event_ring_push -- enqueue one async event.  IRQ-safe (short critical
 * section); callable from a producer thread OR an ISR.
 *
 *   evt_opcode -- the async opcode (one of ALP_CC3501E_EVT_*).
 *   payload    -- event payload bytes (may be NULL when len == 0).
 *   len        -- payload length, clamped to CC3501E_EVENT_PAYLOAD_MAX.
 *
 * Returns 1 if the event was queued, 0 if the ring was full (the event is
 * dropped -- the host will still learn the latest state on its next poll, e.g.
 * via CMD_WIFI_STATUS).
 */
int event_ring_push(uint8_t evt_opcode, const uint8_t *payload, uint8_t len);

/*
 * event_ring_drain -- pop queued events into @p out in the wire format
 * { evt_opcode(1) | len(1) | payload[len] } per entry, packed with no padding.
 * IRQ-safe; run inline from the SPI-ISR dispatch (handle_get_pending_events).
 *
 * Packs WHOLE entries only: an entry that would not fit in @p cap is LEFT in
 * the ring for the next drain (never split).  Returns the number of bytes
 * written to @p out (0 when the ring is empty or the first entry does not fit).
 */
size_t event_ring_drain(uint8_t *out, size_t cap);

#endif /* CC3501E_BRIDGE_EVENT_RING_H */
