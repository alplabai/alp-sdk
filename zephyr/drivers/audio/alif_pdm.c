/*
 * Copyright (C) 2025 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM -> retire onto
 * sdk-alif fork, BENCH-UNVERIFIED) ======
 * The Alif Ensemble PDM (pulse-density-modulation microphone) block is driven by
 * a vendored copy of the Apache-2.0 zephyr_alif fork driver
 * (drivers/audio/alif_pdm.c, compatible "alif,alif_pdm").  hal_alif ships no PDM
 * / DMIC class driver, so this is a genuine fork-driver copy carried in-tree so
 * it survives a `west update`.  Retire onto the opt-in sdk-alif fork compatible
 * once the pdm node is repointed AND bench-verified.  See
 * docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==================================================================
 *
 * Vendored from the fork with this provenance header added, plus one
 * documented divergence below; the register map lives in the companion
 * alif_pdm_reg.h.  vendor-ext, BENCH-UNVERIFIED.
 *
 * ------------------------- alp-sdk divergence -------------------------
 * pdm_channel_config()'s FIR-coefficient store loop (issue #758) was
 * changed from the fork's raw `*ptr++ = value` walk to sys_write32() per
 * word. A plain pointer store gives the compiler no volatile/ordering
 * guarantee for a peripheral register bank; at -O3 the fork's original
 * loop merges pairs of writes into 8x strd (64-bit double-word MMIO
 * stores) plus an alias-check branch against the FIR_COEF/IIR_COEF_SEL
 * split -- a correctness risk even though no in-tree build currently
 * reaches -O3. Reapply this divergence if the file is ever re-synced
 * from the fork.
 * -------------------------------------------------------------------------
 *
 * ------------------------- alp-sdk divergence (2) ----------------------
 * alif_pdm_warning_isr()'s slab rollover (issue #1122) allocated exactly one
 * replacement block and copied the whole remainder of the IRQ burst into it
 * without checking the block was large enough -- a burst (up to
 * MAX_DATA_ITEMS * MAX_NUM_CHANNELS * sizeof(uint16_t) = 128 bytes) can
 * exceed a configured slab block_size, corrupting adjacent slab memory.
 * Rewritten to split the remainder across as many fresh blocks as needed
 * (never copying more than one block's worth per allocation), and
 * dmic_alif_pdm_configure() now rejects a zero block_size so the split loop
 * can never divide-by/against zero. Reapply this divergence if the file is
 * ever re-synced from the fork.
 * -------------------------------------------------------------------------
 */

#define DT_DRV_COMPAT alif_alif_pdm

#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/pdm/pdm_alif.h>
#include <zephyr/logging/log.h>
#include <zephyr/irq.h>
#include <zephyr/init.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/sys/util.h>
#include "alif_pdm_reg.h"
#include "alif_pdm_burst_plan.h"

/* Upper bound on how many slab blocks a single IRQ burst can be split
 * across (#1122). A burst is at most MAX_DATA_ITEMS * MAX_NUM_CHANNELS *
 * sizeof(uint16_t) = 128 bytes; this comfortably covers every block_size
 * a real audio slab configures (down to single-digit bytes) while keeping
 * the ISR's chunk-plan array small and stack-bounded. Configurations
 * needing more chunks than this are rejected at runtime (burst dropped,
 * logged) rather than risk an unbounded ISR stack allocation.
 */
#define MAX_PDM_BURST_CHUNKS 20

LOG_MODULE_REGISTER(alif_pdm, LOG_LEVEL_INF);

#define DEV_DATA(dev) ((struct pdm_data *)((dev)->data))
#define DEV_CFG(dev)  ((const struct pdm_config *)((dev)->config))

struct pdm_data {
	DEVICE_MMIO_RAM;
	struct k_mem_slab *mem_slab;
	uint32_t block_size;
	struct k_msgq buf_queue;
	uint8_t channel_map;
	uint32_t num_channels;
	uint8_t *data_buffer;
	uint32_t buf_index;
	uint32_t slab_missed;
	uint32_t record_data;
	uint32_t bytes_got;
	uint8_t bypass_iir_filter;
	void *queue_data[MAX_QUEUE_LEN];
	uint16_t data[MAX_NUM_CHANNELS * MAX_DATA_ITEMS];
};

struct pdm_config {
	DEVICE_MMIO_ROM;
	void (*irq_config)(void);
	uint32_t fifo_watermark;
	/*
	 * Whether this instance has its OWN error / audio-detect interrupt
	 * lines.  PDM does; LPPDM folds both into the warning interrupt, so the
	 * warning ISR has to service them itself.  These come from
	 * DT_INST_IRQ_HAS_NAME() at instantiation -- the warning ISR used to ask
	 * DT_NODE_HAS_PROP(DT_NODELABEL(dev), ...) with `dev` the C function
	 * parameter, which expands to an undefined node token and is therefore
	 * always 0, so both branches ran on every instance (#1826).
	 */
	bool                             has_error_irq;
	bool                             has_audio_det_irq;
	const struct pinctrl_dev_config *pcfg;
	const struct device *clk_dev;
	clock_control_subsys_t clkid;
};

/**
 * @fn		int dmic_alif_pdm_configure(const struct device *dev,
 *						struct dmic_cfg *config)
 * @brief	Configures requested number of channels, block size and
 *			enable the PDM  channels etc.
 * @param[in]   dev	: pointer to Runtime device structure
 * @param[in]   config  : Pointer to the dmic_cfg structure which contains
 *						  the input configuration.
 * @return	  Zero on success, and a negative value on failure.
 */
static int dmic_alif_pdm_configure(const struct device *dev, struct dmic_cfg *config)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	if (config->channel.req_num_chan == 0 || config->channel.req_num_chan > MAX_NUM_CHANNELS) {
		LOG_DBG("config invalid: number of channels not valid\n");
		return -EINVAL;
	}

	/* A zero block_size would make the IRQ-burst rollover split loop
	 * divide progress by zero (#1122) -- reject before capture can ever
	 * start.
	 */
	if (config->streams[0].block_size == 0) {
		LOG_DBG("config invalid: block size must be non-zero\n");
		return -EINVAL;
	}

	if (pdata) {
		pdata->mem_slab = config->streams[0].mem_slab;
		pdata->block_size = config->streams[0].block_size;
		pdata->channel_map = config->channel.req_chan_map_lo & 0xFF;

		reg_val |= pdata->channel_map;

		/* Enable the PDM multiple channels */
		sys_write32(reg_val, reg_base + PDM_CONFIG_REGISTER);

		pdata->num_channels = config->channel.req_num_chan;

		LOG_DBG("block size: %d\n", pdata->block_size);
	}

	LOG_DBG("DMIC configure okay\n");

	return 0;
}

/**
 * @fn		void pdm_channel_config(const struct device *dev,
 *					struct pdm_ch_config *cnfg)
 * @brief	Sets FIR coefficient and IIR coefficient values.
 * @param[in]	dev  : Pointer to the runtime device structure.
 * @param[in]	cnfg : Pointer to the pdm_ch_config structure.
 * @return	    None
 */
void pdm_channel_config(const struct device *dev, struct pdm_ch_config *cnfg)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint8_t i;
	uintptr_t ch_n_fir_coef_0 = reg_base + PDM_CH_FIR_COEF + (cnfg->ch_num * PDM_CH_OFFSET);

	/* Store the FIR coefficient values. Each write goes through sys_write32()
	 * (a volatile MMIO accessor) rather than a plain pointer store: the
	 * compiler is otherwise free to reorder, combine or elide stores to a
	 * non-volatile object, which would silently corrupt the coefficient bank.
	 */
	for (i = 0; i < PDM_MAX_FIR_COEFFICIENT; i++) {
		sys_write32(cnfg->ch_fir_coef[i],
			    ch_n_fir_coef_0 + ((uintptr_t)i * sizeof(uint32_t)));
	}

	uintptr_t ch_n_iir_coef = (reg_base + PDM_CH_IIR_COEF_SEL + (cnfg->ch_num * PDM_CH_OFFSET));

	/* Store the IIR coefficient values */
	sys_write32(cnfg->ch_iir_coef, ch_n_iir_coef);
}

/**
 * @fn		void pdm_set_ch_phase(const struct device *dev,
 *					uint8_t ch_num,
 *					uint32_t ch_phase)
 * @brief	Sets the PDM channel phase control value
 * @param[in]	dev  : Pointer to the runtime device structure.
 * @param[in]	ch_num : PDM channel number.
 * @param[in]	ch_phase : PDM channel phase control value.
 * @return	    None
 */
void pdm_set_ch_phase(const struct device *dev, uint8_t ch_num, uint32_t ch_phase)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_phase = (reg_base + PDM_CH_PHASE + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_phase, ch_n_phase);
}

/**
 * @fn		void pdm_set_ch_gain(const struct device *dev,
 *					uint8_t ch_num,
 *					uint32_t ch_gain)
 * @brief	Sets the PDM channel gain control value
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	ch_num	: PDM channel number.
 * @param[in]	ch_gain	: PDM channel gain control value.
 * @return	    None
 */
void pdm_set_ch_gain(const struct device *dev, uint8_t ch_num, uint32_t ch_gain)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_gain = (reg_base + PDM_CH_GAIN + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_gain, ch_n_gain);
}

/**
 * @fn		void pdm_set_peak_detect_th(const struct device *dev,
 *						uint8_t ch_num,
 *						uint32_t ch_peak_detect_th)
 * @brief	Sets the PDM channel  Peak detector threshold value
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	ch_num	: PDM channel number.
 * @param[in]	ch_peak_detect_th : PDM channel  Peak detector
 *				threshold value.
 * @return		None
 */
void pdm_set_peak_detect_th(const struct device *dev, uint8_t ch_num, uint32_t ch_peak_detect_th)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_pkdet_th = (reg_base + PDM_CH_PKDET_TH + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_peak_detect_th, ch_n_pkdet_th);
}

/**
 * @fn		void pdm_set_peak_detect_itv(const struct device *dev,
 *						uint8_t ch_num,
 *						uint32_t ch_peak_detect_itv)
 * @brief	Sets the PDM channel  Peak detector interval value
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	ch_num	: PDM channel number.
 * @param[in]	ch_peak_detect_itv : PDM channel  Peak detector
 *				interval value.
 * @return		None
 */
void pdm_set_peak_detect_itv(const struct device *dev, uint8_t ch_num, uint32_t ch_peak_detect_itv)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uintptr_t ch_n_pkdet_itv = (reg_base + PDM_CH_PKDET_ITV + (ch_num * PDM_CH_OFFSET));

	sys_write32(ch_peak_detect_itv, ch_n_pkdet_itv);
}

/**
 * @fn		void pdm_mode(const struct device *dev, uint8_t mode)
 * @brief	Sets the PDM modes
 * @param[in]	dev	: Pointer to the runtime device structure.
 * @param[in]	mode	: pdm frequency modes
 * @return		None
 */
void pdm_mode(const struct device *dev, uint8_t mode)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	/*
	 * HWRM 15.7.5.3.1 PDM_CTL0 bits 19-16 PDM_MODE select ONE of modes
	 * 0x0-0x9, each with its own clock divisor, decimation ratio and
	 * sampling rate (Table 15-118).  OR-ing without clearing first made
	 * pdm_mode(dev, 5) then pdm_mode(dev, 2) leave 5 | 2 = 7 -- a third rate
	 * neither caller asked for, and no error (#1826).
	 */
	reg_val &= ~PDM_CLK_MODE_MASK;
	reg_val |= ((uint32_t)mode << PDM_CLK_MODE) & PDM_CLK_MODE_MASK;

	sys_write32(reg_val, reg_base + PDM_CONFIG_REGISTER);
}

/**
 * @fn		void enable_interrupt(const struct device *dev)
 * @brief		Enable the IRQ
 * @param[in]	dev : Pointer to the runtime device structure.
 * @return	None
 */
static void enable_interrupt(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);
	uint32_t irq_value = 0;
	uint32_t audio_ch = 0;

	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	/* Store user enabled channel */
	audio_ch = reg_val & PDM_CHANNEL_ENABLE;

	irq_value |= (audio_ch << 8 | PDM_FIFO_ALMOST_FULL_IRQ | PDM_FIFO_OVERFLOW_IRQ);

	/* Enable the Interrupt */
	sys_write32(irq_value, reg_base + PDM_INTERRUPT_REGISTER);
}

/**
 * @fn		void disable_interrupt(const struct device *dev)
 * @brief		Disable the IRQ
 * @param[in]	dev : Pointer to the runtime device structure.
 * @return	None
 */
static void disable_interrupt(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	/* Disable the Interrupt */
	sys_write32(0, reg_base + PDM_INTERRUPT_REGISTER);
}

/**
 * @fn		int dmic_alif_pdm_trigger(const struct device *dev,
 *					enum dmic_trigger cmd)
 * @brief	Send DMIC_TRIGGER_STOP or DMIC_TRIGGER_START to
 *			perform the specific operation.
 * @param[in]   dev	: pointer to Runtime device structure
 * @param[in]   cmd	: DMIC start or stop command
 * @return	  Zero on success, and a negative value on failure.
 */
static int dmic_alif_pdm_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	struct pdm_data *pdata = DEV_DATA(dev);

	switch (cmd) {
	case DMIC_TRIGGER_STOP:
		disable_interrupt(dev);
		pdata->record_data = 0;

		/* Free in-progress buffer to prevent slab leak */
		if (pdata->data_buffer != NULL) {
			k_mem_slab_free(pdata->mem_slab, pdata->data_buffer);
			pdata->data_buffer = NULL;
		}

		/* Drain queued buffers that the app hasn't read */
		void *buf;

		while (k_msgq_get(&pdata->buf_queue, &buf, K_NO_WAIT) == 0) {
			k_mem_slab_free(pdata->mem_slab, buf);
		}
		break;

	case DMIC_TRIGGER_START:
		LOG_DBG("trigger start\n");
		pdata->record_data = 1;
		pdata->bytes_got = 0;
		pdata->buf_index = 0;
		pdata->data_buffer = NULL;
		pdata->slab_missed = 0;

		enable_interrupt(dev);
		break;

	default:
		LOG_ERR("Invalid command: %d", cmd);
		return -EINVAL;
	}
	return 0;
}

/**
 * @fn		int dmic_alif_pdm_read(const struct device *dev,
 *					uint8_t stream,
 *					void **buffer, size_t *size,
 *					int32_t timeout)
 * @brief	Read the stored allocated block address in msg queue
 *			get the pcm samples.
 * @param[in]	dev	: pointer to Runtime device structure
 * @param[in]	stream	: stream configuration
 * @param[in]	buffer	: A pointer to the buffer where the
 *			  retrieved message will be copied.
 * @param[in]	size	: Size of the allocated block
 * @param[in]	timeout	: Maximum time to wait for a message
 * @return		Zero on success, and a negative value on failure.
 */
static int dmic_alif_pdm_read(const struct device *dev, uint8_t stream, void **buffer, size_t *size,
			      int32_t timeout)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	int rc;

	rc = k_msgq_get(&pdata->buf_queue, buffer, SYS_TIMEOUT_MS(timeout));

	if (rc != 0) {
		LOG_DBG("No audio data to be read\n");
	} else {
		*size = pdata->block_size;
	}
	return rc;
}

static inline void pdm_error_handler(const struct device *dev)
{
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	sys_clear_bits(reg_base + PDM_INTERRUPT_REGISTER, PDM_FIFO_OVERFLOW_IRQ);
	(void)sys_read32(reg_base + PDM_ERROR_IRQ);
}

static inline void pdm_audio_det_handler(const struct device *dev)
{
	struct pdm_data *pdata = DEV_DATA(dev);
	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	if (pdata->slab_missed != 0) {
		sys_clear_bits(reg_base + PDM_INTERRUPT_REGISTER, PDM_AUDIO_DETECT_IRQ_STAT);
	}
	(void)sys_read32(reg_base + PDM_AUDIO_DETECT_IRQ);
}
/**
 * @fn		static void pdm_error_detect_irq_handler()
 * @brief	ISR to handle the error interrupt
 * @param[in]	None
 * @return	None.
 */
static __maybe_unused void pdm_error_detect_irq_handler(const struct device *dev)
{
	pdm_error_handler(dev);
}

/**
 * @fn		static void pdm_audio_detect_irq_handler(const struct device *dev)
 * @brief	ISR to handle PDM audio detect interrupts.
 * @param[in]	dev	: pointer to Runtime device structure
 * @return	None.
 */
static __maybe_unused void pdm_audio_detect_irq_handler(const struct device *dev)
{
	pdm_audio_det_handler(dev);
}

/**
 * @fn		void *get_slab(struct pdm_data *pdm_data)
 * @brief	Allocates a memory block from the slab for PCM data.
 * @param[in]	pdm_data Pointer to the PDM data structure
 *			containing the memory slab.
 * @return		Pointer to the allocated memory block on
 *			Zero on success, and a negative value on failure.
 */
static void *get_slab(struct pdm_data *pdm_data)
{
	int rc;
	void *buffer;

	rc = k_mem_slab_alloc(pdm_data->mem_slab, &buffer, K_NO_WAIT);

	if (rc == 0) {
		LOG_DBG("Memory block allocated : %p\n", buffer);
	} else {
		pdm_data->slab_missed++;
		return NULL;
	}

	return buffer;
}

/**
 * @fn		static void alif_pdm_warning_isr()
 * @brief	ISR to handle PDM warning interrupts.
 *			Collects audio data from the PDM channels, stores it
 *			in the buffer, and handles memory allocation and queue
 *			management.
 * @param[in]	dev	: pointer to Runtime device structure
 * @return	None.
 */
static void alif_pdm_warning_isr(const struct device *dev)
{
	struct pdm_data *pdmdata = DEV_DATA(dev);
	const struct pdm_config *cfg     = DEV_CFG(dev);
	uint8_t k = 0;
	uint8_t audio_ch;
	uint8_t intstatus;
	uintptr_t reg_base;
	uint32_t num_items;
	uint32_t data_bytes;
	uint32_t block_size;
	uint32_t bytes_available;
	uint32_t i;
	uint32_t audio_ch_0_1;
	uint32_t audio_ch_2_3;
	uint32_t audio_ch_4_5;
	uint32_t audio_ch_6_7;

	block_size = pdmdata->block_size;

	reg_base = DEVICE_MMIO_GET(dev);

	uint32_t reg_val = sys_read32(reg_base + PDM_CONFIG_REGISTER);

	/* User enabled channel */
	audio_ch = reg_val & PDM_CHANNEL_ENABLE;

	intstatus = sys_read32(reg_base + PDM_WARN_IRQ);
	/*
	 * HWRM 15.7.5.3.4 PDM_FIFO_STAT: bits 31-4 RESERVED, bits 3-0 CNT --
	 * "Count of sets of audio data entry in FIFO for each channel available
	 * to be read".  The whole 32-bit word used to become the loop count, and
	 * the loop below writes up to MAX_NUM_CHANNELS uint16_t per iteration
	 * into a MAX_NUM_CHANNELS * MAX_DATA_ITEMS array.  CNT legitimately
	 * reaches 15, so with all eight channels enabled anything above
	 * MAX_DATA_ITEMS runs past the end of data[] -- the LAST member of
	 * struct pdm_data, so the overrun lands in whatever follows it.  The
	 * fifo_watermark binding caps the TRIGGER level at 7; that is not a
	 * ceiling on CNT, because any ISR-latency stall lets the FIFO fill
	 * further before this runs.  Mask to the field, then clamp to what
	 * data[] can hold (#1826).
	 */
	num_items = sys_read32(reg_base + PDM_FIFO_STATUS_REGISTER) & PDM_FIFO_STAT_CNT_MASK;
	num_items = MIN(num_items, MAX_DATA_ITEMS);

	/* LPPDM doesn't have separate error and audio detect isr handlers */
	if (!cfg->has_error_irq) {
		pdm_error_handler(dev);
	}

	if (!cfg->has_audio_det_irq) {
		pdm_audio_det_handler(dev);
	}

	for (i = 0; i < num_items; i++) {
		audio_ch_0_1 = sys_read32(reg_base + PDM_CH0_CH1_AUDIO_OUT);
		audio_ch_2_3 = sys_read32(reg_base + PDM_CH2_CH3_AUDIO_OUT);
		audio_ch_4_5 = sys_read32(reg_base + PDM_CH4_CH5_AUDIO_OUT);
		audio_ch_6_7 = sys_read32(reg_base + PDM_CH6_CH7_AUDIO_OUT);

		if ((audio_ch & PDM_CHANNEL_0) == PDM_CHANNEL_0) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_0_1);
		}
		if ((audio_ch & PDM_CHANNEL_1) == PDM_CHANNEL_1) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_0_1 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_2) == PDM_CHANNEL_2) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_2_3);
		}
		if ((audio_ch & PDM_CHANNEL_3) == PDM_CHANNEL_3) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_2_3 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_4) == PDM_CHANNEL_4) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_4_5);
		}
		if ((audio_ch & PDM_CHANNEL_5) == PDM_CHANNEL_5) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_4_5 >> 16);
		}
		if ((audio_ch & PDM_CHANNEL_6) == PDM_CHANNEL_6) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_6_7);
		}
		if ((audio_ch & PDM_CHANNEL_7) == PDM_CHANNEL_7) {
			pdmdata->data[k++] = (uint16_t)(audio_ch_6_7 >> 16);
		}
	}

	if (pdmdata->record_data == 0) {
		return;
	}

	data_bytes = num_items * pdmdata->num_channels * sizeof(unsigned short);

	pdmdata->bytes_got += data_bytes;

	if (pdmdata->data_buffer == NULL) {

		pdmdata->data_buffer = get_slab(pdmdata);
		if (pdmdata->data_buffer == NULL) {
			/*
			 * The consumer is behind and the slab is momentarily
			 * empty.  Drop THIS burst (get_slab() already counted it
			 * in slab_missed) and leave the interrupts armed so
			 * capture resumes as soon as a block is freed.
			 *
			 * Writing 0 to PDM_INTERRUPT_REGISTER here used to mask
			 * every PDM interrupt including FIFO_ALMOST_FULL_IRQ_EN,
			 * and nothing re-armed them while record_data stayed 1 --
			 * so one transient slab exhaustion stopped capture for the
			 * life of the image and dmic_alif_pdm_read() only ever
			 * returned -EAGAIN (#1826).  Re-arming costs nothing: the
			 * warning interrupt is paced by the audio clock, so this
			 * path cannot spin faster than the sample rate.
			 */
			return;
		}
		pdmdata->buf_index = 0;
	}

	bytes_available = block_size - pdmdata->buf_index;

	/*
	 * Plan the split BEFORE touching any slab/queue state: a burst can
	 * exceed not just bytes_available but the configured block_size
	 * itself, so never assume a single replacement block is enough
	 * (#1122). Each chunk after the first starts a fresh block and is
	 * never larger than block_size, so no single memcpy can overrun a
	 * block.
	 */
	{
		uint32_t chunks[MAX_PDM_BURST_CHUNKS];
		size_t nchunks;
		uint32_t copied = 0;
		size_t idx;

		nchunks = pdm_plan_burst_chunks(bytes_available, data_bytes, block_size, chunks,
						 ARRAY_SIZE(chunks));
		if (nchunks == 0) {
			/* Cannot safely split this burst into MAX_PDM_BURST_CHUNKS
			 * blocks (a pathologically small block_size) -- drop the
			 * burst rather than risk writing past a block boundary.
			 */
			LOG_ERR("PDM burst too large to plan safely (data_bytes=%u "
				"block_size=%u); dropping burst\n",
				data_bytes, block_size);
			pdmdata->buf_index = 0;
			return;
		}

		for (idx = 0; idx < nchunks; idx++) {
			uint32_t chunk = chunks[idx];

			if (chunk > 0) {
				memcpy(pdmdata->data_buffer + pdmdata->buf_index,
				       (uint8_t *)pdmdata->data + copied, chunk);
				pdmdata->buf_index += chunk;
				copied += chunk;
			}

			if (idx + 1 >= nchunks) {
				/* Last chunk: leave it in progress in the
				 * current block, same as the fast path above.
				 */
				break;
			}

			/* This block is now full; queue it and start a fresh
			 * one for the next chunk.
			 */
			if (k_msgq_put(&pdmdata->buf_queue, &pdmdata->data_buffer, K_NO_WAIT) !=
			    0) {
				/* Queue full: drop oldest block to make room */
				void *oldest = NULL;

				if (k_msgq_get(&pdmdata->buf_queue, &oldest, K_NO_WAIT) == 0) {
					k_mem_slab_free(pdmdata->mem_slab, oldest);
					k_msgq_put(&pdmdata->buf_queue, &pdmdata->data_buffer,
						   K_NO_WAIT);
				}
			}

			pdmdata->data_buffer = get_slab(pdmdata);
			pdmdata->buf_index = 0;
			if (pdmdata->data_buffer == NULL) {
				/* Allocation failed mid-burst: drop the
				 * remainder of this burst, same ownership/
				 * error behavior as before (#1122).
				 */
				return;
			}
		}
	}
}

/* Init function */
static int pdm_initialize(const struct device *dev)
{
	const struct pdm_config *cfg = DEV_CFG(dev);
	struct pdm_data *pdata = DEV_DATA(dev);
	int32_t ret = 0;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);

	uintptr_t reg_base = DEVICE_MMIO_GET(dev);

	if (cfg->pcfg != NULL) {
		pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	}

	/* check device availability */
	if (!device_is_ready(cfg->clk_dev)) {
		LOG_ERR("clock controller device not ready");
		return -ENODEV;
	}

	/* Configure PDM clock sources.
	 * alp-sdk patch (Tier-2): the UPSTREAM alif clockctrl implements
	 * clock_control_configure() as a no-op returning -ENOSYS (unlike the fork
	 * clockctrl this driver was written against) -- the clock source field is
	 * carried in the clkid cell and applied by clock_control_on(), and the LPPDM
	 * source resets to the 76.8 MHz default (src=0) anyway. Tolerate -ENOSYS/
	 * -ENOTSUP so init does not abort (same fix as spi_dw_alif on this SoC). */
	ret = clock_control_configure(cfg->clk_dev, cfg->clkid, NULL);
	if (ret != 0 && ret != -ENOSYS && ret != -ENOTSUP) {
		LOG_ERR("Unable to configure clock: err:%d", ret);
		return ret;
	}

	/* Enable PDM clock from clock manager */
	ret = clock_control_on(cfg->clk_dev, cfg->clkid);
	if (ret != 0) {
		LOG_ERR("Unable to turn on clock: err:%d", ret);
		return ret;
	}

	cfg->irq_config();

	k_msgq_init(&pdata->buf_queue, (char *)pdata->queue_data, sizeof(void *), MAX_QUEUE_LEN);

	/* Enable the Bypass IIR Filter */
	sys_write32(pdata->bypass_iir_filter << PDM_BYPASS_IIR, reg_base + PDM_CTL_REGISTER);

	sys_write32(cfg->fifo_watermark, reg_base + PDM_THRESHOLD_REGISTER);

	LOG_DBG("alif pdm driver init okay");

	return 0;
}

static const struct _dmic_ops dmic_alif_pdm_api = {
	.configure = dmic_alif_pdm_configure,
	.trigger = dmic_alif_pdm_trigger,
	.read = dmic_alif_pdm_read,
};

#if defined(CONFIG_PM_DEVICE)

/**
 * @brief PDM PM device action handler
 *
 * Handles power management state transitions for the PDM device.
 * Coordinates with power domain via PM framework.
 *
 * @param dev device struct
 * @param action PM device action
 *
 * @return 0 if successful, negative errno otherwise
 */
static int pdm_pm_action(const struct device *dev, enum pm_device_action action)
{
	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		/* Device is powered - restore state */
		return pdm_initialize(dev);

	case PM_DEVICE_ACTION_SUSPEND:
		/* Save state and prepare for power down */
	case PM_DEVICE_ACTION_TURN_OFF:
	case PM_DEVICE_ACTION_TURN_ON:
		/* Power domain handling is automatic via PM framework */
		return 0;

	default:
		break;
	}

	return -ENOTSUP;
}
#endif /* CONFIG_PM_DEVICE */

/********** Device Definition per instance Macros **********/

#define PDM_INIT(n) \
	PINCTRL_DT_INST_DEFINE(n); \
	static void            pdm_irq_config_##n(void); \
	static struct pdm_data dmic_alif_pdm_data_##n = { \
		.bypass_iir_filter = DT_INST_PROP(n, bypass_iir_filter), \
	}; \
	static const struct pdm_config dmic_alif_pdm_cfg_##n = { \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(n)), \
		.fifo_watermark    = DT_INST_PROP(n, fifo_watermark), \
		.has_error_irq     = DT_INST_IRQ_HAS_NAME(n, error_intr), \
		.has_audio_det_irq = DT_INST_IRQ_HAS_NAME(n, audio_det_intr), \
		.irq_config        = pdm_irq_config_##n, \
		.pcfg              = PINCTRL_DT_INST_DEV_CONFIG_GET(n), \
		.clk_dev           = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)), \
		.clkid             = (clock_control_subsys_t)DT_INST_CLOCKS_CELL(n, clkid), \
	}; \
	static void pdm_irq_config_##n(void) \
	{ \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, warning_intr, irq), \
		            DT_INST_IRQ_BY_NAME(n, warning_intr, priority), \
		            alif_pdm_warning_isr, \
		            DEVICE_DT_INST_GET(n), \
		            0); \
		irq_enable(DT_INST_IRQ_BY_NAME(n, warning_intr, irq)); \
		IF_ENABLED(DT_INST_IRQ_HAS_NAME(n, error_intr), \
		           (IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, error_intr, irq), \
		                        DT_INST_IRQ_BY_NAME(n, error_intr, priority), \
		                        pdm_error_detect_irq_handler, \
		                        DEVICE_DT_INST_GET(n), \
		                        0); \
		            irq_enable(DT_INST_IRQ_BY_NAME(n, error_intr, irq));)) \
		IF_ENABLED(DT_INST_IRQ_HAS_NAME(n, audio_det_intr), \
		           (IRQ_CONNECT(DT_INST_IRQ_BY_NAME(n, audio_det_intr, irq), \
		                        DT_INST_IRQ_BY_NAME(n, audio_det_intr, priority), \
		                        pdm_audio_detect_irq_handler, \
		                        DEVICE_DT_INST_GET(n), \
		                        0); \
		            irq_enable(DT_INST_IRQ_BY_NAME(n, audio_det_intr, irq));)) \
	} \
	PM_DEVICE_DT_INST_DEFINE(n, pdm_pm_action); \
	DEVICE_DT_INST_DEFINE(n, \
	                      pdm_initialize, \
	                      PM_DEVICE_DT_INST_GET(n), \
	                      &dmic_alif_pdm_data_##n, \
	                      &dmic_alif_pdm_cfg_##n, \
	                      POST_KERNEL, \
	                      CONFIG_AUDIO_DMIC_INIT_PRIORITY, \
	                      &dmic_alif_pdm_api);

DT_INST_FOREACH_STATUS_OKAY(PDM_INIT)
