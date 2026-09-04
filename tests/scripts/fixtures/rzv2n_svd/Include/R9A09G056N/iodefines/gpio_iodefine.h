/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/**********************************************************************************************************************
 * File Name    : gpio_iodefine.h
 * Version      : 1.00
 * Description  : IO define file for gpio.
 *********************************************************************************************************************/

/* Trimmed, syntactically-real excerpt of the vendored hal_renesas west
 * module's `.../Include/R9A09G056N/iodefines/gpio_iodefine.h` (the real
 * file is ~6000 lines for one peripheral). Every declaration below is
 * copied verbatim from the real file; only the surrounding ~6000-line
 * R_GPIO_Type body is cut down to a handful of members chosen to exercise
 * scripts/gen_rzv2n_cm33_svd.py's four member shapes in one small struct:
 * RESERVED padding, a nested cluster type (R_ELC_PDBF_Type via PDBF[2]),
 * a register array (ELC_PEL[4]), and a plain scalar register
 * (PFC_ELC_ELSR2). Real addresses: R_GPIO_BASE is 0x40410020 in the real
 * file; this fixture keeps that value but the members' relative offsets
 * inside R_GPIO_Type do NOT match the real file (real GPIO has ~14000
 * more bytes of members before these, trimmed away here).
 *
 * The trailing "Size = 4" comment on R_ELC_PDBF_Type's closing brace below
 * is NOT copied from the real file (the real, untrimmed R_GPIO_Type carries
 * no such per-nested-type comment) -- it is added here, matching the
 * type's real computed size (1 byte ELC_PDBF + 3 bytes RESERVED32[3] = 4),
 * purely so the mutation tests in test_gen_rzv2n_cm33_svd.py have a
 * Size-hint comment to perturb; see SIZE_HINT_RE / SIZE_HINT_SKIPS in the
 * generator itself for the real (untrimmed) construct this exercises.
 */

#ifndef GPIO_IODEFINE_H
#define GPIO_IODEFINE_H

typedef struct
{
    union
    {
        __IOM uint8_t ELC_PDBF;
        struct
        {
            __IOM uint8_t PDBF : 8;
        } ELC_PDBF_b;
    };
    __IM uint8_t RESERVED32[3];
} R_ELC_PDBF_Type; /*!< Size = 4 (0x4) */

typedef struct
{
    __IM uint8_t RESERVED[4];
    __IOM R_ELC_PDBF_Type PDBF[2];
    union
    {
        __IOM uint8_t ELC_PEL[4];
        struct
        {
            __IOM uint8_t PSB : 3;
            __IOM uint8_t PSP : 2;
            __IOM uint8_t PSM : 2;
            uint8_t           : 1;
        } ELC_PEL_b;
    };
    union
    {
        __IOM uint8_t PFC_ELC_ELSR2;
        struct
        {
            uint8_t           : 2;
            __IOM uint8_t PEG : 2;
            __IOM uint8_t PES : 4;
        } PFC_ELC_ELSR2_b;
    };
} R_GPIO_Type;

/* =========================================================================================================================== */
/* ================                          Device Specific Peripheral Address Map                           ================ */
/* =========================================================================================================================== */

#define R_GPIO_BASE    0x40410020

/* =========================================================================================================================== */
/* ================                                  Peripheral declaration                                   ================ */
/* =========================================================================================================================== */

#define R_GPIO    ((R_GPIO_Type *) R_GPIO_BASE)

#endif
