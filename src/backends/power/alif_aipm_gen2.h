/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gen2 aiPM bit layout for the Alif Ensemble E8 (and, by inference, E4).
 *
 * hal_alif's aipm.h picks its memory_block_t / MASK layout with
 * CONFIG_ENSEMBLE_GEN2.  Nothing in alp-sdk defines that symbol, so a
 * stock build compiles the gen1 (#else) layout, in which BACKUP4K_MASK
 * is bit20 -- which is FWRAM_MASK on gen2.  The gen2 BACKUP4K block is
 * bit21.  Defining CONFIG_ENSEMBLE_GEN2 globally is NOT an option here:
 * it also changes hal_alif's OSPI behaviour (XIPWR_DFS_HC in
 * drivers/ospi/src/ospi.c, the AES delay layout in ospi.h), which needs
 * its own regression run (alp-sdk#2785).
 *
 * So the aiPM power backend takes its off_profile_t.memory_blocks,
 * off_profile_t.wakeup_events and EWIC bits from THIS header, never from
 * the hal_alif *_MASK / WE_* / EWIC_* macros.  The names carry the
 * ALP_AIPM_GEN2_ prefix so both can be visible in one translation unit.
 *
 * Evidence, per value:
 *   [AIPM]  hal_alif v2.3.0 (89ee0aa7), se_services/include/aipm.h,
 *           the #elif defined(CONFIG_ENSEMBLE_GEN2) memory_block_t
 *           enum + mask list (lines 195-259) and EWIC list (347-357).
 *   [SVD]   metadata/svd/alif/AE822FA0E5597BS0_CM55_HE_View.svd,
 *             VBAT.RET_CTRL  @ 0x1A60900C  (register at SVD line 5010)
 *             ANA.WKUP_CTRL  @ 0x1A60A008  (register at SVD line 5378)
 *   E8 is gen2 by register evidence: RET_CTRL carries SRAM0 RET1..RET4
 *   and SRAM1 RET fields, which only the gen2 memory_block_t has members
 *   for (MB_SRAM0_1_RET..MB_SRAM1_RET).  E4 is gen2 by inference only (no
 *   E4 SVD in the tree); the AEN401 bench run confirms it.
 *
 * What is enforced, and where:
 *   - Every ALP_AIPM_GEN2_* value is asserted EQUAL to its hal_alif gen2
 *     twin (aipm.h compiled with CONFIG_ENSEMBLE_GEN2) by
 *     tests/unit/power_alif_aipm_gen2/src/aipm_twin.c.  That is the
 *     check that catches a permuted or moved bit.
 *   - The ALP_AIPM_SVD_* constants are checked against the SVD file by
 *     tests/scripts/test_alif_aipm_gen2_svd.py.  The memory-block mask
 *     bits are an SE-service protocol, not register bits, so they are NOT
 *     derived from the SVD constants.  The SVD only corroborates which
 *     blocks exist (BKRAM, SRAM0 RET1..4, SRAM1 RET).
 *   - The asserts at the bottom of this header pin a few load-bearing
 *     values (BACKUP4K bit21 vs FWRAM bit20) and check the aggregates tile.
 *   - UNVERIFIED until the bench: that SRAM4_1/4_2/5_1/5_2 (HE ITCM/DTCM
 *     RET1/RET2) correspond to RET_CTRL.HETCM_RET1/RET2.
 *
 * Header-only, no includes beyond <stdint.h>, no runtime behaviour.
 */

#ifndef ALP_BACKENDS_POWER_ALIF_AIPM_GEN2_H_
#define ALP_BACKENDS_POWER_ALIF_AIPM_GEN2_H_

#include <stdint.h>

/* ---- SVD bit positions (VBAT.RET_CTRL, ANA.WKUP_CTRL) -------------- */

/* [SVD] RET_CTRL.BKRAM_RET_MASK, bits [0:0] "Utility SRAM" */
#define ALP_AIPM_SVD_RET_CTRL_BKRAM_RET_MASK_BIT 0u
/* [SVD] RET_CTRL.HETCM_RET1_MASK, bits [4:4] "M55-HE TCM RET1"; mapping of
 * SRAM4_x/SRAM5_x onto HETCM_RET1/RET2 is unverified until the bench. */
#define ALP_AIPM_SVD_RET_CTRL_HETCM_RET1_MASK_BIT 4u
/* [SVD] RET_CTRL.HETCM_RET2_MASK, bits [6:6] "M55-HE TCM RET2" (same caveat) */
#define ALP_AIPM_SVD_RET_CTRL_HETCM_RET2_MASK_BIT 6u
/* [SVD] RET_CTRL.CVM_RET1_MASK, bits [8:8] "SRAM0 RET1" */
#define ALP_AIPM_SVD_RET_CTRL_CVM_RET1_MASK_BIT 8u
/* [SVD] RET_CTRL.CVM_RET2_MASK, bits [10:10] "SRAM0 RET2" */
#define ALP_AIPM_SVD_RET_CTRL_CVM_RET2_MASK_BIT 10u
/* [SVD] RET_CTRL.CVM_RET3_MASK, bits [12:12] "SRAM0 RET3" */
#define ALP_AIPM_SVD_RET_CTRL_CVM_RET3_MASK_BIT 12u
/* [SVD] RET_CTRL.CVM_RET4_MASK, bits [14:14] "SRAM0 RET4" */
#define ALP_AIPM_SVD_RET_CTRL_CVM_RET4_MASK_BIT 14u
/* [SVD] RET_CTRL.OCVM_RET_MASK, bits [16:16] "SRAM1 RET" */
#define ALP_AIPM_SVD_RET_CTRL_OCVM_RET_MASK_BIT 16u

/* [SVD] WKUP_CTRL.RTCA, bits [5:5] */
#define ALP_AIPM_SVD_WKUP_CTRL_RTCA_BIT 5u
/* [SVD] WKUP_CTRL.LPCMP, bits [6:6] */
#define ALP_AIPM_SVD_WKUP_CTRL_LPCMP_BIT 6u
/* [SVD] WKUP_CTRL.BROWN_OUT, bits [7:7] */
#define ALP_AIPM_SVD_WKUP_CTRL_BROWN_OUT_BIT 7u
/* [SVD] WKUP_CTRL.LPTIMER, bits [11:8] (field LSB; width 4) */
#define ALP_AIPM_SVD_WKUP_CTRL_LPTIMER_LSB   8u
#define ALP_AIPM_SVD_WKUP_CTRL_LPTIMER_WIDTH 4u
/* [SVD] WKUP_CTRL.LPGPIO, bits [23:16] (field LSB; width 8) */
#define ALP_AIPM_SVD_WKUP_CTRL_LPGPIO_LSB   16u
#define ALP_AIPM_SVD_WKUP_CTRL_LPGPIO_WIDTH 8u

/* ---- Gen2 off_profile_t.memory_blocks bits -------------------------- */
/* [AIPM] aipm.h:195-259, gen2 memory_block_t index == mask bit. */

#define ALP_AIPM_GEN2_MB_SRAM0       0u
#define ALP_AIPM_GEN2_MB_SRAM1       1u
#define ALP_AIPM_GEN2_MB_SRAM2       2u
#define ALP_AIPM_GEN2_MB_SRAM3       3u
#define ALP_AIPM_GEN2_MB_SRAM4_1     4u /* M55-HE ITCM RET1 */
#define ALP_AIPM_GEN2_MB_SRAM4_2     5u /* M55-HE ITCM RET2 */
#define ALP_AIPM_GEN2_MB_SRAM5_1     6u /* M55-HE DTCM RET1 */
#define ALP_AIPM_GEN2_MB_SRAM5_2     7u /* M55-HE DTCM RET2 */
#define ALP_AIPM_GEN2_MB_SRAM6A      8u
#define ALP_AIPM_GEN2_MB_SRAM6B      9u
#define ALP_AIPM_GEN2_MB_SRAM7_1     10u
#define ALP_AIPM_GEN2_MB_SRAM7_2     11u
#define ALP_AIPM_GEN2_MB_SRAM7_3     12u
#define ALP_AIPM_GEN2_MB_SRAM8       13u
#define ALP_AIPM_GEN2_MB_SRAM9       14u
#define ALP_AIPM_GEN2_MB_MRAM        15u
#define ALP_AIPM_GEN2_MB_OSPI0       16u
#define ALP_AIPM_GEN2_MB_OSPI1       17u
#define ALP_AIPM_GEN2_MB_SERAM_1     18u
#define ALP_AIPM_GEN2_MB_SERAM_2     19u
#define ALP_AIPM_GEN2_MB_FWRAM       20u
#define ALP_AIPM_GEN2_MB_BACKUP4K    21u /* Utility SRAM; gen1 aipm.h puts this at 20 */
#define ALP_AIPM_GEN2_MB_SRAM0_1_RET 22u
#define ALP_AIPM_GEN2_MB_SRAM0_2_RET 23u
#define ALP_AIPM_GEN2_MB_SRAM0_3_RET 24u
#define ALP_AIPM_GEN2_MB_SRAM0_4_RET 25u
#define ALP_AIPM_GEN2_MB_SRAM1_RET   26u

#define ALP_AIPM_GEN2_SRAM0_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM0)
#define ALP_AIPM_GEN2_SRAM1_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM1)
#define ALP_AIPM_GEN2_SRAM2_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM2)
#define ALP_AIPM_GEN2_SRAM3_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM3)
#define ALP_AIPM_GEN2_SRAM4_1_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM4_1)
#define ALP_AIPM_GEN2_SRAM4_2_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM4_2)
#define ALP_AIPM_GEN2_SRAM5_1_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM5_1)
#define ALP_AIPM_GEN2_SRAM5_2_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM5_2)
#define ALP_AIPM_GEN2_SRAM6A_MASK      (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM6A)
#define ALP_AIPM_GEN2_SRAM6B_MASK      (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM6B)
#define ALP_AIPM_GEN2_SRAM7_1_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM7_1)
#define ALP_AIPM_GEN2_SRAM7_2_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM7_2)
#define ALP_AIPM_GEN2_SRAM7_3_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM7_3)
#define ALP_AIPM_GEN2_SRAM8_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM8)
#define ALP_AIPM_GEN2_SRAM9_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM9)
#define ALP_AIPM_GEN2_MRAM_MASK        (UINT32_C(1) << ALP_AIPM_GEN2_MB_MRAM)
#define ALP_AIPM_GEN2_OSPI0_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_OSPI0)
#define ALP_AIPM_GEN2_OSPI1_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_OSPI1)
#define ALP_AIPM_GEN2_SERAM_1_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SERAM_1)
#define ALP_AIPM_GEN2_SERAM_2_MASK     (UINT32_C(1) << ALP_AIPM_GEN2_MB_SERAM_2)
#define ALP_AIPM_GEN2_FWRAM_MASK       (UINT32_C(1) << ALP_AIPM_GEN2_MB_FWRAM)
#define ALP_AIPM_GEN2_BACKUP4K_MASK    (UINT32_C(1) << ALP_AIPM_GEN2_MB_BACKUP4K)
#define ALP_AIPM_GEN2_SRAM0_1_RET_MASK (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM0_1_RET)
#define ALP_AIPM_GEN2_SRAM0_2_RET_MASK (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM0_2_RET)
#define ALP_AIPM_GEN2_SRAM0_3_RET_MASK (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM0_3_RET)
#define ALP_AIPM_GEN2_SRAM0_4_RET_MASK (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM0_4_RET)
#define ALP_AIPM_GEN2_SRAM1_RET_MASK   (UINT32_C(1) << ALP_AIPM_GEN2_MB_SRAM1_RET)

#define ALP_AIPM_GEN2_SERAM_MASK (ALP_AIPM_GEN2_SERAM_1_MASK | ALP_AIPM_GEN2_SERAM_2_MASK)

/* ---- off_profile_t.wakeup_events bits ------------------------------- */
/* [AIPM] aipm.h:311-330 (identical on gen1 and gen2).  Bits 5..11 and
 * 16..23 are tied to ANA.WKUP_CTRL below; WE_SERTC (bit4) has no
 * WKUP_CTRL field, it is an SE-level source only. */

#define ALP_AIPM_GEN2_WE_SERTC    (UINT32_C(1) << 4)
#define ALP_AIPM_GEN2_WE_LPRTC    (UINT32_C(1) << 5) /* WKUP_CTRL.RTCA */
#define ALP_AIPM_GEN2_WE_LPCMP    (UINT32_C(1) << 6) /* WKUP_CTRL.LPCMP */
#define ALP_AIPM_GEN2_WE_BOD      (UINT32_C(1) << 7) /* WKUP_CTRL.BROWN_OUT */
#define ALP_AIPM_GEN2_WE_LPTIMER0 (UINT32_C(1) << 8)
#define ALP_AIPM_GEN2_WE_LPTIMER1 (UINT32_C(1) << 9)
#define ALP_AIPM_GEN2_WE_LPTIMER2 (UINT32_C(1) << 10)
#define ALP_AIPM_GEN2_WE_LPTIMER3 (UINT32_C(1) << 11)
#define ALP_AIPM_GEN2_WE_LPGPIO0  (UINT32_C(1) << 16)
#define ALP_AIPM_GEN2_WE_LPGPIO1  (UINT32_C(1) << 17)
#define ALP_AIPM_GEN2_WE_LPGPIO2  (UINT32_C(1) << 18)
#define ALP_AIPM_GEN2_WE_LPGPIO3  (UINT32_C(1) << 19)
#define ALP_AIPM_GEN2_WE_LPGPIO4  (UINT32_C(1) << 20)
#define ALP_AIPM_GEN2_WE_LPGPIO5  (UINT32_C(1) << 21)
#define ALP_AIPM_GEN2_WE_LPGPIO6  (UINT32_C(1) << 22)
#define ALP_AIPM_GEN2_WE_LPGPIO7  (UINT32_C(1) << 23)
#define ALP_AIPM_GEN2_WE_LPTIMER  UINT32_C(0xF00)    /* bit11:8 */
#define ALP_AIPM_GEN2_WE_LPGPIO   UINT32_C(0xFF0000) /* bit23:16 */

/* ---- Gen2 EWIC bits -------------------------------------------------- */
/* [AIPM] aipm.h:347-357 (#elif defined(CONFIG_ENSEMBLE_GEN2)).  The EWIC
 * register is not described by the E8 SVD, so these have no SVD tie; the
 * asserts below only check internal consistency.  The gen1 (#else) list
 * at aipm.h:359-367 carries the same bit positions for every bit gen1
 * defines (gen1 has no EWIC_LPGPIO / EWIC_UNUSED_1), so the EWIC half of
 * the mismatch is benign today. */

#define ALP_AIPM_GEN2_EWIC_RTC_SE          UINT32_C(0x1)          /* bit0 */
#define ALP_AIPM_GEN2_EWIC_LPGPIO          (UINT32_C(3) << 4)     /* bit5:4 */
#define ALP_AIPM_GEN2_EWIC_RTC_A           (UINT32_C(1) << 6)     /* bit6 */
#define ALP_AIPM_GEN2_EWIC_VBAT_TIMER      (UINT32_C(0xF) << 7)   /* bit10:7 */
#define ALP_AIPM_GEN2_EWIC_VBAT_GPIO       (UINT32_C(0xFF) << 11) /* bit18:11 */
#define ALP_AIPM_GEN2_EWIC_VBAT_LP_CMP_IRQ (UINT32_C(1) << 19)    /* bit19 */
#define ALP_AIPM_GEN2_EWIC_ES1_LP_I2C_IRQ  (UINT32_C(1) << 20)    /* bit20 */
#define ALP_AIPM_GEN2_EWIC_ES1_LP_UART_IRQ (UINT32_C(1) << 21)    /* bit21 */
#define ALP_AIPM_GEN2_EWIC_BROWN_OUT       (UINT32_C(1) << 22)    /* bit22 */

/* ---- Static asserts -------------------------------------------------- */

/* Utility SRAM: BACKUP4K mask is bit21 (NOT bit20, which is FWRAM here). */
_Static_assert(ALP_AIPM_GEN2_BACKUP4K_MASK == UINT32_C(0x00200000),
               "gen2 BACKUP4K_MASK must be bit21 (aipm.h:252)");
_Static_assert(ALP_AIPM_GEN2_FWRAM_MASK == UINT32_C(0x00100000),
               "gen2 FWRAM_MASK must be bit20 (aipm.h:251)");
_Static_assert(ALP_AIPM_GEN2_BACKUP4K_MASK != ALP_AIPM_GEN2_FWRAM_MASK,
               "BACKUP4K and FWRAM must not alias (the gen1 layout aliases them)");

/* SRAM0 RET1..RET4 and SRAM1 RET run from bit22 in that order. */
_Static_assert(ALP_AIPM_GEN2_SRAM0_1_RET_MASK == UINT32_C(1) << 22, "SRAM0_1_RET is bit22");
_Static_assert(ALP_AIPM_GEN2_SRAM0_4_RET_MASK == UINT32_C(1) << 25, "SRAM0_4_RET is bit25");
_Static_assert(ALP_AIPM_GEN2_SRAM1_RET_MASK == UINT32_C(1) << 26, "SRAM1_RET is bit26");

/* The mask list covers bits 0..26 with no gaps and no overlap. */
_Static_assert((ALP_AIPM_GEN2_SRAM0_MASK | ALP_AIPM_GEN2_SRAM1_MASK | ALP_AIPM_GEN2_SRAM2_MASK |
                ALP_AIPM_GEN2_SRAM3_MASK | ALP_AIPM_GEN2_SRAM4_1_MASK | ALP_AIPM_GEN2_SRAM4_2_MASK |
                ALP_AIPM_GEN2_SRAM5_1_MASK | ALP_AIPM_GEN2_SRAM5_2_MASK |
                ALP_AIPM_GEN2_SRAM6A_MASK | ALP_AIPM_GEN2_SRAM6B_MASK | ALP_AIPM_GEN2_SRAM7_1_MASK |
                ALP_AIPM_GEN2_SRAM7_2_MASK | ALP_AIPM_GEN2_SRAM7_3_MASK | ALP_AIPM_GEN2_SRAM8_MASK |
                ALP_AIPM_GEN2_SRAM9_MASK | ALP_AIPM_GEN2_MRAM_MASK | ALP_AIPM_GEN2_OSPI0_MASK |
                ALP_AIPM_GEN2_OSPI1_MASK | ALP_AIPM_GEN2_SERAM_1_MASK | ALP_AIPM_GEN2_SERAM_2_MASK |
                ALP_AIPM_GEN2_FWRAM_MASK | ALP_AIPM_GEN2_BACKUP4K_MASK |
                ALP_AIPM_GEN2_SRAM0_1_RET_MASK | ALP_AIPM_GEN2_SRAM0_2_RET_MASK |
                ALP_AIPM_GEN2_SRAM0_3_RET_MASK | ALP_AIPM_GEN2_SRAM0_4_RET_MASK |
                ALP_AIPM_GEN2_SRAM1_RET_MASK) == UINT32_C(0x07FFFFFF),
               "gen2 memory_block masks must cover bits 0..26");

/* Wake-event groups tile their aggregate masks. */
_Static_assert(ALP_AIPM_GEN2_WE_LPTIMER == (ALP_AIPM_GEN2_WE_LPTIMER0 | ALP_AIPM_GEN2_WE_LPTIMER1 |
                                            ALP_AIPM_GEN2_WE_LPTIMER2 | ALP_AIPM_GEN2_WE_LPTIMER3),
               "WE_LPTIMER0..3 must tile WE_LPTIMER");
_Static_assert(ALP_AIPM_GEN2_WE_LPGPIO ==
                   (ALP_AIPM_GEN2_WE_LPGPIO0 | ALP_AIPM_GEN2_WE_LPGPIO1 | ALP_AIPM_GEN2_WE_LPGPIO2 |
                    ALP_AIPM_GEN2_WE_LPGPIO3 | ALP_AIPM_GEN2_WE_LPGPIO4 | ALP_AIPM_GEN2_WE_LPGPIO5 |
                    ALP_AIPM_GEN2_WE_LPGPIO6 | ALP_AIPM_GEN2_WE_LPGPIO7),
               "WE_LPGPIO0..7 must tile WE_LPGPIO");

/* EWIC: contiguous, non-overlapping, bits 0..22 minus the unused 3:1. */
_Static_assert((ALP_AIPM_GEN2_EWIC_RTC_SE | ALP_AIPM_GEN2_EWIC_LPGPIO | ALP_AIPM_GEN2_EWIC_RTC_A |
                ALP_AIPM_GEN2_EWIC_VBAT_TIMER | ALP_AIPM_GEN2_EWIC_VBAT_GPIO |
                ALP_AIPM_GEN2_EWIC_VBAT_LP_CMP_IRQ | ALP_AIPM_GEN2_EWIC_ES1_LP_I2C_IRQ |
                ALP_AIPM_GEN2_EWIC_ES1_LP_UART_IRQ | ALP_AIPM_GEN2_EWIC_BROWN_OUT) ==
                   UINT32_C(0x007FFFF1),
               "gen2 EWIC bits must cover 0 and 4..22");
_Static_assert(ALP_AIPM_GEN2_EWIC_VBAT_TIMER == UINT32_C(0x780), "EWIC VBAT_TIMER is bit10:7");
_Static_assert(ALP_AIPM_GEN2_EWIC_VBAT_GPIO == UINT32_C(0x7F800), "EWIC VBAT_GPIO is bit18:11");

#endif /* ALP_BACKENDS_POWER_ALIF_AIPM_GEN2_H_ */
