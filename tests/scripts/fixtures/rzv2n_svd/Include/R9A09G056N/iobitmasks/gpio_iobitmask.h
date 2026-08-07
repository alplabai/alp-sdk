/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/* Trimmed excerpt of the vendored hal_renesas west module's
 * `.../Include/R9A09G056N/iobitmasks/gpio_iobitmask.h`, paired with the
 * trimmed iodefines/gpio_iodefine.h fixture next to this file --
 * scripts/gen_rzv2n_cm33_svd.py cross-validates every iodefine field
 * against macros like these. Values are copied verbatim from the real
 * file.
 */

#define R_GPIO_ELC_PDBF_PDBF_Msk        (0xFFUL)
#define R_GPIO_ELC_PDBF_PDBF_Pos        (0UL)

#define R_GPIO_ELC_PEL_PSB_Msk          (0x07UL)
#define R_GPIO_ELC_PEL_PSB_Pos          (0UL)
#define R_GPIO_ELC_PEL_PSP_Msk          (0x18UL)
#define R_GPIO_ELC_PEL_PSP_Pos          (3UL)
#define R_GPIO_ELC_PEL_PSM_Msk          (0x60UL)
#define R_GPIO_ELC_PEL_PSM_Pos          (5UL)

#define R_GPIO_PFC_ELC_ELSR2_PEG_Msk    (0x0CUL)
#define R_GPIO_PFC_ELC_ELSR2_PEG_Pos    (2UL)
#define R_GPIO_PFC_ELC_ELSR2_PES_Msk    (0xF0UL)
#define R_GPIO_PFC_ELC_ELSR2_PES_Pos    (4UL)
