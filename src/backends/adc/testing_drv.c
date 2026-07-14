/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC test double.  Ordinary alp_adc_ops_t backend registered at
 * the reserved test-double priority (255, silicon_ref="*"), so with
 * CONFIG_ALP_SDK_TESTING_ADC=y it outranks every real/proxy/fallback
 * ADC backend and alp_adc_open() rides on it transparently -- see
 * the priority note on ALP_BACKEND_REGISTER in <alp/backend.h>.
 *
 * open()     -- ALWAYS succeeds.  Same deliberate ergonomic choice as
 *                the GPIO/UART doubles (src/backends/gpio/testing_drv.c,
 *                src/backends/uart/testing_drv.c): a "*" silicon_ref
 *                double has no valid-channel-range knowledge to reject
 *                a channel_id with, and the injection API
 *                (<alp/testing/adc.h>) needs inject-before-open to
 *                work.  ADC-SPECIFIC MUST (issue #610 design note):
 *                open() sets state->reference_uv / state->resolution_bits
 *                from cfg (falling back to a sane default -- 3.3 V /
 *                12-bit -- when cfg leaves resolution_bits at 0, its
 *                "use the backend default" sentinel; see
 *                <alp/adc.h>) so src/adc_dispatch.c's
 *                alp_adc_read_uv() raw -> uV conversion is exercised
 *                with a non-zero full-scale divisor instead of always
 *                failing ALP_ERR_NOT_READY.
 * read_raw() -- pops the next queued raw sample (FIFO order); once
 *                the FIFO drains, keeps returning the last popped
 *                value (a held ADC input) -- see the latch model on
 *                <alp/testing/adc.h>.  A pending alp_testing_adc_fail_next()
 *                pre-empts the pop and returns the injected status
 *                instead, consuming itself (single-shot).
 * close()    -- detaches the handle from its slot (be_data = NULL) so
 *                a stale struct alp_adc cannot reach freed backend
 *                state; the channel's queued/latched/fault state
 *                itself is NOT cleared by close() (mirrors the
 *                GPIO/UART doubles leaving their side-state intact
 *                across a close/re-open on the same id) -- only
 *                alp_testing_reset_all() clears it.
 *
 * The injection API (<alp/testing/adc.h>) and the ops table share the
 * same per-channel slot via the generic instance table
 * (src/testing/instance_table.h), keyed by channel_id -- exactly the
 * id the app passes to alp_adc_open() -- so a test can queue raw
 * samples before the app ever opens the channel.
 *
 * @par Cost: ROM ~1 KB; RAM = capacity * sizeof(slot) (test-only,
 *      never linked into a production image -- gated by
 *      CONFIG_ALP_SDK_TESTING, itself gated on CONFIG_ZTEST).
 * @par Performance: O(capacity) per call (linear slot scan) plus
 *      O(1) FIFO push/pop (fixed ring buffer); fine for the handful
 *      of channels and samples a unit test ever touches.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/testing/adc.h>

#include "adc_ops.h"
#include "instance_table.h"
#include "reset_registry.h"

#ifndef ALP_TESTING_ADC_MAX_CHANNELS
#define ALP_TESTING_ADC_MAX_CHANNELS 8
#endif

#ifndef ALP_TESTING_ADC_MAX_QUEUE
#define ALP_TESTING_ADC_MAX_QUEUE 8
#endif

/* Sane defaults applied at open() when cfg leaves the corresponding
 * field at its "use the backend default" sentinel -- see the
 * ADC-specific must in this file's header comment and on
 * <alp/testing/adc.h>. */
#ifndef ALP_TESTING_ADC_DEFAULT_REFERENCE_UV
#define ALP_TESTING_ADC_DEFAULT_REFERENCE_UV 3300000u /* 3.3 V */
#endif

#ifndef ALP_TESTING_ADC_DEFAULT_RESOLUTION_BITS
#define ALP_TESTING_ADC_DEFAULT_RESOLUTION_BITS 12u
#endif

typedef struct {
	int32_t      fifo[ALP_TESTING_ADC_MAX_QUEUE];
	size_t       head;
	size_t       count;
	int32_t      last_raw; /* latched value once the FIFO drains */
	bool         fail_pending;
	alp_status_t fail_err;
} alp_testing_adc_slot_t;

static alp_testing_adc_slot_t       g_slots[ALP_TESTING_ADC_MAX_CHANNELS];
static alp_testing_slot_hdr_t       g_hdrs[ALP_TESTING_ADC_MAX_CHANNELS];
static alp_testing_instance_table_t g_table;
static bool                         g_table_ready;

/* Every field's zero value IS the correct "never touched" state
 * (empty FIFO, 0 latched raw, no pending fault) -- the instance
 * table's memset already produces it, so this has nothing left to
 * set.  Kept (rather than passed as NULL) to mirror the GPIO/UART
 * doubles' explicit-reset style. */
static void slot_reset(void *slot_v)
{
	(void)slot_v;
}

/* Reset hook: the channel's FIFO + latch + pending fault live
 * entirely inside this table, so alp_testing_instance_table_reset_all()
 * (run unconditionally by alp_testing_reset_all()) already clears
 * them on its own -- this hook is therefore a defensive, explicit
 * re-assertion of that contract rather than a functional necessity
 * (same reasoning as the UART double's uart_reset_hook), so a future
 * change to this double's state shape that DOES step outside the
 * table has somewhere to go without a silent reset gap. */
static void adc_reset_hook(void)
{
	alp_testing_instance_table_reset(&g_table);
}

static alp_testing_instance_table_t *table(void)
{
	if (!g_table_ready) {
		alp_testing_instance_table_init(&g_table,
		                                g_slots,
		                                g_hdrs,
		                                sizeof(g_slots[0]),
		                                ALP_TESTING_ADC_MAX_CHANNELS,
		                                slot_reset);
		alp_testing_register_reset_hook(adc_reset_hook);
		g_table_ready = true;
	}
	return &g_table;
}

alp_status_t alp_testing_adc_queue_raw(uint32_t channel_id, const int32_t *raw, size_t n)
{
	if (raw == NULL && n > 0) return ALP_ERR_INVAL;

	alp_testing_adc_slot_t *slot = alp_testing_instance_table_touch(table(), channel_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	/* All-or-nothing: reject the whole call rather than queue a
	 * partial sequence a test would have to reason about. */
	if (n > ALP_TESTING_ADC_MAX_QUEUE - slot->count) return ALP_ERR_NOMEM;

	for (size_t i = 0; i < n; ++i) {
		size_t idx      = (slot->head + slot->count) % ALP_TESTING_ADC_MAX_QUEUE;
		slot->fifo[idx] = raw[i];
		slot->count++;
	}
	return ALP_OK;
}

alp_status_t alp_testing_adc_fail_next(uint32_t channel_id, alp_status_t err)
{
	alp_testing_adc_slot_t *slot = alp_testing_instance_table_touch(table(), channel_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	slot->fail_pending = true;
	slot->fail_err     = err;
	return ALP_OK;
}

/* ---- alp_adc_ops_t ---- */

static alp_status_t
t_open(const alp_adc_config_t *cfg, alp_adc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	alp_testing_adc_slot_t *slot = alp_testing_instance_table_touch(table(), cfg->channel_id);
	if (slot == NULL) return ALP_ERR_NOMEM;

	st->be_data = slot;
	/* ADC-specific must (see file header): a real reference_uv +
	 * resolution_bits, not zero, so the dispatcher's raw -> uV
	 * conversion (src/adc_dispatch.c) has a non-zero full-scale
	 * divisor to divide by. */
	st->resolution_bits = (cfg->resolution_bits != 0)
	                          ? cfg->resolution_bits
	                          : (uint16_t)ALP_TESTING_ADC_DEFAULT_RESOLUTION_BITS;
	st->reference_uv    = (uint32_t)ALP_TESTING_ADC_DEFAULT_REFERENCE_UV;

	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t t_read_raw(alp_adc_backend_state_t *st, int32_t *raw_out)
{
	alp_testing_adc_slot_t *slot = (alp_testing_adc_slot_t *)st->be_data;
	if (slot == NULL) return ALP_ERR_NOT_READY;

	if (slot->fail_pending) {
		slot->fail_pending = false;
		return slot->fail_err;
	}

	if (slot->count > 0) {
		slot->last_raw = slot->fifo[slot->head];
		slot->head     = (slot->head + 1) % ALP_TESTING_ADC_MAX_QUEUE;
		slot->count--;
	}
	*raw_out = slot->last_raw;
	return ALP_OK;
}

static void t_close(alp_adc_backend_state_t *st)
{
	st->be_data = NULL;
}

static const alp_adc_ops_t _ops = {
	.open     = t_open,
	.read_raw = t_read_raw,
	.close    = t_close,
};

ALP_BACKEND_REGISTER(adc,
                     alp_testing,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp_testing",
                         .base_caps   = 0u,
                         .priority    = 255,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
