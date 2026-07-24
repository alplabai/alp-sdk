/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * JPEG-encoder NOSUPPORT stubs -- <alp/jpeg.h>.  Native/host (Yocto,
 * baremetal) build's provider of every `alp_jpeg_*` symbol; the
 * Zephyr build instead resolves these through the backend-registry
 * dispatcher (src/jpeg_dispatch.c + src/backends/jpeg/zephyr_stub.c).
 */

#include <stddef.h>

#include "alp/jpeg.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_JPEG)
alp_jpeg_t *alp_jpeg_open(const alp_jpeg_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_jpeg_encode(alp_jpeg_t                  *h,
                             const alp_jpeg_encode_req_t *req,
                             void                        *out_buf,
                             size_t                       out_cap,
                             size_t                      *out_len)
{
	(void)h;
	(void)req;
	(void)out_buf;
	(void)out_cap;
	(void)out_len;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_jpeg_capabilities(const alp_jpeg_t *h, alp_jpeg_caps_t *out)
{
	if (h == NULL || out == NULL) {
		return ALP_ERR_INVAL;
	}
	*out = (alp_jpeg_caps_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
void alp_jpeg_close(alp_jpeg_t *h)
{
	(void)h;
}
#endif /* !ALP_VENDOR_OVERRIDES_JPEG */
