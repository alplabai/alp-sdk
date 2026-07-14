/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN NOSUPPORT stubs -- <alp/can.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_can_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/can.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_CAN)
alp_can_t *alp_can_open(const alp_can_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_can_start(alp_can_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_stop(alp_can_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_send(alp_can_t *c, const alp_can_frame_t *f, uint32_t t)
{
	(void)c;
	(void)f;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_add_filter(alp_can_t              *c,
                                const alp_can_filter_t *f,
                                alp_can_rx_cb_t         cb,
                                void                   *u,
                                int32_t                *id)
{
	(void)c;
	(void)f;
	(void)cb;
	(void)u;
	(void)id;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_can_remove_filter(alp_can_t *c, int32_t id)
{
	(void)c;
	(void)id;
	return ALP_ERR_NOSUPPORT;
}
void alp_can_close(alp_can_t *c)
{
	(void)c;
}
const alp_capabilities_t *alp_can_capabilities(const alp_can_t *c)
{
	(void)c;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_CAN */
