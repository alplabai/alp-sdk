/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-m1-deepx-inference -- bring up the DEEPX DX-M1 NPU on a
 * V2N-M1 SoM and run a single inference through <alp/inference.h>.
 *
 * What this example shows
 * =======================
 *
 * The V2N-M1 SoM pairs the Renesas RZ/V2N (Cortex-A55 + DRP-AI)
 * with a DEEPX DX-M1 NPU over PCIe.  The M1 bring-up sequence is
 * below (docs/soms/v2n-m1.md is authoritative); steps 1, 3, and 4 run
 * entirely in U-Boot's board_late_init(), BEFORE this m33_sm image
 * ever starts.  Step 2 is NOT run by any automated bring-up code --
 * it is a bench-diagnostic check only.  This app exercises steps 5-6
 * through
 * <alp/inference.h>:
 *
 *   1. Bring up the DEEPX 0.75 V rail (DA9292 CH2), over RIIC8/
 *      BRD_I2C.  U-Boot's board_late_init() does this (meta-alp-sdk's
 *      0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch) -- RIIC8/
 *      BRD_I2C is Cortex-A55/Linux-exclusive, so nothing on this core
 *      could do it even if it wanted to.
 *   2. ACK-probe the DEEPX TPS628640 bucks at 0x44/0x4F (see
 *      docs/soms/v2n-m1.md) -- a bench-diagnostic step only, not
 *      implemented by U-Boot or any other automated bring-up code.
 *   3. Route the PCIe mux (PI3DBS12212) to the M1.
 *   4. Release `M1_RESET` (Renesas PA6) and wait for DEEPX firmware.
 *      Also U-Boot (same patch, `alp_deepx_pcie_bringup()`), only
 *      after step 1 confirms the rail is power-good.
 *      `deepx_dxm1_bring_up()` in `chips/deepx_dxm1/` implements the
 *      same steps 3-4 for platforms where a portable caller owns
 *      `M1_RESET`; on V2N-M1 it does not apply, and this example does
 *      not call it.
 *   5. Open an inference handle through `<alp/inference.h>` with
 *      `backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1` -- the
 *      Renesas-side Linux PCIe driver + the DEEPX runtime (both
 *      pulled in by the customer from `github.com/DEEPX-AI` per
 *      the §C.31 / §C.33 vendor-partnership trackers) handle the
 *      heavy lifting.
 *   6. Run one inference + print the result.
 *
 * The fake model and input in this example are illustrative
 * placeholders -- on real hardware you'd swap in a DXNN-compiled
 * model (from DEEPX's compiler toolchain) and your domain
 * payload.
 *
 * Licence story
 * =============
 *
 * The SDK code in this example is Apache-2.0.  The runtime
 * code paths it exercises pull from two licence buckets:
 *
 *   - `chips/deepx_dxm1/` (host driver, Apache-2.0) -- in-tree
 *     here; no licence-encumbered redistribution.
 *   - DEEPX `dx_rt` + Linux PCIe driver -- customer-only,
 *     pulled by the customer from `github.com/DEEPX-AI` at
 *     integration time.  See `docs/vendor-partnerships.md`
 *     §DEEPX for the licence text.
 *
 * Under native_sim this example compiles and runs, and the
 * inference call lands on the documented NOSUPPORT contract.
 * On E1M-V2M101 silicon this m33_sm image does not bring up the
 * DEEPX rail itself -- U-Boot already did, before this image
 * started -- and does not call `deepx_dxm1_bring_up()`;
 * `ALP_INFERENCE_BACKEND_DEEPX_DXM1` has no Zephyr backend
 * either.  Real DEEPX inference runs on the A55 Yocto image
 * (`src/yocto/inference_deepx.cpp`, via dx_rt).
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/inference.h"
#include "alp/peripheral.h"

/* No boot-wait constant here: this example does not call
 * deepx_dxm1_bring_up() (U-Boot does the equivalent sequencing
 * before this image starts -- see the file header).  Code that
 * does call it passes DEEPX_DXM1_DEFAULT_BOOT_US as its post-
 * M1_RESET-release wait; that is the single source for the delay.
 * (`<alp/chips/deepx_dxm1.h>` documents that the DX-M1 datasheet's
 * POR-to-PCIe-link-up number has not been read out yet, so no
 * `M1_BOOT_US`-style constant belongs in this file -- it would
 * claim a figure nobody has verified.) */

/* Placeholder model buffer -- on real hardware this is the
 * DXNN-compiled model byte stream.  Sized for the example only;
 * production code mmaps a model file or links it as a const
 * blob from a generator step. */
static const uint8_t k_placeholder_model[8] = {
	0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
};

int main(void)
{
	printf("[deepx] v2n-m1-deepx-inference flagship\n");
	printf("[deepx] stage 1: PCIe mux + DEEPX rail bring-up (already done by U-Boot)\n");
	/* U-Boot's board_late_init() sequenced the DA9292 CH2 = 0.75 V
     * DEEPX rail over RIIC8/BRD_I2C, confirmed power-good, and only
     * then routed the PCIe mux + released M1_RESET -- all of this
     * runs BEFORE this m33_sm image's main() -- see the file header
     * and meta-alp-sdk's 0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.
     * patch.  Nothing here does or could repeat any of that: RIIC8/
     * BRD_I2C is Cortex-A55/Linux-exclusive, and this example does
     * not call deepx_dxm1_bring_up() either.  Under native_sim the
     * example still validates the framing all the way through. */

	printf("[deepx] stage 2: opening DEEPX inference handle\n");
	alp_inference_config_t cfg = {
		.model_data  = k_placeholder_model,
		.model_size  = sizeof k_placeholder_model,
		.format      = ALP_INFERENCE_MODEL_DXNN,
		.backend     = ALP_INFERENCE_BACKEND_DEEPX_DXM1,
		.arena_bytes = 0u, /* let the backend pick */
		.arena       = NULL,
	};
	alp_inference_t *inf = alp_inference_open(&cfg);
	if (inf == NULL) {
		printf("[deepx]   open returned NULL: last_err=%d\n", (int)alp_last_error());
		printf("[deepx]   (expected under native_sim and on builds without dx_rt)\n");
		goto done;
	}

	printf("[deepx] stage 3: model accepts %zu input + %zu output tensors\n",
	       alp_inference_num_inputs(inf),
	       alp_inference_num_outputs(inf));

	alp_inference_tensor_t in = { 0 };
	if (alp_inference_get_input(inf, 0u, &in) == ALP_OK) {
		/* Fill the input buffer with the caller's domain payload
         * (sensor sample, image pixels, audio frame, ...).  For
         * this example we just zero it. */
		memset(in.data, 0, in.size_bytes);
	}

	printf("[deepx] stage 4: running inference\n");
	const alp_status_t rc = alp_inference_invoke(inf);
	if (rc != ALP_OK) {
		printf("[deepx]   invoke -> %d\n", (int)rc);
		alp_inference_close(inf);
		goto done;
	}

	alp_inference_tensor_t out = { 0 };
	if (alp_inference_get_output(inf, 0u, &out) == ALP_OK) {
		printf("[deepx] stage 5: output tensor: %u bytes, dtype=%d\n",
		       (unsigned)out.size_bytes,
		       (int)out.dtype);
	}

	alp_inference_close(inf);

done:
	printf("[deepx] done\n");
	return 0;
}
