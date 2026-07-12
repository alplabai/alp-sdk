/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-1.5 (thin Zephyr glue over the vendored r_mhu_b_ns FSP module) ======
 * RZ/V2N's MHU is the "MHU-B" IP; upstream Zephyr's mbox_renesas_rz_mhu.c only binds the
 * plain-MHU FSP module (r_mhu_ns), which does not compile for rzv2n -- see
 * zephyr/drivers/mbox/r_mhu_b_ns/r_mhu_b_ns.c's file header (alp-sdk #683). This is a
 * separate compatible ("renesas,rz-mhu-b-mbox"), NOT a patch of the upstream driver, so it
 * never collides with a real `renesas,rz-mhu-mbox` node and stays a clean `west update`.
 * It is otherwise a structural copy of mbox_renesas_rz_mhu.c: same MBOX class API shape,
 * same tx-mask/rx-mask/channels-count channel model, same busy-wait-before-send behaviour.
 * BENCH-UNVERIFIED. See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ============================================================================
 */

#define DT_DRV_COMPAT renesas_rz_mhu_b_mbox

#include <stdint.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/logging/log.h>

#include "r_mhu_b_ns.h"

LOG_MODULE_REGISTER(mbox_renesas_rz_mhu_b, CONFIG_MBOX_LOG_LEVEL);

/* Global dummy value required for FSP driver implementation (the "default shared memory
 * location" branch in r_mhu_b_ns.c is dead code here -- DT always supplies shared-memory). */
#define MHU_B_SHM_START_ADDR 0
const uint32_t *const __mhu_shmem_start = (uint32_t *)MHU_B_SHM_START_ADDR;

/* FSP interrupt handler. */
void mhu_b_ns_int_isr(void);

static volatile uint32_t callback_msg;
static void mhu_b_ns_callback(mhu_callback_args_t *p_args)
{
	callback_msg = p_args->msg;
}

struct mbox_rz_mhu_b_config {
	const mhu_api_t *fsp_api;
	uint16_t mhu_ch_size;
	/* Number of supported channels */
	uint32_t num_channels;
	/* TX channels mask */
	uint32_t tx_mask;
	/* RX channels mask */
	uint32_t rx_mask;
};

struct mbox_rz_mhu_b_data {
	const struct device *dev;
	mhu_b_ns_instance_ctrl_t *fsp_ctrl;
	mhu_cfg_t *fsp_cfg;
	mbox_callback_t cb;
	void *user_data;
	uint32_t channel_id;
};

/**
 * @brief Return true if the channel of the MBOX device is an inbound channel.
 */
static inline bool is_rx_channel_valid(const struct device *dev, uint32_t ch)
{
	const struct mbox_rz_mhu_b_config *config = dev->config;

	return ((ch < config->num_channels) && (config->rx_mask & BIT(ch)));
}

/**
 * @brief Return true if the channel of the MBOX device is an outbound channel.
 */
static inline bool is_tx_channel_valid(const struct device *dev, uint32_t ch)
{
	const struct mbox_rz_mhu_b_config *config = dev->config;

	return ((ch < config->num_channels) && (config->tx_mask & BIT(ch)));
}

/**
 * Interrupt handler
 */
static void mbox_rz_mhu_b_isr(const struct device *dev)
{
	struct mbox_rz_mhu_b_data *data = dev->data;
	struct mbox_msg msg;

	mhu_b_ns_int_isr();
	if (data->cb && data->fsp_cfg->p_shared_memory) {
		uint32_t local_msg = callback_msg;

		msg.data = &local_msg;

		/* On the receiving end, the size of the message is always 4 bytes since the FSP MHU
		 * driver requires the message to be of type uint32_t
		 */
		msg.size = sizeof(local_msg);

		data->cb(dev, data->channel_id, data->user_data, &msg);
	}
}

/**
 * @brief Try to send a message over the MBOX device.
 */
static int mbox_rz_mhu_b_send(const struct device *dev, mbox_channel_id_t channel_id,
			       const struct mbox_msg *msg)
{
	const struct mbox_rz_mhu_b_config *config = dev->config;
	struct mbox_rz_mhu_b_data *data = dev->data;
	fsp_err_t fsp_err = FSP_SUCCESS;

	/* FSP driver implementation requires the message to be of type uint32_t */
	uint32_t message = 0;

	if (!is_tx_channel_valid(dev, channel_id)) {
		if (!is_rx_channel_valid(dev, channel_id)) {
			/* Channel is neither RX nor TX */
			LOG_ERR("Invalid MBOX channel number: %d", channel_id);
			return -EINVAL;
		}

		/* Channel is a RX channel, but this function only accepts TX */
		LOG_ERR("Channel ID %d is a RX channel, but only TX channels are allowed",
			channel_id);
		return -ENOSYS;
	}

	if (msg != NULL) {
		/* Maximum size allowed is 4 bytes */
		if (msg->size > config->mhu_ch_size) {
			LOG_ERR("Size %d is not valid. Maximum size is 4 bytes", msg->size);
			return -EMSGSIZE;
		}

		if (msg->data && msg->size) {
			/* Copy message */
			memcpy(&message, msg->data, msg->size);
		} else {
			/* Clear Message */
			message = 0;
		}
	} else {
		message = 0;
	}

	if (data->fsp_cfg->p_shared_memory) {

#if CONFIG_MBOX_BUSY_WAIT_TIMEOUT_US > 0
		/* The FSP MHU "msgSend" API continuously polls until the
		 * previous message is consumed before sending a new one. To avoid
		 * blocking indefinitely, we need to check if the remote clears the message
		 * within the allowed time before sending a new one
		 */
		if (MHU_SEND_TYPE_MSG == data->fsp_ctrl->send_type) {
			if (data->fsp_ctrl->p_regs->MSG_INT_STSn != 0) {
				k_busy_wait(CONFIG_MBOX_BUSY_WAIT_TIMEOUT_US);
				if (data->fsp_ctrl->p_regs->MSG_INT_STSn != 0) {
					LOG_ERR("Remote is busy");
					return -EBUSY;
				}
			}
		} else {
			if (data->fsp_ctrl->p_regs->RSP_INT_STSn != 0) {
				k_busy_wait(CONFIG_MBOX_BUSY_WAIT_TIMEOUT_US);
				if (data->fsp_ctrl->p_regs->RSP_INT_STSn != 0) {
					LOG_ERR("Remote is busy");
					return -EBUSY;
				}
			}
		}
#endif

		/* Send message to shared memory, this will also invoke interrupt on the receiving
		 * core
		 */
		fsp_err = config->fsp_api->msgSend(data->fsp_ctrl, message);
	}

	if (fsp_err) {
		LOG_ERR("Message send failed");
		return -EIO;
	}

	return 0;
}

/**
 * @brief Register a callback function on a channel for incoming messages.
 */
static int mbox_rz_mhu_b_reg_callback(const struct device *dev, mbox_channel_id_t channel_id,
				      mbox_callback_t cb, void *user_data)
{
	struct mbox_rz_mhu_b_data *data = dev->data;

	if (!is_rx_channel_valid(dev, channel_id)) {
		if (!is_tx_channel_valid(dev, channel_id)) {
			/* Channel is neither RX nor TX */
			LOG_ERR("Invalid MBOX channel number: %d", channel_id);
			return -EINVAL;
		}

		/* Channel is a TX channel, but this function only accepts RX */
		LOG_ERR("Channel ID %d is a TX channel, but only RX channels are allowed",
			channel_id);
		return -ENOSYS;
	}

	if (!cb) {
		LOG_ERR("Must provide callback");
		return -EINVAL;
	}

	data->cb = cb;
	data->user_data = user_data;
	data->channel_id = channel_id;

	return 0;
}

/**
 * @brief Initialize the module.
 */
static int mbox_rz_mhu_b_init(const struct device *dev)
{
	const struct mbox_rz_mhu_b_config *config = dev->config;
	struct mbox_rz_mhu_b_data *data = dev->data;
	fsp_err_t fsp_err = FSP_SUCCESS;

	fsp_err = config->fsp_api->open(data->fsp_ctrl, data->fsp_cfg);

	if (fsp_err) {
		LOG_ERR("MBOX initialization failed");
		return -EIO;
	}

	return 0;
}

/**
 * @brief Enable (disable) interrupts and callbacks for inbound channels.
 */
static int mbox_rz_mhu_b_set_enabled(const struct device *dev, mbox_channel_id_t channel_id,
				     bool enabled)
{
	if (!is_rx_channel_valid(dev, channel_id)) {
		if (!is_tx_channel_valid(dev, channel_id)) {
			/* Channel is neither RX nor TX */
			LOG_ERR("Invalid MBOX channel number: %d", channel_id);
			return -EINVAL;
		}

		/* Channel is a TX channel, but this function only accepts RX */
		LOG_ERR("Channel ID %d is a TX channel, but only RX channels are allowed",
			channel_id);
		return -ENOSYS;
	}

	ARG_UNUSED(enabled);
	return 0;
}

/**
 * @brief Return the maximum number of bytes possible in an outbound message.
 */
static int mbox_rz_mhu_b_mtu_get(const struct device *dev)
{
	const struct mbox_rz_mhu_b_config *config = dev->config;

	return config->mhu_ch_size;
}

/**
 * @brief Return the maximum number of channels.
 */
static uint32_t mbox_rz_mhu_b_max_channels_get(const struct device *dev)
{
	const struct mbox_rz_mhu_b_config *config = dev->config;

	return config->num_channels;
}

static DEVICE_API(mbox, mbox_rz_mhu_b_driver_api) = {
	.send = mbox_rz_mhu_b_send,
	.register_callback = mbox_rz_mhu_b_reg_callback,
	.mtu_get = mbox_rz_mhu_b_mtu_get,
	.max_channels_get = mbox_rz_mhu_b_max_channels_get,
	.set_enabled = mbox_rz_mhu_b_set_enabled,
};

/*
 * ************************* DRIVER REGISTER SECTION ***************************
 */

#define MHU_B_RZ_IRQ_CONNECT(idx, irq_name, isr)                                                  \
	do {                                                                                       \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(idx, irq_name, irq),                               \
			    DT_INST_IRQ_BY_NAME(idx, irq_name, priority), isr,                     \
			    DEVICE_DT_INST_GET(idx), 0);                                           \
		irq_enable(DT_INST_IRQ_BY_NAME(idx, irq_name, irq));                               \
	} while (0)

#define MHU_B_RZ_CONFIG_FUNC(idx) MHU_B_RZ_IRQ_CONNECT(idx, mhuns, mbox_rz_mhu_b_isr);

/* No extended-cfg object: r_mhu_b_ns.c resolves both TX and RX registers itself from the
 * MHU-B pair-body table by channel number (see its file header) -- p_extend is unused. */
#define MHU_B_RZ_INIT(idx)                                                                        \
	static mhu_b_ns_instance_ctrl_t g_mhu_b_ns##idx##_ctrl;                                    \
	static mhu_cfg_t g_mhu_b_ns##idx##_cfg = {                                                 \
		.channel = DT_INST_PROP(idx, channel),                                             \
		.rx_ipl = DT_INST_IRQ_BY_NAME(idx, mhuns, priority),                               \
		.rx_irq = DT_INST_IRQ_BY_NAME(idx, mhuns, irq),                                    \
		.p_callback = mhu_b_ns_callback,                                                  \
		.p_context = NULL,                                                                 \
		.p_extend = NULL,                                                                  \
		.p_shared_memory = (void *)COND_CODE_1(DT_INST_NODE_HAS_PROP(idx, shared_memory),  \
		      (DT_REG_ADDR(DT_INST_PHANDLE(idx, shared_memory))), (NULL)),                 \
	};                                                                                         \
	static const struct mbox_rz_mhu_b_config mbox_rz_mhu_b_config_##idx = {                    \
		.fsp_api = &g_mhu_b_ns_on_mhu_b_ns,                                                \
		.mhu_ch_size = 4,                                                                  \
		.num_channels = DT_INST_PROP(idx, channels_count),                                 \
		.tx_mask = DT_INST_PROP(idx, tx_mask),                                             \
		.rx_mask = DT_INST_PROP(idx, rx_mask),                                             \
	};                                                                                         \
	static struct mbox_rz_mhu_b_data mbox_rz_mhu_b_data_##idx = {                              \
		.dev = DEVICE_DT_INST_GET(idx),                                                    \
		.fsp_ctrl = &g_mhu_b_ns##idx##_ctrl,                                               \
		.fsp_cfg = &g_mhu_b_ns##idx##_cfg,                                                 \
	};                                                                                         \
	static int mbox_rz_mhu_b_init_##idx(const struct device *dev)                              \
	{                                                                                          \
		MHU_B_RZ_CONFIG_FUNC(idx)                                                          \
		return mbox_rz_mhu_b_init(dev);                                                    \
	}                                                                                          \
	DEVICE_DT_INST_DEFINE(idx, mbox_rz_mhu_b_init_##idx, NULL, &mbox_rz_mhu_b_data_##idx,      \
			      &mbox_rz_mhu_b_config_##idx, PRE_KERNEL_1,                           \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &mbox_rz_mhu_b_driver_api)

DT_INST_FOREACH_STATUS_OKAY(MHU_B_RZ_INIT);
