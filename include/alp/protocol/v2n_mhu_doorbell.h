/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file v2n_mhu_doorbell.h
 * @brief RZ/V2N MHU-B doorbell register offsets shared by the Cortex-A55
 *        OpenAMP master and the Cortex-M33 responder.
 *
 * Both halves of the CM33 <-> CA55 link ring each other through the MHU-B
 * block (A55 0x10480000, CM33 view 0x50480000, offsets below are from the
 * block base).  The offsets live here so the Linux backend
 * (src/backends/rpc/yocto_uio_drv.c) and the CM33 firmware
 * (examples/multicore/rpmsg-v2n/m33_sm) cannot drift apart; the GIC SPI the
 * A55 listens on is the one number the devicetree repeats
 * (meta-alp-sdk .../e1m-v2n-doorbell.dtsi).
 *
 * Forward doorbell (CA55 -> CM33): MSG_INT_SET of NS slot 5, raising the
 * CM33's NVIC IRQ 293.  Bench-proven (#697).
 *
 * Reverse doorbell (CM33 -> CA55), two selectable paths:
 *
 *  - default: MHU-B SWINT unit 12, GIC_SPI 404 (INTID 436).  Bench-proven
 *    (#697 cycle 10: the NS-channel RSP of slot 5 reaches no GIC line).
 *  - ALP_V2N_DOORBELL_RSP_CH8: Renesas' documented `rsp_ch8_ns`, the RSP
 *    half of NS slot 8, GIC_SPI 385, level-high (Renesas RZ Multi-OS
 *    Package v4.2.0 platform_info.h INT_MHU_RSP_CH8_NS; the CA55 clears it
 *    with RSP_INT_CLR(8)).  NOT yet bench-proven.  Select it consistently:
 *    define ALP_V2N_DOORBELL_RSP_CH8 for the A55 backend
 *    (ALP_SDK_V2N_DOORBELL_RSP_CH8 CMake option), set
 *    CONFIG_ALP_V2N_DOORBELL_RSP_CH8=y for the CM33, and
 *    ALP_V2N_DOORBELL_SPI = "385" for the kernel devicetree.
 *
 * The A55 -> CM33 channel stays message channel 5 on both paths.
 */

#ifndef ALP_PROTOCOL_V2N_MHU_DOORBELL_H
#define ALP_PROTOCOL_V2N_MHU_DOORBELL_H

/** MHU-B block base as the Cortex-A55 maps it. */
#define ALP_V2N_MHU_B_A55_BASE 0x10480000u
/** MHU-B block base as the Cortex-M33 sees it. */
#define ALP_V2N_MHU_B_CM33_BASE 0x50480000u

/** Size of one NS crossbar slot (hal_renesas mhu_iodefine.h R_MHU0_Type). */
#define ALP_MHU_NS_SLOT_STRIDE 0x20u
/** Offset of NS slot @p ch from the block base. */
#define ALP_MHU_NS_SLOT(ch) ((ch) * ALP_MHU_NS_SLOT_STRIDE)

/* Register offsets inside one NS slot.  RSP_INT_* sit at +0x10 because of a
 * RESERVED[4] word at +0x0C (silicon-confirmed, #697 cycle 9). */
#define ALP_MHU_NS_SLOT_MSG_INT_STS 0x00u
#define ALP_MHU_NS_SLOT_MSG_INT_SET 0x04u
#define ALP_MHU_NS_SLOT_MSG_INT_CLR 0x08u
#define ALP_MHU_NS_SLOT_RSP_INT_STS 0x10u
#define ALP_MHU_NS_SLOT_RSP_INT_SET 0x14u
#define ALP_MHU_NS_SLOT_RSP_INT_CLR 0x18u

/** Slot of the CA55 -> CM33 kick (R_MHU_NS5.MSG, bench-proven #697 cycle 5). */
#define ALP_MHU_NS_CH5_KICK_SLOT ALP_MHU_NS_SLOT(5u)

/* SWINT block: one 0x10 unit per line, STS/SET/CLR at +0x00/04/08.  Only
 * units 12-15 reach the CA55 GIC (INTID 436 + (N - 12) = GIC_SPI 404 + ...). */
#define ALP_MHU_SWINT_BASE        0x800u
#define ALP_MHU_SWINT_UNIT_STRIDE 0x10u
#define ALP_MHU_SWINT_SET         0x04u
#define ALP_MHU_SWINT_CLR         0x08u
#define ALP_MHU_SWINT_UNIT(n)     (ALP_MHU_SWINT_BASE + (n) * ALP_MHU_SWINT_UNIT_STRIDE)

#if defined(ALP_V2N_DOORBELL_RSP_CH8) || defined(CONFIG_ALP_V2N_DOORBELL_RSP_CH8)

/** RSP channel the CM33 raises: Renesas rsp_ch8_ns. */
#define ALP_V2N_DOORBELL_RSP_CH 8u
/** GIC SPI the A55 receives the doorbell on. */
#define ALP_V2N_DOORBELL_GIC_SPI 385u
/** Block offset the CM33 writes 1 to, to ring the A55. */
#define ALP_V2N_DOORBELL_SET_OFF \
	(ALP_MHU_NS_SLOT(ALP_V2N_DOORBELL_RSP_CH) + ALP_MHU_NS_SLOT_RSP_INT_SET)
/** Block offset the A55 writes 1 to, to acknowledge (level-high line). */
#define ALP_V2N_DOORBELL_CLR_OFF \
	(ALP_MHU_NS_SLOT(ALP_V2N_DOORBELL_RSP_CH) + ALP_MHU_NS_SLOT_RSP_INT_CLR)

#else

/** SWINT unit the CM33 raises. */
#define ALP_V2N_DOORBELL_SWINT_UNIT 12u
/** GIC SPI the A55 receives the doorbell on. */
#define ALP_V2N_DOORBELL_GIC_SPI    404u
/** Block offset the CM33 writes 1 to, to ring the A55. */
#define ALP_V2N_DOORBELL_SET_OFF \
	(ALP_MHU_SWINT_UNIT(ALP_V2N_DOORBELL_SWINT_UNIT) + ALP_MHU_SWINT_SET)
/** Block offset the A55 writes 1 to, to acknowledge (level-high line). */
#define ALP_V2N_DOORBELL_CLR_OFF \
	(ALP_MHU_SWINT_UNIT(ALP_V2N_DOORBELL_SWINT_UNIT) + ALP_MHU_SWINT_CLR)

#endif

#endif /* ALP_PROTOCOL_V2N_MHU_DOORBELL_H */
