/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- shared private declarations.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 *
 * Type-light on purpose: this header pulls in only <stdbool.h> +
 * <stdint.h> and NEVER the vendor "gd32g5x3.h".  The per-peripheral
 * .c files include the vendor header themselves; the shared tables
 * declared below are addressed by their already-resolved uint32_t
 * peripheral/pin base values, so no vendor types leak through here.
 */

#ifndef GD32_BRIDGE_HAL_GD32_COMMON_H
#define GD32_BRIDGE_HAL_GD32_COMMON_H

#include <stdbool.h>
#include <stdint.h>

/* ----------------------------------------------------------------- */
/* Shared peripheral-table element types.                             */
/* ----------------------------------------------------------------- */

/* GPIO pad map element (table lives in gpio.c). */
typedef struct {
	uint32_t periph; /* GPIOA..GPIOF base address. */
	uint32_t pin;    /* GPIO_PIN_n bit mask.       */
} gd32_gpio_pad_t;

/* ADC channel map element (table lives in adc.c). */
typedef struct {
	uint32_t periph;    /* ADC0..ADC3 base                            */
	uint8_t  channel;   /* ADC_CHANNEL_n                              */
	uint32_t gpio_port; /* GPIOA..GPIOF                               */
	uint32_t gpio_pin;  /* GPIO_PIN_n                                 */
} gd32_adc_ch_t;

/* Quadrature-encoder map element (table lives in qenc.c). */
typedef struct {
	uint32_t timer_periph;
	uint32_t gpio_x_port;
	uint32_t gpio_x_pin;
	uint32_t gpio_y_port;
	uint32_t gpio_y_pin;
	uint32_t gpio_af;
} gd32_qenc_t;

/* PWM channel map element (table lives in pwm.c). */
typedef struct {
	uint32_t periph;     /* TIMER0 or TIMER7 base                          */
	uint16_t channel;    /* TIMER_CH_0..TIMER_CH_3                          */
	bool     complement; /* true: drive complementary output, false: main */
	uint32_t gpio_port;  /* GPIOA..GPIOF                                    */
	uint32_t gpio_pin;   /* GPIO_PIN_n                                     */
	uint32_t gpio_af;    /* GPIO_AF_X                                      */
} gd32_pwm_ch_t;

/* DAC channel map element (table lives in dac.c). */
typedef struct {
	uint32_t periph;    /* DAC0..DAC3 base address */
	uint8_t  out;       /* DAC_OUT0 or DAC_OUT1    */
	uint32_t gpio_port; /* GPIOA..GPIOG base addr  */
	uint32_t gpio_pin;  /* GPIO_PIN_n bit mask     */
} gd32_dac_ch_t;

/* Stream-DMA bring-up state.  Two parallel streams: stream 0 binds
 * DMA0_CH0, stream 1 binds DMA1_CH0.  Each stream owns a circular
 * ring buffer that the DMA fills peripheral-to-memory at the ADC
 * clock; bridge_hw_adc_stream_read drains samples between the host's
 * polls.  Declared here (above the single-shot read path) because
 * bridge_hw_adc_read's converter-sharing guard inspects the live
 * stream slots.
 *
 * Ring size is chosen so a 100 kHz stream fills it in ~10 ms (1024
 * samples) -- comfortably above the host's typical poll cadence yet
 * small enough to keep the SRAM footprint inside the GD32G553's
 * 128 KB budget.  Total cost: 2 streams x 1024 samples x 2 bytes =
 * 4 KB. */
#define BRIDGE_ADC_STREAM_RING_SAMPLES 1024u
#define BRIDGE_ADC_STREAM_COUNT        2u

/* Honest sample-rate contract: the requested rate is realised by a
 * dedicated pacing timer (see stream_begin), not ignored.  100 kHz
 * cap = the ring's documented design point (fills in ~10 ms); 0 is
 * INVAL at the call site, anything above the cap is RANGE. */
#define BRIDGE_ADC_STREAM_RATE_MAX_HZ 100000u

/* Pacing-timer clock.  TIMER5/6 are APB1 basic timers; with APB1 at
 * DIV1 they tick at the full 216 MHz core clock (same base the PWM
 * timers use -- see PWM_TIMER_CLK_HZ).  The silicon validation
 * cross-checks this constant: a wrong base shows up directly as a
 * got-count mismatch over a timed dwell. */
#define BRIDGE_ADC_PACE_CLK_HZ 216000000u

typedef struct {
	bool     in_use;
	uint8_t  channel;     /* ADC channel index this stream watches */
	uint16_t full_scale;  /* (1<<res_bits)-1 snapshot at begin, mv math */
	uint32_t dma_periph;  /* DMA0 or DMA1                          */
	uint8_t  dma_channel; /* dma_channel_enum value                */
	uint32_t pace_timer;  /* TIMER5 (stream 0) or TIMER6 (stream 1) */
	uint16_t ring[BRIDGE_ADC_STREAM_RING_SAMPLES];
	uint16_t read_idx; /* host's consumer cursor                */
	/* Overrun accounting (adc_stream.c).  lap_count is bumped by the
	 * stream's DMA full-transfer-finish ISR on every circular-mode
	 * reload (one full "lap" of the ring); total_read accumulates the
	 * samples the host has drained.  lap_count * RING_SAMPLES + the
	 * live write index is the TOTAL the DMA ever deposited, so a
	 * backlog beyond one ring means the writer lapped the reader.
	 * volatile: written in ISR context, read from the CS-EXTI-driven
	 * stream_read path. */
	volatile uint32_t lap_count;
	uint32_t          total_read;
	uint8_t           dsp_chain_id;
	bool              dsp_bound;
} adc_stream_state_t;

/* ----------------------------------------------------------------- */
/* Table-size counts (literals; each defining TU carries a            */
/* _Static_assert that the sizeof-derived size matches these).        */
/* ----------------------------------------------------------------- */

#define GPIO_PAD_MAP_COUNT    18u /* _Static_assert against sizeof in gpio.c       */
#define ADC_CHANNEL_MAP_COUNT 8u  /* _Static_assert against sizeof in adc.c        */
#define QENC_CHANNEL_COUNT    4u  /* _Static_assert against sizeof in qenc.c       */
#define PWM_CHANNEL_COUNT     8u  /* _Static_assert against sizeof in pwm.c        */
#define DAC_CHANNEL_COUNT     2u  /* _Static_assert against sizeof in dac.c        */

/* ----------------------------------------------------------------- */
/* Shared analog + timer constants.                                   */
/* ----------------------------------------------------------------- */

/* VREF for the ADC's right-aligned code -> millivolt conversion.
 * V2N's analog supply is 1.8 V (maintainer-confirmed the same rail
 * used by DAC_VREF_MV).  ADC_FULL_SCALE is the 12-bit default; when a
 * channel is reconfigured to a lower resolution via
 * bridge_hw_adc_configure the code range shrinks (10b -> 1023, 8b ->
 * 255, 6b -> 63), so the read paths divide by adc_full_scale_for_bits()
 * of the channel's cached resolution rather than this constant.
 * Oversampling keeps the code range at the selected resolution's
 * full-scale (see adc_oversample_params: shift == log2(ratio)
 * normalises the accumulator), so full-scale tracks resolution alone. */
#define ADC_VREF_MV    1800u
#define ADC_FULL_SCALE 4095u

/* Resolution + oversample bounds honoured by bridge_hw_adc_configure. */
#define ADC_RES_BITS_DEFAULT     12u
#define ADC_OVERSAMPLE_RATIO_MAX 256u /* power-of-two ratios 1..256 */

/* Default sample time used for single-shot reads.  240 cycles is
 * the most conservative setting in the vendor's range -- gives the
 * external source plenty of settling time for a high-impedance
 * input divider, at the cost of slower conversion (~1 us per
 * sample at ADC_CLK_SYNC_HCLK_DIV6 with HCLK=216 MHz: 240 ADCCK
 * sample + 12.5 ADCCK conversion ~= 7.0 us). */
#define ADC_DEFAULT_SAMPLE_CYCLES 240u

/* TIMER core clock.  This SoM's SystemInit override runs SYSCLK at
 * 216 MHz (216M-PLL-IRC8M -- see vendors/gd32_firmware_library/
 * overrides/system_gd32g5x3.c) with APB1/APB2 at DIV1, so CK_TIMER =
 * 216 MHz at every timer counter input.  GigaDevice's own PWM
 * example states the same base ("TIMER0 frequency is fixed to
 * 216MHz").  NOTE 2026-06-04: this was wrongly coded as 240 MHz
 * through v0.2.3 -- every PWM period was ~11 % long (a commanded
 * 1 kHz physically ran ~900 Hz).  1 ns LSB resolution would need a
 * faster counter; we instead round period_ns + duty_ns to the
 * nearest 1 us cycle by fixing the prescaler at (216 - 1) so the
 * counter ticks at exactly 1 MHz.  ARR is then `period_us - 1`,
 * fitting in 16 bits for periods up to ~65 ms which covers every
 * realistic control PWM frequency (>=15 Hz). */
#define PWM_TIMER_CLK_HZ    216000000u
#define PWM_TIMER_PRESCALER (216u - 1u) /* 216 MHz -> 1 MHz tick    */
#define PWM_TIMER_TICK_NS   1000u       /* 1 us per timer tick      */
#define PWM_TIMER_ARR_MAX   0xFFFFu     /* 16-bit auto-reload limit */

/* ----------------------------------------------------------------- */
/* Shared tables (defined in the TU named per line).                  */
/* ----------------------------------------------------------------- */

extern const gd32_gpio_pad_t gpio_pad_map[GPIO_PAD_MAP_COUNT];        /* gpio.c */
extern bool                  gpio_is_output[GPIO_PAD_MAP_COUNT];      /* gpio.c */
extern const gd32_adc_ch_t   adc_channels_map[ADC_CHANNEL_MAP_COUNT]; /* adc.c */
extern uint16_t              adc_sample_cycles_cache[8];              /* adc.c */
extern uint8_t               adc_resolution_bits_cache[8];            /* adc.c */
extern uint16_t              adc_oversample_ratio_cache[8];           /* adc.c */
extern const gd32_qenc_t     qenc_map[QENC_CHANNEL_COUNT];            /* qenc.c */
extern const gd32_pwm_ch_t   pwm_channels[PWM_CHANNEL_COUNT];         /* pwm.c */
extern const gd32_dac_ch_t   dac_channels[DAC_CHANNEL_COUNT];         /* dac.c */
extern adc_stream_state_t    adc_streams[BRIDGE_ADC_STREAM_COUNT];    /* adc_stream.c */
extern bool                  trng_started;                            /* trng.c */
extern bool                  trng_ready;                              /* trng.c */
extern bool                  vref_ok;                                 /* vref.c */

/* ----------------------------------------------------------------- */
/* Shared helpers (defined in the TU named per line).                 */
/* ----------------------------------------------------------------- */

bool trng_start(void);                 /* trng.c */
bool trng_poll_ready(void);            /* trng.c */
bool vref_ready_check(void);           /* vref.c */
bool adc_periph_init(uint32_t periph); /* adc.c */

/* Resolution/oversample helpers (adc.c) shared with the stream path.
 * adc_full_scale_for_bits maps a cached resolution to its code range;
 * adc_apply_conv_format programs a channel's cached resolution +
 * oversample into the ADC (caller MUST hold the converter disabled --
 * DRES/OVSAMPCTL only latch with ADCON == 0). */
uint16_t adc_full_scale_for_bits(uint8_t bits);                   /* adc.c */
void     adc_apply_conv_format(uint32_t periph, uint8_t channel); /* adc.c */
void     qenc_channel_init(const gd32_qenc_t *e);                 /* qenc.c */
void     pwm_timer_init(uint32_t periph);                         /* pwm.c */
void     pwm_channel_init(const gd32_pwm_ch_t *ch);               /* pwm.c */
void     se_reset_init(void);                                     /* se_reset.c */

#endif /* GD32_BRIDGE_HAL_GD32_COMMON_H */
