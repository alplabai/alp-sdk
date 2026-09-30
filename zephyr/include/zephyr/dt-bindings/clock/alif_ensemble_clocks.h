/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif's own Zephyr fork names its E-series clock binding header
 * alif_ensemble_clocks.h; upstream Zephyr (which alp-sdk pins) names it
 * alif-ensemble-clocks.h, and alp-sdk adds the clocks upstream lacks in
 * alif-ensemble-clocks-ext.h. Vendor modules written against the fork, such
 * as Alif's D/AVE 2D driver (d1/src/dave_base.c), include the fork's name;
 * this maps it onto the pinned tree.
 */

#ifndef ALP_SDK_DT_BINDINGS_CLOCK_ALIF_ENSEMBLE_CLOCKS_FORK_NAME_H_
#define ALP_SDK_DT_BINDINGS_CLOCK_ALIF_ENSEMBLE_CLOCKS_FORK_NAME_H_

#include <zephyr/dt-bindings/clock/alif-ensemble-clocks.h>
#include <zephyr/dt-bindings/clock/alif-ensemble-clocks-ext.h>

#endif
