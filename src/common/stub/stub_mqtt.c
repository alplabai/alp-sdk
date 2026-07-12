/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * MQTT NOSUPPORT stubs -- <alp/iot.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_mqtt_*` symbol not provided by a vendor backend.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/iot.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_MQTT)
alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
alp_status_t alp_mqtt_connect(alp_mqtt_t *m, uint32_t t)
{
	(void)m;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_mqtt_publish(alp_mqtt_t *m, const char *t, const uint8_t *p, size_t l, alp_mqtt_qos_t q, bool r)
{
	(void)m;
	(void)t;
	(void)p;
	(void)l;
	(void)q;
	(void)r;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_mqtt_subscribe(alp_mqtt_t *m, const char *f, alp_mqtt_qos_t q, alp_mqtt_msg_cb_t cb, void *u)
{
	(void)m;
	(void)f;
	(void)q;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_mqtt_loop(alp_mqtt_t *m, uint32_t t)
{
	(void)m;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_mqtt_close(alp_mqtt_t *m)
{
	(void)m;
}
const alp_capabilities_t *alp_mqtt_capabilities(const alp_mqtt_t *m)
{
	(void)m;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_MQTT */
