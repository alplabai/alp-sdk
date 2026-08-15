/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Model reader (alp/model.h) -- .alpmodel container parser NOSUPPORT
 * stub.  Split out of the former src/common/stub_backend.c monolith
 * (issue #673).
 *
 * The real body (src/common/alp_model.c) decodes the CBOR manifest via
 * zcbor. zcbor is a Zephyr west module (see west.yml) AND, since
 * #1254, a plain-CMake Yocto dependency too -- meta-alp-sdk's own
 * recipe (recipes-devtools/zcbor/zcbor_0.9.1.bb) vendors it for that
 * OS, so src/yocto/CMakeLists.txt compiles the real alp_model.c in
 * place of this stub whenever its zcbor find_path/find_library
 * succeeds. baremetal still has no plain-CMake zcbor vendoring at all
 * (src/baremetal/CMakeLists.txt stubs unconditionally), and Yocto
 * itself falls back to this stub on a sysroot with no zcbor (see that
 * file's own ALP_SDK_MODEL_ZCBOR_REQUIRED two-mode comment). This is
 * an explicit, documented stub (issue #593), not an oversight: an
 * app that calls alp_model_parse directly on one of those legs gets
 * ALP_ERR_NOSUPPORT. alp_inference_open_alpmodel
 * (src/common/alp_model_loader.c) is OS-agnostic and already degrades
 * to its own NOSUPPORT body when CONFIG_ALP_SDK_MODEL_READER is
 * unset, so it's compiled for real (not stubbed) on every OS -- see
 * src/baremetal/CMakeLists.txt / src/yocto/CMakeLists.txt.
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
