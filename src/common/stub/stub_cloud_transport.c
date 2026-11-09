/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cloud-transport NOSUPPORT stubs -- <alp/cloud_transport.h>.  The real
 * implementation (src/cloud_transport.c) is Zephyr sockets + mbedtls; the
 * baremetal and Yocto plain-CMake libraries have no backend for it yet, so
 * they export every entry point and report the gap instead of failing to link.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/cloud_transport.h"
#include "alp/peripheral.h"

alp_status_t alp_cloud_transport_connect(alp_cloud_transport_t              *t,
                                         const alp_cloud_transport_config_t *cfg)
{
	(void)t;
	(void)cfg;
	return ALP_ERR_NOSUPPORT;
}

int32_t alp_cloud_transport_send(alp_cloud_transport_t *t, const void *buf, size_t len)
{
	(void)t;
	(void)buf;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

int32_t alp_cloud_transport_recv(alp_cloud_transport_t *t, void *buf, size_t len)
{
	(void)t;
	(void)buf;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

void alp_cloud_transport_close(alp_cloud_transport_t *t)
{
	(void)t;
}
