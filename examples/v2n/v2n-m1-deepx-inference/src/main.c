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
 * with a DEEPX DX-M1 NPU over PCIe.  This example walks the full
 * bring-up sequence for the M1 side:
 *
 *   1. Configure the PCIe mux (PI3DBS12212) to route the Renesas
 *      PCIe controller to the M1.
 *   2. Bring up the DEEPX 0.75 V rail (DA9292 CH2) -- handled by
 *      the V2N supervisor's `v2n_power_mgmt.c` module landed in
 *      §C.28.
 *   3. Release `M1_RESET` (Renesas PA6) via the
 *      `chips/deepx_dxm1/` host driver's `bring_up` sequencer.
 *   4. Wait for the PCIe link-up event.
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
 * Without a DEEPX NPU + the matching runtime, this example
 * compiles + runs its bring-up phase under native_sim (every
 * step returns NOSUPPORT / NOT_READY) but the inference call
 * lands on the documented NOSUPPORT contract.  Real silicon
 * runs the full path.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "alp/inference.h"
#include "alp/peripheral.h"

/* No boot-wait constant here: this example does not sequence the
 * DEEPX itself -- the supervisor's SYS_INIT drives bring-up.  Code
 * that does sequence it passes DEEPX_DXM1_DEFAULT_BOOT_US to
 * deepx_dxm1_bring_up(); that is the single source for the delay.
 * (A dead `M1_BOOT_US 5000u` used to sit here claiming a
 * DX-M1-datasheet figure -- <alp/chips/deepx_dxm1.h> states the
 * datasheet's POR-to-PCIe-link-up number has not been read out yet,
 * so the claim was unfounded as well as unused.) */

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
	printf("[deepx] stage 1: PCIe mux + power_mgmt bring-up (supervisor-side)\n");
	/* On real V2N-M1 silicon the supervisor's v2n_power_mgmt.c
     * (§C.28) brings up the 0.75 V DEEPX rail as soon as the
     * P65 IRQ fires; the PCIe mux + M1_RESET release ride the
     * deepx_dxm1_bring_up() helper below.  Under native_sim the
     * supervisor's NOSUPPORT path runs instead -- the example
     * still validates the framing all the way through. */

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
