/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/* Trimmed, syntactically-real excerpt of the vendored hal_renesas west
 * module's `.../Include/R9A09G056N.h` (only the CM33 core-config `#define`
 * block scripts/gen_rzv2n_cm33_svd.py's parse_cpu_header() reads is kept;
 * everything else in the real ~600-line file is unrelated CMSIS core
 * boilerplate). Values are copied verbatim from the real header.
 */

#ifndef R9A09G056N_H
#define R9A09G056N_H

#if defined(BSP_SUPPORT_CORE_CM33)
  #define __CM33_REV                0x0004U
  #define __NVIC_PRIO_BITS          7
  #define __Vendor_SysTickConfig    0
  #define __VTOR_PRESENT            1
  #define __MPU_PRESENT             1
  #define __FPU_PRESENT             1
  #define __FPU_DP                  0
  #define __DSP_PRESENT             1
  #define __SAUREGION_PRESENT       0
#elif defined(BSP_SUPPORT_CORE_CR8)
  #define __FPU_PRESENT             1
#endif

#endif
