/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software I3C fallback backend.  Registered (priority 0, "*") only
 * to keep the i3c class section non-empty for the registry's
 * __start_/__stop_ linker bounds.  zephyr_drv (priority 100, "*")
 * compiles unconditionally and always wins the backend match on
 * native_sim, so this backend's open() is never actually reached
 * through alp_i3c_open() there -- it only runs if a caller picks it
 * directly via the registry.  Its open() itself still succeeds with
 * an empty capability set, but write / read / write_read return
 * ALP_ERR_NOSUPPORT -- there is no bus to drive.
 *
 * @par Cost: ROM ~300 B, RAM 0 B (stateless; no device, no buffer).
 * @par Performance: O(1) per call; every op short-circuits to
 *      NOSUPPORT.  For native_sim build/test only -- never use on
 *      production hardware.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i3c.h>
#include <alp/peripheral.h>

#include "i3c_ops.h"

static alp_status_t
sw_open(const alp_i3c_config_t *cfg, alp_i3c_backend_state_t *st, alp_capabilities_t *caps_out)
{
	st->dev         = NULL;
	st->bus_id      = cfg->bus_id;
	st->be_data     = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
sw_write(alp_i3c_backend_state_t *st, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)st;
	(void)addr;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_read(alp_i3c_backend_state_t *st, uint8_t addr, uint8_t *data, size_t len)
{
	(void)st;
	(void)addr;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_write_read(alp_i3c_backend_state_t *st,
                                  uint8_t                  addr,
                                  const uint8_t           *wdata,
                                  size_t                   wlen,
                                  uint8_t                 *rdata,
                                  size_t                   rlen)
{
	(void)st;
	(void)addr;
	(void)wdata;
	(void)wlen;
	(void)rdata;
	(void)rlen;
	return ALP_ERR_NOSUPPORT;
}

static const alp_i3c_ops_t sw_ops = {
	.open       = sw_open,
	.write      = sw_write,
	.read       = sw_read,
	.write_read = sw_write_read,
	.close      = NULL,
};

ALP_BACKEND_ANCHOR_DEFINE(i3c);
ALP_BACKEND_REGISTER(i3c,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &sw_ops,
                         .probe       = NULL,
                     });
