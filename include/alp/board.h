/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file board.h
 * @brief Board-agnostic facade for cross-EVK examples.
 *
 * Includes the active board's generated routes header (selected by the
 * ALP_BOARD_<SLUG> compile define the build emits from the board.yaml
 * preset) so an example can open pins via the portable BOARD_* aliases
 * and build for whichever EVK is targeted.  The BOARD_* names cover the
 * e1m-spec STANDARD.md §7.2 interfaces common to both form factors.
 *
 * Form-factor-specific examples should NOT use this facade; they include
 * the specific routes header (alp_e1m_evk_routes.h / alp_e1m_x_evk_routes.h)
 * directly and use EVK_* / XEVK_* macros.
 */
#ifndef ALP_BOARD_H
#define ALP_BOARD_H

#if defined(ALP_BOARD_E1M_X_EVK)
#include "alp/boards/alp_e1m_x_evk_routes.h"
#elif defined(ALP_BOARD_E1M_EVK)
#include "alp/boards/alp_e1m_evk_routes.h"
#else
#error \
    "alp/board.h: no ALP_BOARD_* board selected. Set the example's board.yaml preset (e1m-evk / e1m-x-evk), or pass -DALP_BOARD_E1M_EVK / -DALP_BOARD_E1M_X_EVK."
#endif

#endif /* ALP_BOARD_H */
