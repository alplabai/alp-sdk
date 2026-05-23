/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DSP class dispatcher.  Owns the public <alp/dsp.h> surface -- one
 * stateful handle type (alp_dsp_chain_t) carrying a composable
 * FIR / IIR / window / FFT pipeline -- on top of the backend registry.
 *
 * Dispatch shape mirrors the rtc / audio siblings: each open()
 * resolves the backend, allocates from a static pool, stores the
 * ops pointer on the handle's state struct, and lets the per-sample
 * apply ops dispatch through state.ops directly.  Capability
 * getters return per-handle cached snapshots the registry produced
 * at open() time.
 *
 * DSP is structurally unusual: the math kernels are libm +
 * CMSIS-DSP only -- OS-agnostic -- so the body ships as the
 * sw_fallback backend, and no separate zephyr_drv backend exists
 * today (V2N's HW-FFT bridge surface lands via wave-2's
 * alp_adc_filter_t / alp_adc_spectrum_t composition in <alp/adc.h>,
 * not through this class).  The dispatcher accepts a future HW
 * backend without change.
 *
 * Pool default: 2 chains (CONFIG_ALP_SDK_MAX_DSP_HANDLES).  Bumping
 * the cap costs ~9 KB per slot (FFT scratch + window samples +
 * per-stage state) so apps that need more should size the Kconfig
 * deliberately.
 *
 * last_error stamping reuses the existing TLS slot via extern
 * forward decls so the dispatcher does not pull in handles.h.  No
 * probe() is invoked -- base_caps are sufficient until a
 * SoC-specific backend with refined caps lands.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dsp.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/dsp/dsp_ops.h"

ALP_BACKEND_DEFINE_CLASS(dsp);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_DSP_HANDLES
#define CONFIG_ALP_SDK_MAX_DSP_HANDLES 2
#endif

static struct alp_dsp_chain _pool[CONFIG_ALP_SDK_MAX_DSP_HANDLES];

static struct alp_dsp_chain *_alloc(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_DSP_HANDLES; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free(struct alp_dsp_chain *h) { h->in_use = false; }

alp_dsp_chain_t *alp_dsp_chain_open(const alp_dsp_stage_t *stages,
                                    size_t n_stages)
{
    alp_z_clear_last_error();
    if (stages == NULL || n_stages == 0u) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("dsp", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_dsp_ops_t *ops = (const alp_dsp_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_dsp_chain *h = _alloc();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend   = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->open(stages, n_stages, &h->state, &caps);
    if (rc != ALP_OK) {
        _free(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_dsp_chain_apply_samples(alp_dsp_chain_t *chain,
                                         const int16_t *in_mv,
                                         size_t in_n,
                                         int16_t *out_mv,
                                         size_t out_cap,
                                         size_t *got)
{
    if (chain == NULL || !chain->in_use || got == NULL) {
        return ALP_ERR_INVAL;
    }
    if (chain->state.ops == NULL || chain->state.ops->apply_samples == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return chain->state.ops->apply_samples(&chain->state,
                                           in_mv, in_n,
                                           out_mv, out_cap, got);
}

alp_status_t alp_dsp_chain_apply_bins(alp_dsp_chain_t *chain,
                                      const int16_t *in_mv,
                                      size_t in_n,
                                      float *out_bins,
                                      size_t out_cap,
                                      size_t *got)
{
    if (chain == NULL || !chain->in_use || got == NULL) {
        return ALP_ERR_INVAL;
    }
    if (chain->state.ops == NULL || chain->state.ops->apply_bins == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return chain->state.ops->apply_bins(&chain->state,
                                        in_mv, in_n,
                                        out_bins, out_cap, got);
}

void alp_dsp_chain_close(alp_dsp_chain_t *chain)
{
    if (chain == NULL || !chain->in_use) return;
    if (chain->state.ops != NULL && chain->state.ops->close != NULL) {
        chain->state.ops->close(&chain->state);
    }
    _free(chain);
}

const alp_capabilities_t *alp_dsp_chain_capabilities(const alp_dsp_chain_t *chain)
{
    return (chain != NULL) ? &chain->cached_caps : NULL;
}
