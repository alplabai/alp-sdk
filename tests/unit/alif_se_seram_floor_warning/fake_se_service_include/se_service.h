/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-only stand-in for hal_alif's <se_service.h>, used ONLY by this
 * test directory to compile the REAL src/backends/soc_info/alif_se.c
 * translation unit on native_sim, where the real header (and the SE
 * hardware it talks to) does not exist. Same technique as
 * tests/unit/se_cryptocell_hash_bounds/fake_se_service_include/se_service.h.
 *
 * Supplies exactly the symbols alif_se.c references:
 *   - VERSION_RESPONSE_LENGTH -- alif_se.c's own comment ("up to
 *     VERSION_RESPONSE_LENGTH = 80 bytes") states the value; not
 *     invented here, just carried over.
 *   - get_device_revision_data_t -- field NAMES and USAGE
 *     (revision_id, LCS, SerialN[8]) mirror alif_se.c's own
 *     references to the hal_alif wire struct; exact widths are this
 *     test's choice, not a vendor ABI claim -- this test never links
 *     against the real struct.
 *   - se_service_get_se_revision / _get_device_part_number /
 *     _system_get_device_data / _heartbeat -- defined as controllable
 *     stubs in src/test_alif_se_seram_floor.c.
 */

#ifndef ALP_TEST_FAKE_SE_SERVICE_H
#define ALP_TEST_FAKE_SE_SERVICE_H

#include <stdint.h>

#define VERSION_RESPONSE_LENGTH 80u

typedef struct {
	uint32_t         revision_id;
	uint32_t         LCS;
	volatile uint8_t SerialN[8];
} get_device_revision_data_t;

int se_service_get_se_revision(uint8_t *rev);
int se_service_get_device_part_number(uint32_t *out);
int se_service_system_get_device_data(get_device_revision_data_t *out);
int se_service_heartbeat(void);

#endif /* ALP_TEST_FAKE_SE_SERVICE_H */
