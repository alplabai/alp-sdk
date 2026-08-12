/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ABI pin for the ZCBOR_* compile-time macro set (issue #1254 follow-up).
 *
 * ZCBOR_MAP_SMART_SEARCH and ZCBOR_STOP_ON_ERROR #ifdef fields into
 * `struct zcbor_state` / `struct zcbor_state_constant` (zcbor_common.h);
 * ZCBOR_CANONICAL and ZCBOR_BIG_ENDIAN change default behaviour baked
 * into libzcbor.so at ITS OWN build time (zcbor_0.9.1.bb's do_compile).
 * A consumer TU compiled with a different setting than the .so's own
 * build gets a silently mismatched struct across the .so boundary --
 * memory corruption at runtime, no link or compile error.
 *
 * The set pinned here MATCHES what Zephyr's own zcbor module Kconfig
 * produces for every alp-sdk target today (see the grounding note in
 * metadata/libraries/zcbor.yaml): no alp-sdk prj.conf sets
 * CONFIG_ZCBOR_STOP_ON_ERROR or CONFIG_ZCBOR_CANONICAL,
 * CONFIG_ZCBOR_MAP_SMART_SEARCH has no Kconfig symbol in this zcbor
 * version at all (so Zephyr can never turn it on), and
 * CONFIG_ZCBOR_BIG_ENDIAN tracks target endianness -- every alp-sdk
 * core (Cortex-A55/M33/M55) is little-endian. So the matching set is:
 * NONE of the four defined. do_compile above deliberately adds no
 * -DZCBOR_* flags for the same reason -- keep the two in lockstep.
 *
 * #error below turns a stray future -D into a build failure instead of
 * a silent runtime struct-layout mismatch.
 */
#ifndef ALP_ZCBOR_ABI_PIN_H
#define ALP_ZCBOR_ABI_PIN_H

#if defined(ZCBOR_MAP_SMART_SEARCH)
#error \
    "ZCBOR_MAP_SMART_SEARCH must stay undefined to match libzcbor.so's own build -- see zcbor_abi_pin.h"
#endif
#if defined(ZCBOR_STOP_ON_ERROR)
#error \
    "ZCBOR_STOP_ON_ERROR must stay undefined to match libzcbor.so's own build -- see zcbor_abi_pin.h"
#endif
#if defined(ZCBOR_CANONICAL)
#error "ZCBOR_CANONICAL must stay undefined to match libzcbor.so's own build -- see zcbor_abi_pin.h"
#endif
#if defined(ZCBOR_BIG_ENDIAN)
#error \
    "ZCBOR_BIG_ENDIAN must stay undefined to match libzcbor.so's own build -- see zcbor_abi_pin.h"
#endif

#endif /* ALP_ZCBOR_ABI_PIN_H */
