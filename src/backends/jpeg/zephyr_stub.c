/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * JPEG stub backend.  Wildcard ("*") registration at priority 0:
 * picks up every silicon_ref the build targets so apps that
 * #include <alp/jpeg.h> link cleanly on every supported SoC.
 *
 * open()/close() succeed (and open() fills a zeroed caps struct) so
 * apps can query capabilities against a real handle; encode() always
 * returns ALP_ERR_NOT_IMPLEMENTED -- the dispatcher propagates that
 * out of the public alp_jpeg_encode() call.  The software baseline
 * (priority 50) and Alif Hantro VC9000E (priority 100, "alif:ensemble:e8")
 * backends outrank this stub once they land.
 */

#include <alp/backend.h>
#include <alp/jpeg.h>

#include "jpeg_ops.h"

static alp_status_t
stub_open(const alp_jpeg_config_t *cfg, alp_jpeg_backend_state_t *state, alp_jpeg_caps_t *caps_out)
{
	(void)cfg;
	(void)state;
	*caps_out = (alp_jpeg_caps_t){ 0 };
	return ALP_OK;
}

static alp_status_t stub_encode(alp_jpeg_backend_state_t    *state,
                                const alp_jpeg_encode_req_t *req,
                                void                        *out_buf,
                                size_t                       out_cap,
                                size_t                      *out_len)
{
	(void)state;
	(void)req;
	(void)out_buf;
	(void)out_cap;
	(void)out_len;
	return ALP_ERR_NOT_IMPLEMENTED;
}

static void stub_close(alp_jpeg_backend_state_t *state)
{
	(void)state;
}

static const alp_jpeg_ops_t _ops = {
	.open   = stub_open,
	.encode = stub_encode,
	.close  = stub_close,
};

ALP_BACKEND_ANCHOR_DEFINE(jpeg);
ALP_BACKEND_REGISTER(jpeg,
                     zephyr_stub,
                     {
                         .silicon_ref = "*",
                         .vendor      = "alp",
                         .base_caps   = 0u,
                         .priority    = 0u,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
