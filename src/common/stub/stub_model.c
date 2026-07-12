/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model reader (alp/model.h) -- .alpmodel container parser NOSUPPORT
 * stub.  Split out of the former src/common/stub_backend.c monolith
 * (issue #673).
 *
 * The real body (src/common/alp_model.c) decodes the CBOR manifest
 * via zcbor, a Zephyr-only west module today (see west.yml) -- no
 * plain-CMake (baremetal/yocto) build vendors it, so alp_model_parse
 * has no non-Zephyr implementation yet.  This is an explicit,
 * documented stub (issue #593), not an oversight: baremetal/yocto
 * apps that call alp_model_parse directly get ALP_ERR_NOSUPPORT.
 * alp_inference_open_alpmodel (src/common/alp_model_loader.c) is
 * OS-agnostic and already degrades to its own NOSUPPORT body when
 * CONFIG_ALP_SDK_MODEL_READER is unset, so it's compiled for real
 * (not stubbed) on every OS -- see src/baremetal/CMakeLists.txt /
 * src/yocto/CMakeLists.txt.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/model.h"
#include "alp/peripheral.h"

alp_status_t alp_model_parse(const uint8_t *data, size_t size, alp_model_t *out)
{
	(void)data;
	(void)size;
	(void)out;
	return ALP_ERR_NOSUPPORT;
}
