/* src/platform/rail5v_power.c */
#include <stdbool.h>
#include <stdint.h>

#include <alp/boards/alp_e1m_evk.h>
#include <alp/peripheral.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "ina236_math.h"
#include "rail5v_power.h"

/*
 * NOT module-only power -- U30 (R127, 20 mOhm) sits on the carrier's whole
 * downstream "+5V" net, not a dedicated module-input tap. Netlist-traced
 * (E1M-EVK-2626-R2_{pinmap,components}.csv): "+5V" reaches E2 (the E1M
 * module footprint, 4 pins) AND J6 pins 39/40 (the display connector) AND
 * U7 (a +3V3 regulator/load) AND U52 AND U6 AND the carrier's amps -- every
 * one of those on the same net U30 measures across R127. There is no INA236
 * on this EVK that isolates the module's own input from the rest of that
 * net (rejected an earlier draft of this comment that called it "module
 * power" on exactly that mistaken assumption -- see the HUD label below).
 *
 * This is still the most useful single number the board can show without
 * added hardware: SoM + display + their immediate carrier regulators,
 * which is most of what the game actually draws. U30 INA236B, 0x4A, 20
 * mOhm shunt R127 -- alp/boards/alp_e1m_evk_routes.h (EVK_I2C_ADDR_INA236_5V
 * / EVK_INA236_SHUNT_5V_OHMS). The HUD line is honest about the scope:
 * "5V .. mW SoM+LCD", not "SOM .. mW".
 */
#define RAIL5V_ADDR       EVK_I2C_ADDR_INA236_5V
#define RAIL5V_SHUNT_OHMS EVK_INA236_SHUNT_5V_OHMS

/* INA236 register map (SBOSA81D table 7-1): SHUNT/BUS are the data read,
 * CONFIG is written + verified (ina236_math.h TR_INA236_CONFIG); see
 * ina236_math.h for why CALIBRATION/CURRENT/POWER are never used (so
 * CALIBRATION's value cannot affect this reading either). */
#define REG_SHUNT  0x01u
#define REG_BUS    0x02u
#define REG_MFG_ID 0x3Eu
#define MFG_ID_TI  0x5449u

/* ~3 Hz: inside the task's 2-5 Hz band, off the ~30 Hz render hot path. */
#define RAIL5V_PERIOD_MS 320

volatile int32_t tr_rail5v_avg_mw;
volatile uint32_t tr_rail5v_config_rb;

/* Polish round (silicon: one of three cold boots read 856-966 mW, the
 * others 2098-2221 mW, the 4.5 V gate notwithstanding). Root cause: this
 * file never configured the part. At its power-on default (AVG 1) the
 * SHUNT register is ONE 1.1 ms conversion, and tr_rail5v_poll() runs from
 * the vsync-locked 30 Hz game tick -- every sample lands at the same
 * point of the frame, so the "average" was one 1.1 ms slice of a load
 * that pulses with the frame (A32 render bursts, panel refresh), at a
 * phase fixed per boot. Not ADCRANGE (0 on every boot: a 4x error would be
 * 4x, not ~2.3x) and not CALIBRATION (never used); not the other I2C2
 * user either (the IMU is on this core, the Zephyr controller serialises
 * the transfers; the HP's i2c2 is disabled, fix round 6). Fix: AVG 128 --
 * each result the mean over 282 ms, i.e. over ~8 frames -- written and
 * read back at open, re-checked every poll. */
static uint16_t g_cfg_rb;
static uint8_t  g_cfg_rewrites;
static bool     g_cfg_verified;

static void publish_config(void)
{
	tr_rail5v_config_rb = (uint32_t)g_cfg_verified << 31 | (uint32_t)g_cfg_rewrites << 16 | g_cfg_rb;
}

static alp_i2c_t *g_bus;
static bool       g_ok;
static bool       g_have_sample; /* fix round 8 item 5: seed the EMA with the first real sample
				   * instead of chasing up from 0 -- silicon showed 564-580 mW for
				   * the first few seconds after boot (the true rail read ~2.1-2.3
				   * W once settled) while alpha=1/4 climbed from a cold 0. */
static int64_t    g_next_ms;

static alp_status_t reg_read16(uint8_t reg, int16_t *val_out)
{
	uint8_t      buf[2];
	alp_status_t s = alp_i2c_write_read(g_bus, RAIL5V_ADDR, &reg, 1, buf, 2);

	if (s != ALP_OK) {
		return s;
	}
	*val_out = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
	return ALP_OK;
}

static alp_status_t config_write(void)
{
	uint8_t buf[3] = { TR_INA236_REG_CONFIG, (uint8_t)(TR_INA236_CONFIG >> 8), (uint8_t)TR_INA236_CONFIG };

	return alp_i2c_write(g_bus, RAIL5V_ADDR, buf, sizeof(buf));
}

/* Reads CONFIG into g_cfg_rb; true when it holds TR_INA236_CONFIG. */
static bool config_check(void)
{
	int16_t rb;

	if (reg_read16(TR_INA236_REG_CONFIG, &rb) != ALP_OK) {
		return false;
	}
	g_cfg_rb = (uint16_t)rb;
	return tr_ina236_config_ok(g_cfg_rb);
}

int tr_rail5v_open(void)
{
	alp_i2c_config_t cfg = ALP_I2C_CONFIG_DEFAULT(EVK_I2C_BUS_SENSORS);

	/* A second open on EVK_I2C_BUS_SENSORS alongside platform/imu.c's BMI323
	 * handle -- the SDK's I2C dispatch pools independent handles per bus_id
	 * (src/i2c_dispatch.c), so this never touches the IMU's context. */
	g_bus = alp_i2c_open(&cfg);
	if (g_bus == NULL) {
		printk("rail5v  : bus unavailable -- power HUD disabled\n");
		return -1;
	}

	/* Fix round 5 tried a retry loop here on the theory this open racing
	 * platform/imu.c's own BMI323 config on the same physical bus was a
	 * power-up/bus-settling timing issue. Round 6 found the REAL cause of
	 * "5V 0 mW SoM+LCD": the HP core's own i2c2 ISR (this bus is the SoC's
	 * shared I2C2, EVK_I2C_BUS_SENSORS) clearing INTR_MASK out from under
	 * the HE on every HP-side i2c_dw entry/exit -- fixed by disabling i2c2
	 * on the HP entirely (hp_vision's board overlay, hp_vision_check.sh's
	 * build-time guard). A retry cannot fix a peripheral another core keeps
	 * actively knocking out; removed (fix round 12, review) now that the
	 * real fix makes it moot. */
	int16_t      id = 0;
	alp_status_t s  = reg_read16(REG_MFG_ID, &id);

	if (s != ALP_OK || (uint16_t)id != MFG_ID_TI) {
		printk("rail5v  : U30 INA236 not found at 0x%02x -- power HUD disabled\n", RAIL5V_ADDR);
		alp_i2c_close(g_bus);
		g_bus = NULL;
		return -1;
	}

	for (int i = 0; i < 3 && !g_cfg_verified; i++) {
		g_cfg_verified = config_write() == ALP_OK && config_check();
	}
	publish_config();
	if (!g_cfg_verified) {
		printk("rail5v  : CONFIG readback 0x%04x != 0x%04x -- power HUD disabled\n", g_cfg_rb, TR_INA236_CONFIG);
		alp_i2c_close(g_bus);
		g_bus = NULL;
		return -1;
	}

	g_ok          = true;
	g_have_sample = false;
	g_next_ms     = k_uptime_get() + RAIL5V_PERIOD_MS; /* the first 282 ms average under the new CONFIG */
	printk("rail5v  : READY (+5V net, 0x%02x, CONFIG 0x%04x)\n", RAIL5V_ADDR, g_cfg_rb);
	return 0;
}

void tr_rail5v_poll(void)
{
	if (!g_ok) {
		return;
	}
	int64_t now = k_uptime_get();

	if (now < g_next_ms) {
		return;
	}
	g_next_ms = now + RAIL5V_PERIOD_MS;

	/* The part keeps its CONFIG across our resets but not its own (a
	 * brown-out on the carrier's 3V3): re-checked every poll, rewritten
	 * and this sample skipped if it ever reverted. */
	if (!config_check()) {
		(void)config_write();
		g_cfg_rewrites = g_cfg_rewrites < 0xFFu ? g_cfg_rewrites + 1u : 0xFFu;
		publish_config();
		return;
	}
	publish_config();

	int16_t shunt_raw, bus_raw;

	if (reg_read16(REG_SHUNT, &shunt_raw) != ALP_OK || reg_read16(REG_BUS, &bus_raw) != ALP_OK) {
		/* Leave the last good average on the HUD rather than blanking it;
		 * a transient bus miss here does not touch imu.c's own fail
		 * counter or vice versa -- each context gives up independently. */
		return;
	}

	int32_t bus_mv    = tr_ina236_bus_mv(bus_raw);

	/* fix round 13 (review, silicon finding): one of three cold boots showed
	 * the HUD stuck at 793-965 mW where the other two read the true ~2.1 W
	 * steady state. No calibration/ADC-range bug found (REG_BUS/REG_SHUNT,
	 * ADCRANGE=0, and RAIL5V_SHUNT_OHMS are all boot-invariant -- nothing
	 * there can vary boot to boot). The remaining candidate is a race this
	 * file already has a comment scar from (fix round 8 item 5, above): the
	 * seed-not-climb fix reads whatever the FIRST poll after open() sees,
	 * unconditionally, and the +5V net this rail measures (this file's own
	 * header comment: SoM + LCD + carrier regulators) has its own power-up
	 * ramp -- if that first poll (or several, before the alpha=1/4 EMA
	 * settles) lands mid-ramp, the seed/average carries a genuinely-low BUT
	 * not-yet-representative bus voltage. Rather than guess which exact
	 * poll raced the ramp, gate on the one thing that is always true once
	 * the rail is actually up: INA236 bus voltage same as the rail's own
	 * nominal 5V, with headroom for U30's shunt drop and ADC quantization
	 * (SBOSA81D 7-5-5, 1.6 mV/LSB) -- 4.5 V. Below that, this read is
	 * ramp noise: skip it entirely (no seed, no EMA update, same as an
	 * I2C-read miss just above) rather than let it corrupt the average. */
	if (bus_mv < 4500) {
		return;
	}
	int32_t shunt_uv   = tr_ina236_shunt_uv(shunt_raw, false /* ADCRANGE=0, checked in CONFIG this poll */);
	int32_t current_ua = tr_ina236_current_ua(shunt_uv, RAIL5V_SHUNT_OHMS);
	int32_t sample_mw  = tr_ina236_power_mw(bus_mv, current_ua);

	if (!g_have_sample) {
		tr_rail5v_avg_mw = sample_mw; /* seed, not a slow climb from 0 (fix round 8 item 5) */
		g_have_sample    = true;
		return;
	}
	/* EMA, alpha = 1/4: settles in a couple of seconds at this period,
	 * smooth enough to read without chasing every I2C-noise count. */
	tr_rail5v_avg_mw += (sample_mw - tr_rail5v_avg_mw) / 4;
}
