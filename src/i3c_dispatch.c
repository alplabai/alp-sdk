/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I3C class dispatcher.  Owns the public alp_i3c_* API surface
 * and routes through the backend registry mechanism shipped in
 * Slice 0 (PR #17).
 *
 * The handle struct layout (struct alp_i3c) lives in
 * src/backends/i3c/i3c_ops.h so the backend .c files can reach
 * the fields directly without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i3c.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/i3c/i3c_ops.h"

ALP_BACKEND_DEFINE_CLASS(i3c);
ALP_BACKEND_ANCHOR(i3c);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_I3C_HANDLES
#define CONFIG_ALP_SDK_MAX_I3C_HANDLES 1
#endif

static struct alp_i3c _pool[CONFIG_ALP_SDK_MAX_I3C_HANDLES];

static struct alp_i3c *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_I3C_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_i3c, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_i3c *h)
{
	alp_slot_release(&h->in_use);
}

alp_i3c_t *alp_i3c_open(const alp_i3c_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	/* SoC capability gate: reject an out-of-range bus before any
	 * backend dispatch.  Under CONFIG_ALP_SOC_NONE the no-SoC branch of
	 * soc_caps.h sets ALP_SOC_I3C_COUNT to UINT16_MAX, so the gate still
	 * runs but never rejects -- a valid-but-unresolved bus then surfaces
	 * NOT_READY from the backend open() instead.  The `> 0` term is for a
	 * SoC that genuinely has no I3C: there the gate rejects every bus_id.
	 * (Mirrors the DAC/ADC dispatch capability gate.) */
	if ((ALP_SOC_I3C_COUNT > 0) && (uint32_t)cfg->bus_id >= (uint32_t)ALP_SOC_I3C_COUNT) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}

	const alp_backend_t *be = alp_backend_select("i3c", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_i3c_ops_t *ops = (const alp_i3c_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_i3c *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->bus_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

/* Gate on the lifecycle byte via alp_handle_op_enter(), not a plain
 * in_use read (issue #629): in_use is claimed/released atomically in
 * _alloc/_free, but a plain load here would let a racing alp_i3c_close()
 * free the slot while an op is still dereferencing bus->state. */

alp_status_t alp_i3c_write(alp_i3c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
	if (bus == NULL || !alp_handle_op_enter(&bus->lifecycle, &bus->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (data == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (bus->state.ops->write == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = bus->state.ops->write(&bus->state, addr, data, len);
	}
	alp_handle_op_leave(&bus->active_ops);
	return rc;
}

alp_status_t alp_i3c_read(alp_i3c_t *bus, uint8_t addr, uint8_t *data, size_t len)
{
	if (bus == NULL || !alp_handle_op_enter(&bus->lifecycle, &bus->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (data == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (bus->state.ops->read == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = bus->state.ops->read(&bus->state, addr, data, len);
	}
	alp_handle_op_leave(&bus->active_ops);
	return rc;
}

alp_status_t alp_i3c_write_read(alp_i3c_t     *bus,
                                uint8_t        addr,
                                const uint8_t *wdata,
                                size_t         wlen,
                                uint8_t       *rdata,
                                size_t         rlen)
{
	if (bus == NULL || !alp_handle_op_enter(&bus->lifecycle, &bus->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if ((wdata == NULL && wlen > 0) || (rdata == NULL && rlen > 0)) {
		rc = ALP_ERR_INVAL;
	} else if (bus->state.ops->write_read == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = bus->state.ops->write_read(&bus->state, addr, wdata, wlen, rdata, rlen);
	}
	alp_handle_op_leave(&bus->active_ops);
	return rc;
}

void alp_i3c_close(alp_i3c_t *bus)
{
	if (bus == NULL) return;
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops -- makes "close races a blocked/in-flight op" a
	 * bounded wait instead of a use-after-free (issue #629).  Losing
	 * the CAS (already closed/closing/never-opened) makes this a
	 * no-op, matching the existing void-close idempotency contract. */
	if (!alp_handle_begin_close_blocking(&bus->lifecycle, &bus->active_ops)) return;
	if (bus->state.ops != NULL && bus->state.ops->close != NULL) {
		bus->state.ops->close(&bus->state);
	}
	alp_lifecycle_set(&bus->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(bus);
}

const alp_capabilities_t *alp_i3c_capabilities(const alp_i3c_t *bus)
{
	return (bus != NULL) ? &bus->cached_caps : NULL;
}
