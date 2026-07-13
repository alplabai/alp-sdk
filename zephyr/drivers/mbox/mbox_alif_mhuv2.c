// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 Alp Lab AB
 *
 * Zephyr MBOX-class driver for the ARM MHUv2 (Message Handling Unit, v2) as
 * integrated in the Alif Ensemble family (E1/E3/E5/E7/E8) for inter-core
 * doorbell signalling -- e.g. RTSS-HE <-> APSS / RTSS-HP. Backs alp_rpc_open
 * (OpenAMP static-vrings) on AEN. Resolves alp-sdk issues #45/#50.
 *
 * ============================== STATUS ==============================
 * ADR-0017-ADJACENT (vendor-native custom): a novel Alif MHUv2 doorbell IP with
 * NO upstream Zephyr driver, no sdk-alif fork driver, and no hal_alif library to
 * consume -- so it does not fit Tier-1/1.5/2/3. Authored from spec as a last
 * resort per ADR 0017 (docs/adr/0017-alp-sdk-over-the-vendor-sdk.md).
 * vendor-ext, bench-validated on E1M-AEN801 for the dual-M55 RPMsg path.
 *
 * This driver was authored from the ARM MHUv2 register map (ARM DDI 0515,
 * corroborated by Linux drivers/mailbox/arm_mhuv2.c #defines and the original
 * LKML patch) and the node addresses/IRQs in the alifsemi/zephyr_alif fork's
 * dts/arm/alif/ensemble/common/e1.dtsi. The E1M-AEN801 bench validated this
 * driver with a dual-M55 OpenAMP RPMsg ping/pong on 2026-06-19 (#225). Every
 * register offset below is transcribed verbatim from that spec; no offset has
 * been invented. Where a feature is version-dependent (v2.0 vs v2.1) and not
 * relied upon, it is called out in a comment rather than guessed at.
 * ====================================================================
 *
 * Compatible: `alif,mhuv2-mbox` (deliberately NOT `arm,mhuv2`, to stay
 * collision-free against the opt-in zephyr_alif fork's `arm,mhuv2` binding so
 * the whole stack works on the default upstream-Zephyr + hal_alif base).
 *
 * Model:
 *   - One devicetree node = one MHUv2 frame, sender OR receiver, reg size
 *     0x1000, role selected by the `alif,direction` string property.
 *   - Doorbell (signalling) mode only: no payload travels through the MHU.
 *     `mtu_get` returns 0; data lives in shared SRAM per the OpenAMP vrings.
 *   - #mbox-cells = <1>: the cell is the doorbell bit index (0..31) inside
 *     channel-window 0. We use window 0 only.
 *
 * No HAL dependency; pure sys_read32/sys_write32 against the mapped frame.
 */

#define DT_DRV_COMPAT alif_mhuv2_mbox

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/*
 * ARM MHUv2 register map (authoritative -- ARM DDI 0515). Channel-window
 * stride is 0x20; this driver uses window 0 (offset 0 from the frame base).
 * All offsets below are window-0 / control-page offsets transcribed verbatim
 * from the spec. No value here is invented.
 */

/* Channel-window stride between successive channel windows (we use window 0). */
#define MHUV2_CH_STRIDE 0x20U

/* SENDER frame registers (window 0). */
#define MHUV2_TX_CH0_STAT 0x00U        /* RO  current flag bits (rx clears)   */
#define MHUV2_TX_CH0_SET 0x0CU         /* W1S ring doorbell: write (1u<<bit)  */
#define MHUV2_TX_ACCESS_REQUEST 0xF88U /* RW  write 1 to request rx wake      */
#define MHUV2_TX_ACCESS_READY 0xF8CU   /* RO  reads 1 when rx is ready        */
/*
 * NOTE: MHUv2.1 adds per-window CH_INT_* at window-offset +0x10/+0x14/+0x18 on
 * the sender side; MHUv2.0 lacks them. This driver does NOT rely on them.
 */

/* RECEIVER frame registers (window 0). */
#define MHUV2_RX_CH0_STAT 0x00U        /* RO  raw incoming flag bits          */
#define MHUV2_RX_CH0_STAT_MASKED 0x04U /* RO  flags AND enabled (unmasked)    */
#define MHUV2_RX_CH0_CLEAR 0x08U       /* W1C ack: write (1u<<bit) to clear   */
#define MHUV2_RX_CH0_MASK_STATUS 0x10U /* RO  1 = masked (no IRQ)             */
#define MHUV2_RX_CH0_MASK_SET 0x14U    /* W1S write 1 to MASK (disable IRQ)   */
#define MHUV2_RX_CH0_MASK_CLEAR 0x18U  /* W1C write 1 to UNMASK (enable IRQ)  */
/*
 * Receiver-frame control block (after the 124 channel windows at 0x0..0xF7C):
 * INT_EN @0xF98 gates the COMBINED receive interrupt. Unmasking a channel
 * (CH0_MASK_CLEAR) is NOT enough on the Alif MHUv2 frame -- the CHCOMB bit in
 * INT_EN must ALSO be set or the NVIC IRQ never fires (Alif DFP
 * drivers/source/mhu_receiver.c: MHU_receiver_set_irq_enable(INT_EN, CHCOMB)).
 */
#define MHUV2_RX_INT_EN     0xF98U /* RW  combined-interrupt enable */
#define MHUV2_RX_INT_CHCOMB 0x4U   /* combined-channel interrupt bit (MHU_CHCOMB) */

/* The frame exposes 32 doorbell bits in channel-window 0. */
#define MHUV2_NUM_CHANNELS 32U

/* Bounded spin budget for the ACCESS_REQUEST -> ACCESS_READY wake handshake. */
#define MHUV2_ACCESS_READY_SPINS 100000U

/** @brief Per-instance immutable configuration. */
struct mhuv2_config {
    /** Mapped base address of this MHUv2 frame (window 0 base). */
    mm_reg_t base;
    /** true for a sender ("tx") frame, false for a receiver ("rx") frame. */
    bool is_tx;
};

/** @brief Per-instance mutable state (receiver frames only). */
struct mhuv2_data {
    /** Registered per-doorbell-bit callbacks. */
    mbox_callback_t cb[MHUV2_NUM_CHANNELS];
    /** User context paired with each callback. */
    void *cb_ctx[MHUV2_NUM_CHANNELS];
    /** Back-pointer for the poll timer (see below). */
    const struct device *dev;
    /**
     * POLL fallback: on the Alif E8 the non-secure HE<->HP MHU-1 receiver does
     * NOT raise the combined NVIC interrupt even when correctly configured
     * (INT_EN.CHCOMB set, channel unmasked, IRQ enabled) -- bench-confirmed
     * (the doorbell status bit sets, but no IRQ). Every working path on this
     * silicon polls the raw CH0_STAT. So drive the same dispatch from a periodic
     * poll timer started in set_enabled(), independent of the (dead) IRQ.
     */
    struct k_timer poll_timer;
    /**
     * Bitmask of currently-enabled doorbell bits. The poll timer is shared by
     * the whole frame, so it may only be stopped once the LAST enabled channel
     * is disabled -- otherwise disabling one channel would silently kill
     * dispatch for every other still-enabled channel.
     */
    uint32_t enabled_mask;
};

/* Dispatch the registered callbacks for every asserted (and unmasked) doorbell
 * bit, then ack. Shared by the ISR and the poll timer. */
static void mhuv2_rx_dispatch(const struct device *dev);

/**
 * @brief Send (ring a doorbell) over a sender frame.
 *
 * Doorbell mode: no payload travels through the MHU, so @p msg must be NULL.
 * Writes (1u << channel_id) to CH0_SET (window 0) to assert the doorbell bit;
 * the receiver clears it on ack, which releases the bit.
 *
 * @param dev        MBOX device instance (must be a "tx" frame).
 * @param channel_id Doorbell bit index (0..31).
 * @param msg        Must be NULL (signalling mode).
 *
 * @retval 0         On success.
 * @retval -EINVAL   channel_id out of range, or frame is not tx.
 * @retval -EMSGSIZE msg is non-NULL (doorbell mode carries no payload).
 */
static int mhuv2_send(const struct device *dev, mbox_channel_id_t channel_id,
                      const struct mbox_msg *msg)
{
    const struct mhuv2_config *cfg = dev->config;

    if (!cfg->is_tx) {
        return -EINVAL;
    }
    if (channel_id >= MHUV2_NUM_CHANNELS) {
        return -EINVAL;
    }
    /* Doorbell-only: no payload is carried through the MHU. */
    if (msg != NULL) {
        return -EMSGSIZE;
    }

    /*
     * Wake the link BEFORE ringing: assert ACCESS_REQUEST and spin for
     * ACCESS_READY. The OpenAMP ipc_service backend never calls set_enabled on
     * the TX frame, so without this the doorbell write does not propagate to the
     * receiver's CH0_STAT (bench-confirmed: rings sent but RX status stayed 0).
     * Matches the proven raw-MHU path in examples/aen/aen-dualcore-ipc.
     */
    sys_write32(1U, cfg->base + MHUV2_TX_ACCESS_REQUEST);
    for (uint32_t i = 0U; i < MHUV2_ACCESS_READY_SPINS; i++) {
        if (sys_read32(cfg->base + MHUV2_TX_ACCESS_READY) != 0U) {
            break;
        }
    }

    /* Window 0, CH0_SET is write-1-to-set: assert the doorbell bit. */
    sys_write32(BIT(channel_id), cfg->base + MHUV2_TX_CH0_SET);

    return 0;
}

/**
 * @brief Register an inbound-doorbell callback on a receiver frame.
 *
 * @param dev        MBOX device instance (must be an "rx" frame).
 * @param channel_id Doorbell bit index (0..31).
 * @param cb         Callback invoked from ISR context on that doorbell.
 * @param user_data  Opaque context passed back to @p cb.
 *
 * @retval 0        On success.
 * @retval -EINVAL  channel_id out of range or frame is not rx.
 */
static int mhuv2_register_callback(const struct device *dev, mbox_channel_id_t channel_id,
                                   mbox_callback_t cb, void *user_data)
{
    const struct mhuv2_config *cfg  = dev->config;
    struct mhuv2_data         *data = dev->data;

    if (cfg->is_tx) {
        return -EINVAL;
    }
    if (channel_id >= MHUV2_NUM_CHANNELS) {
        return -EINVAL;
    }

    data->cb[channel_id]     = cb;
    data->cb_ctx[channel_id] = user_data;

    return 0;
}

/**
 * @brief Report the outbound MTU.
 *
 * Doorbell mode only -- always 0 (signalling). Callers must pass NULL msg to
 * mbox_send() and carry real data in shared SRAM (the OpenAMP vrings).
 *
 * @param dev MBOX device instance.
 * @return 0 (signalling mode).
 */
static int mhuv2_mtu_get(const struct device *dev)
{
    ARG_UNUSED(dev);

    return 0;
}

/**
 * @brief Report the channel count of an MHUv2 frame (window-0 doorbell bits).
 *
 * @param dev MBOX device instance.
 * @return 32.
 */
static uint32_t mhuv2_max_channels_get(const struct device *dev)
{
    ARG_UNUSED(dev);

    return MHUV2_NUM_CHANNELS;
}

/**
 * @brief Enable or disable a channel.
 *
 * On a receiver frame, "enable" UNMASKs the doorbell bit (CH0_MASK_CLEAR) so it
 * raises the combined IRQ; "disable" MASKs it (CH0_MASK_SET).
 *
 * On a sender frame, the first "enable" performs the ACCESS_REQUEST ->
 * ACCESS_READY wake handshake (request the receiver wake, then poll READY with
 * a bounded spin). Disabling a sender channel is a no-op.
 *
 * @param dev        MBOX device instance.
 * @param channel_id Doorbell bit index (0..31).
 * @param enable     true to enable, false to disable.
 *
 * @retval 0        On success.
 * @retval -EINVAL  channel_id out of range.
 * @retval -EIO     (tx only) the receiver never reported ACCESS_READY.
 */
static int mhuv2_set_enabled(const struct device *dev, mbox_channel_id_t channel_id, bool enable)
{
    const struct mhuv2_config *cfg = dev->config;

    if (channel_id >= MHUV2_NUM_CHANNELS) {
        return -EINVAL;
    }

    if (cfg->is_tx) {
        if (!enable) {
            /* Sender doorbells are not IRQ sources -- nothing to mask. */
            return 0;
        }

        /*
		 * Wake handshake: request the receiver, then poll ACCESS_READY
		 * with a bounded spin so a dead/absent peer can't hang us.
		 */
        sys_write32(1U, cfg->base + MHUV2_TX_ACCESS_REQUEST);

        for (uint32_t i = 0U; i < MHUV2_ACCESS_READY_SPINS; i++) {
            if (sys_read32(cfg->base + MHUV2_TX_ACCESS_READY) != 0U) {
                return 0;
            }
        }

        return -EIO;
    }

    /* Receiver: UNMASK to enable the IRQ for this bit, MASK to disable. */
    if (enable) {
        struct mhuv2_data *data = dev->data;

        data->enabled_mask |= BIT(channel_id);
        sys_write32(BIT(channel_id), cfg->base + MHUV2_RX_CH0_MASK_CLEAR);
        /* Also enable the COMBINED receive interrupt (Alif DFP mhu_receiver.c).
         * NOTE: on the E8 non-secure HE<->HP pair this still does not raise an
         * NVIC IRQ, so we ALSO start the poll timer below as the real driver.
         * (Re)starting an already-running k_timer is a harmless restart. */
        sys_write32(sys_read32(cfg->base + MHUV2_RX_INT_EN) | MHUV2_RX_INT_CHCOMB,
                    cfg->base + MHUV2_RX_INT_EN);
        k_timer_start(&data->poll_timer, K_MSEC(1), K_MSEC(1));
    } else {
        struct mhuv2_data *data = dev->data;

        data->enabled_mask &= ~BIT(channel_id);
        /* The poll timer serves EVERY enabled doorbell bit on this frame:
         * only stop it once the last enabled channel goes away. */
        if (data->enabled_mask == 0U) {
            k_timer_stop(&data->poll_timer);
        }
        sys_write32(BIT(channel_id), cfg->base + MHUV2_RX_CH0_MASK_SET);
    }

    return 0;
}

/**
 * @brief Receiver doorbell ISR.
 *
 * Reads CH0_STAT_MASKED (window 0) -- the set of asserted-AND-unmasked bits --
 * dispatches the registered callback for each such bit (channel id = bit, NULL
 * data, signalling mode), then acks the handled bitmask via CH0_CLEAR to
 * release the sender.
 *
 * @param arg The MBOX device instance (passed by IRQ_CONNECT).
 */
static void mhuv2_rx_dispatch(const struct device *dev)
{
    const struct mhuv2_config *cfg  = dev->config;
    struct mhuv2_data         *data = dev->data;
    /* Read the RAW status (+0x00, the only bench-proven RX register on the Alif
     * MHU frame) rather than CH0_STAT_MASKED (+0x04, unvalidated on silicon). */
    uint32_t pending = sys_read32(cfg->base + MHUV2_RX_CH0_STAT);

    if (pending == 0U) {
        return;
    }

    /* Ack first (W1C) so a fresh ring during dispatch is not lost. */
    sys_write32(pending, cfg->base + MHUV2_RX_CH0_CLEAR);

    for (uint32_t bit = 0U; bit < MHUV2_NUM_CHANNELS; bit++) {
        if ((pending & BIT(bit)) == 0U) {
            continue;
        }
        if (data->cb[bit] != NULL) {
            data->cb[bit](dev, bit, data->cb_ctx[bit], NULL);
        }
    }
}

static void mhuv2_rx_isr(const void *arg)
{
    mhuv2_rx_dispatch((const struct device *)arg);
}

/* Poll-timer expiry: same dispatch as the ISR, for the silicon where the
 * combined RX interrupt does not fire (see struct mhuv2_data). */
static void mhuv2_poll_expiry(struct k_timer *t)
{
    struct mhuv2_data *data = CONTAINER_OF(t, struct mhuv2_data, poll_timer);

    mhuv2_rx_dispatch(data->dev);
}

static const struct mbox_driver_api mhuv2_driver_api = {
    .send              = mhuv2_send,
    .register_callback = mhuv2_register_callback,
    .mtu_get           = mhuv2_mtu_get,
    .max_channels_get  = mhuv2_max_channels_get,
    .set_enabled       = mhuv2_set_enabled,
};

/*
 * Per-instance IRQ wiring. Only receiver frames carry an `interrupts` entry, so
 * the IRQ_CONNECT is emitted via COND_CODE_1(DT_INST_IRQ_HAS_IDX(...)) -- sender
 * frames (no interrupts) compile cleanly with no IRQ code at all.
 */
#define MHUV2_IRQ_CONNECT(inst)                                                                    \
    COND_CODE_1(DT_INST_IRQ_HAS_IDX(inst, 0),                                                      \
                (IRQ_CONNECT(DT_INST_IRQN(inst), DT_INST_IRQ(inst, priority), mhuv2_rx_isr,        \
                             DEVICE_DT_INST_GET(inst), 0);                                         \
                 irq_enable(DT_INST_IRQN(inst));),                                                 \
                ())

#define MHUV2_INST(inst)                                                                           \
    static struct mhuv2_data         mhuv2_data_##inst;                                            \
    static const struct mhuv2_config mhuv2_config_##inst = {                                       \
        .base = (mm_reg_t)DT_INST_REG_ADDR(                                                        \
            inst), /* `alif,direction` enum is ["tx","rx"] -> idx 0 == "tx". */                    \
        .is_tx = (DT_INST_ENUM_IDX(inst, alif_direction) == 0),                                    \
    };                                                                                             \
    static int mhuv2_init_##inst(const struct device *dev)                                         \
    {                                                                                              \
        /* rx frames start fully masked; UNMASK happens in            \
		 * set_enabled(true) per the MBOX contract. tx frames have    \
		 * no init-time register writes (the wake handshake is        \
		 * deferred to the first set_enabled).                        \
		 */                            \
        if (!mhuv2_config_##inst.is_tx) {                                                          \
            mhuv2_data_##inst.dev = dev;                                                           \
            k_timer_init(&mhuv2_data_##inst.poll_timer, mhuv2_poll_expiry, NULL);                  \
            sys_write32(0xFFFFFFFFU, mhuv2_config_##inst.base + MHUV2_RX_CH0_MASK_SET);            \
        }                                                                                          \
        MHUV2_IRQ_CONNECT(inst);                                                                   \
        return 0;                                                                                  \
    }                                                                                              \
    DEVICE_DT_INST_DEFINE(inst, mhuv2_init_##inst, NULL, &mhuv2_data_##inst, &mhuv2_config_##inst, \
                          POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY, &mhuv2_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MHUV2_INST)
