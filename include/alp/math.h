/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file math.h
 * @brief ALP SDK math library — re-export of CMSIS-DSP.
 *
 * No new ABI in v0.1.  Including this header pulls in arm_math.h
 * when ALP_HAS_CMSIS_DSP is set by the build.  See
 * docs/os-support-matrix.md for which CMSIS-DSP feature groups are
 * validated per SoM (Helium / DSP extension presence varies).
 */

#ifndef ALP_MATH_H
#define ALP_MATH_H

#ifdef ALP_HAS_CMSIS_DSP
#  include "arm_math.h"
#endif

#endif  /* ALP_MATH_H */
