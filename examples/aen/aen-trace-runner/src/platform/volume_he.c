/* src/platform/volume_he.c -- see volume_he.h. The rules are src/ipc/tr_vol.c (host-tested); this
 * file only reads the two EVK controls through the portable API and hands the result over.
 *
 * THE CONTROLS. The E1M EVK's rotary encoder (PEC11R-4215K-S0024, 24 PPR) is ALP_E1M_ENC0
 * (EVK_ENC_ROTARY): gpio-qdec A / B = P3_1 / P3_0 (the bench-confirmed order, volume_he.overlay), read by <alp/counter.h> alp_qenc_*, whose gpio-qdec
 * backend decodes the two phases in software (a detent = one count, steps-per-period 4). Its push
 * switch is E1M_GPIO_IO4 (EVK_PIN_ENCODER_SW, P4_3, active low, RC-debounced on the board) read
 * with <alp/peripheral.h> alp_gpio_*. All three pads are SoC GPIO3 / GPIO4: not GPIO5 (the HP's
 * SD_N / IRQZ) and not the lpgpio island (the HP's CC3501E lines). volume_he.overlay maps them.
 *
 * THE ONE NON-PORTABLE LINE (alp-sdk#2808). A pad's input buffer (REN) is on only once pinctrl sets it, and
 * neither gpio_dw nor gpio-qdec applies a pinctrl state, so without it both inputs read idle
 * forever (the trap the aen-evk-demo overlay documents). The SDK has no portable pad-config API:
 * pinctrl_apply_state() on the overlay's /zephyr,user group is the Zephyr call (the pattern of
 * aen-camera-firstlight/trigger_gpio.overlay), nothing vendor-specific; it goes when #2808 lands.
 */
#include "volume_he.h"

#if TR_HP_SOUND

#include <alp/boards/alp_e1m_evk.h>
#include <alp/counter.h>
#include <alp/peripheral.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/printk.h>

#include "../ipc/tr_vol.h"

#define ENC_NODE DT_PATH(zephyr_user) /* carries pinctrl-0 only, volume_he.overlay */
PINCTRL_DT_DEFINE(ENC_NODE);

static volatile tr_vol_t *const s_rec = (volatile tr_vol_t *)TR_MEM_VOL;
static tr_vol_he_t              s_he;
static alp_qenc_t              *s_enc;
static alp_gpio_t              *s_sw;
static int32_t                  s_pos;
static bool                     s_down;
static bool                     s_up; /* init done: before it the record holds cold-SRAM garbage */

void tr_volume_he_init(void)
{
	tr_vol_he_boot(&s_he, s_rec);

	int rc = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(ENC_NODE), PINCTRL_STATE_DEFAULT);

	const alp_qenc_config_t cfg = ALP_QENC_CONFIG_DEFAULT(EVK_ENC_ROTARY);

	s_enc = alp_qenc_open(&cfg);
	s_sw  = alp_gpio_open(EVK_PIN_ENCODER_SW);
	if (s_enc != NULL) {
		(void)alp_qenc_get_position(s_enc, &s_pos);
	}
	if (s_sw != NULL) {
		/* The board has the external 10k pull-up; gpio_dw has no pull of its own and rejects the
		 * flag (-ENOTSUP), so ask for none. A failed configure is no switch, not a half-open one. */
		if (alp_gpio_configure(s_sw, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE) != ALP_OK) {
			alp_gpio_close(s_sw);
			s_sw = NULL;
		}
	}
	printk("[vol] %u%% at 0x%08x: pads %d, encoder %s, switch %s, bench request word +4\n",
	       (unsigned)s_he.pct,
	       (unsigned)TR_MEM_VOL,
	       rc,
	       s_enc != NULL ? "ok" : "none",
	       s_sw != NULL ? "ok" : "none");
	s_up = true;
}

void tr_volume_he_frame(void)
{
	int32_t pos = s_pos;
	bool    now = false;

	if (!s_up) {
		return; /* the first presents run before tr_volume_he_init() */
	}
	if (s_enc != NULL && alp_qenc_get_position(s_enc, &pos) != ALP_OK) {
		pos = s_pos;
	}
	if (s_sw != NULL && alp_gpio_read(s_sw, &now) != ALP_OK) {
		now = false;
	}
	/* wrap-safe: the accumulator is 32-bit, the difference is the detents since last frame */
	int32_t detents = (int32_t)((uint32_t)pos - (uint32_t)s_pos);
	bool    press   = now && !s_down;

	s_pos  = pos;
	s_down = now;
	if (tr_vol_he_step(&s_he, s_rec, detents, press)) {
		printk("[vol] %u%% (seq %u)\n", (unsigned)s_he.pct, (unsigned)s_rec->seq);
	}
}

uint8_t tr_volume_he_pct(void)
{
	return s_up ? (uint8_t)s_he.pct : (uint8_t)TR_VOL_DEFAULT;
}

uint32_t tr_volume_he_seq(void)
{
	return s_up ? s_rec->seq : 0u;
}

#endif /* TR_HP_SOUND */
