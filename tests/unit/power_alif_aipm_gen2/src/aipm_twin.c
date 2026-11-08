/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2784: build-time twin check.  Compiles hal_alif's aipm.h WITH
 * CONFIG_ENSEMBLE_GEN2 (defined locally, in this translation unit only --
 * it is never defined globally, see alif_aipm_gen2.h) and asserts every
 * ALP_AIPM_GEN2_* value equals its hal_alif gen2 twin.  A swapped,
 * shifted or mistyped bit in alif_aipm_gen2.h fails the build here.
 * Nothing in this file runs.
 */

#define CONFIG_ENSEMBLE_GEN2 1

#include <stdint.h>

#include <aipm.h>

#include "alif_aipm_gen2.h"

_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM0_MASK) == (uint32_t)(SRAM0_MASK),
               "SRAM0_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM0) == (uint32_t)(MB_SRAM0),
               "MB_SRAM0 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM1_MASK) == (uint32_t)(SRAM1_MASK),
               "SRAM1_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM1) == (uint32_t)(MB_SRAM1),
               "MB_SRAM1 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM2_MASK) == (uint32_t)(SRAM2_MASK),
               "SRAM2_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM2) == (uint32_t)(MB_SRAM2),
               "MB_SRAM2 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM3_MASK) == (uint32_t)(SRAM3_MASK),
               "SRAM3_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM3) == (uint32_t)(MB_SRAM3),
               "MB_SRAM3 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM4_1_MASK) == (uint32_t)(SRAM4_1_MASK),
               "SRAM4_1_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM4_1) == (uint32_t)(MB_SRAM4_1),
               "MB_SRAM4_1 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM4_2_MASK) == (uint32_t)(SRAM4_2_MASK),
               "SRAM4_2_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM4_2) == (uint32_t)(MB_SRAM4_2),
               "MB_SRAM4_2 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM5_1_MASK) == (uint32_t)(SRAM5_1_MASK),
               "SRAM5_1_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM5_1) == (uint32_t)(MB_SRAM5_1),
               "MB_SRAM5_1 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM5_2_MASK) == (uint32_t)(SRAM5_2_MASK),
               "SRAM5_2_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM5_2) == (uint32_t)(MB_SRAM5_2),
               "MB_SRAM5_2 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM6A_MASK) == (uint32_t)(SRAM6A_MASK),
               "SRAM6A_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM6A) == (uint32_t)(MB_SRAM6A),
               "MB_SRAM6A differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM6B_MASK) == (uint32_t)(SRAM6B_MASK),
               "SRAM6B_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM6B) == (uint32_t)(MB_SRAM6B),
               "MB_SRAM6B differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM7_1_MASK) == (uint32_t)(SRAM7_1_MASK),
               "SRAM7_1_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM7_1) == (uint32_t)(MB_SRAM7_1),
               "MB_SRAM7_1 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM7_2_MASK) == (uint32_t)(SRAM7_2_MASK),
               "SRAM7_2_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM7_2) == (uint32_t)(MB_SRAM7_2),
               "MB_SRAM7_2 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM7_3_MASK) == (uint32_t)(SRAM7_3_MASK),
               "SRAM7_3_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM7_3) == (uint32_t)(MB_SRAM7_3),
               "MB_SRAM7_3 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM8_MASK) == (uint32_t)(SRAM8_MASK),
               "SRAM8_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM8) == (uint32_t)(MB_SRAM8),
               "MB_SRAM8 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM9_MASK) == (uint32_t)(SRAM9_MASK),
               "SRAM9_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM9) == (uint32_t)(MB_SRAM9),
               "MB_SRAM9 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MRAM_MASK) == (uint32_t)(MRAM_MASK),
               "MRAM_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_MRAM) == (uint32_t)(MB_MRAM),
               "MB_MRAM differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_OSPI0_MASK) == (uint32_t)(OSPI0_MASK),
               "OSPI0_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_OSPI0) == (uint32_t)(MB_OSPI0),
               "MB_OSPI0 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_OSPI1_MASK) == (uint32_t)(OSPI1_MASK),
               "OSPI1_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_OSPI1) == (uint32_t)(MB_OSPI1),
               "MB_OSPI1 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SERAM_1_MASK) == (uint32_t)(SERAM_1_MASK),
               "SERAM_1_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SERAM_1) == (uint32_t)(MB_SERAM_1),
               "MB_SERAM_1 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SERAM_2_MASK) == (uint32_t)(SERAM_2_MASK),
               "SERAM_2_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SERAM_2) == (uint32_t)(MB_SERAM_2),
               "MB_SERAM_2 differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_FWRAM_MASK) == (uint32_t)(FWRAM_MASK),
               "FWRAM_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_FWRAM) == (uint32_t)(MB_FWRAM),
               "MB_FWRAM differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_BACKUP4K_MASK) == (uint32_t)(BACKUP4K_MASK),
               "BACKUP4K_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_BACKUP4K) == (uint32_t)(MB_BACKUP4K),
               "MB_BACKUP4K differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM0_1_RET_MASK) == (uint32_t)(SRAM0_1_RET_MASK),
               "SRAM0_1_RET_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM0_1_RET) == (uint32_t)(MB_SRAM0_1_RET),
               "MB_SRAM0_1_RET differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM0_2_RET_MASK) == (uint32_t)(SRAM0_2_RET_MASK),
               "SRAM0_2_RET_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM0_2_RET) == (uint32_t)(MB_SRAM0_2_RET),
               "MB_SRAM0_2_RET differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM0_3_RET_MASK) == (uint32_t)(SRAM0_3_RET_MASK),
               "SRAM0_3_RET_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM0_3_RET) == (uint32_t)(MB_SRAM0_3_RET),
               "MB_SRAM0_3_RET differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM0_4_RET_MASK) == (uint32_t)(SRAM0_4_RET_MASK),
               "SRAM0_4_RET_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM0_4_RET) == (uint32_t)(MB_SRAM0_4_RET),
               "MB_SRAM0_4_RET differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SRAM1_RET_MASK) == (uint32_t)(SRAM1_RET_MASK),
               "SRAM1_RET_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_MB_SRAM1_RET) == (uint32_t)(MB_SRAM1_RET),
               "MB_SRAM1_RET differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_SERAM_MASK) == (uint32_t)(SERAM_MASK),
               "SERAM_MASK differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_SERTC) == (uint32_t)(WE_SERTC),
               "WE_SERTC differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPRTC) == (uint32_t)(WE_LPRTC),
               "WE_LPRTC differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPCMP) == (uint32_t)(WE_LPCMP),
               "WE_LPCMP differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_BOD) == (uint32_t)(WE_BOD),
               "WE_BOD differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPTIMER0) == (uint32_t)(WE_LPTIMER0),
               "WE_LPTIMER0 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPTIMER1) == (uint32_t)(WE_LPTIMER1),
               "WE_LPTIMER1 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPTIMER2) == (uint32_t)(WE_LPTIMER2),
               "WE_LPTIMER2 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPTIMER3) == (uint32_t)(WE_LPTIMER3),
               "WE_LPTIMER3 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO0) == (uint32_t)(WE_LPGPIO0),
               "WE_LPGPIO0 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO1) == (uint32_t)(WE_LPGPIO1),
               "WE_LPGPIO1 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO2) == (uint32_t)(WE_LPGPIO2),
               "WE_LPGPIO2 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO3) == (uint32_t)(WE_LPGPIO3),
               "WE_LPGPIO3 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO4) == (uint32_t)(WE_LPGPIO4),
               "WE_LPGPIO4 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO5) == (uint32_t)(WE_LPGPIO5),
               "WE_LPGPIO5 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO6) == (uint32_t)(WE_LPGPIO6),
               "WE_LPGPIO6 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO7) == (uint32_t)(WE_LPGPIO7),
               "WE_LPGPIO7 differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPTIMER) == (uint32_t)(WE_LPTIMER),
               "WE_LPTIMER differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_WE_LPGPIO) == (uint32_t)(WE_LPGPIO),
               "WE_LPGPIO differs from hal_alif");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_RTC_SE) == (uint32_t)(EWIC_RTC_SE),
               "EWIC_RTC_SE differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_LPGPIO) == (uint32_t)(EWIC_LPGPIO),
               "EWIC_LPGPIO differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_RTC_A) == (uint32_t)(EWIC_RTC_A),
               "EWIC_RTC_A differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_VBAT_TIMER) == (uint32_t)(EWIC_VBAT_TIMER),
               "EWIC_VBAT_TIMER differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_VBAT_GPIO) == (uint32_t)(EWIC_VBAT_GPIO),
               "EWIC_VBAT_GPIO differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_VBAT_LP_CMP_IRQ) == (uint32_t)(EWIC_VBAT_LP_CMP_IRQ),
               "EWIC_VBAT_LP_CMP_IRQ differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_ES1_LP_I2C_IRQ) == (uint32_t)(EWIC_ES1_LP_I2C_IRQ),
               "EWIC_ES1_LP_I2C_IRQ differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_ES1_LP_UART_IRQ) == (uint32_t)(EWIC_ES1_LP_UART_IRQ),
               "EWIC_ES1_LP_UART_IRQ differs from hal_alif gen2");
_Static_assert((uint32_t)(ALP_AIPM_GEN2_EWIC_BROWN_OUT) == (uint32_t)(EWIC_BROWN_OUT),
               "EWIC_BROWN_OUT differs from hal_alif gen2");
