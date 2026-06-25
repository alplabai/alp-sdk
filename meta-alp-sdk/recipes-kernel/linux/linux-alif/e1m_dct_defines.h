/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * E1M-AEN801 / E1M-EVK DCT *_STATUS overrides.  The Alif dct-kernel class
 * force-includes ../common/devkit_ex_dct_defines.h via DTS_MACRO_FILE; this
 * header re-includes it (guard-deduped) and overrides only the peripherals the
 * E1M-EVK carrier actually routes, per the AUTHORITATIVE SoM routing
 * (metadata/e1m_modules/aen/from-alif.tsv) + carrier routing
 * (metadata/boards/e1m-evk.yaml) -- not invented:
 *
 *   console   E1M_UART0 = Alif UART5  (P3_5 UART5_TX_A / P3_4 UART5_RX_A)
 *   sensors   E1M_I2C0  = Alif I2C2   (P5_6 I2C2_SCL_C / P5_7 I2C2_SDA_C)
 *   disp/cam  E1M_I2C1  = Alif I2C1   (P3_7 I2C1_SCL_B / P7_2 I2C1_SDA_C)
 *
 * The devkit baseline enabled UART2 (different pins); that is a devkit-board
 * peripheral, disabled here.  Alif I2C0 is not an E1M bus -> left at the common
 * default (disabled).  Node bodies + pinmux groups live in e1m-aen801-evk.dts.
 */
#include "../common/devkit_ex_dct_defines.h"

/* console: E1M_UART0 -> Alif UART5 (devkit used UART2) */
#undef  UART2_STATUS
#define UART2_STATUS "disabled"
#undef  UART5_STATUS
#define UART5_STATUS "okay"

/* carrier I2C: E1M_I2C0 -> Alif I2C2 (sensors), E1M_I2C1 -> Alif I2C1 (disp/cam) */
#undef  I2C1_STATUS
#define I2C1_STATUS "okay"
#undef  I2C2_STATUS
#define I2C2_STATUS "okay"
