/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * E1M-AEN801 / E1M-EVK DCT *_STATUS defines -- self-contained.
 *
 * The Alif kernel ships ../common/devkit_ex_dct_defines.h as a 0-byte stub;
 * the real content is injected at build time by the Alif dct-kernel Yocto
 * class (via DTS_MACRO_FILE force-include).  This recipe uses the standard
 * linux.inc kernel class (no dct-kernel), so the stub stays empty.
 *
 * This header therefore defines every macro e1m-aen801-evk.dts references
 * directly, without chaining to the stub.  Values are grounded from the
 * AEN801 E8 hardware (metadata/e1m_modules/aen/from-alif.tsv and
 * metadata/boards/e1m-evk.yaml) -- not invented:
 *
 *   console   E1M_UART0 = Alif UART5  (P3_5 UART5_TX_A / P3_4 UART5_RX_A)
 *   sensors   E1M_I2C0  = Alif I2C2   (P5_6 I2C2_SCL_C / P5_7 I2C2_SDA_C)
 *   disp/cam  E1M_I2C1  = Alif I2C1   (P3_7 I2C1_SCL_B / P7_2 I2C1_SDA_C)
 *
 * Node bodies + pinmux groups live in e1m-aen801-evk.dts.
 */

/* A32 cluster is dual-core; cpu1 online for SMP Linux. */
#define CPU1_STATUS "okay"

/* Memory stitching / HyperRAM: disabled pending full memory-map audit.
 * TODO(aen-memory-map): re-evaluate once the E1M-EVK memory map is confirmed. */
#define MEM_STITCH_STATUS     "disabled"
#define MEM_HYPER_STATUS      "disabled"
#define MEM_HYP_STITCH_STATUS "disabled"

/* Console: E1M_UART0 -> Alif UART5 (devkit default UART2 not routed). */
#define UART2_STATUS "disabled"
#define UART5_STATUS "okay"

/* I2C: E1M_I2C0 -> I2C2 (sensors bus), E1M_I2C1 -> I2C1 (disp/cam bus).
 * Alif I2C0 is not an E1M connector bus -> remains disabled (no #define needed;
 * ensemble-ex.dtsi default is disabled). */
#define I2C1_STATUS "okay"
#define I2C2_STATUS "okay"
