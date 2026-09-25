/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-pmic-inspect -- read-only-by-default inspector for the V2N SoM's
 * on-module power chips, with a small set of guarded write actions.
 *
 * Runs as a Linux/Yocto user-space app on the V2N Cortex-A55 cluster.
 * The power chips all sit on BRD_I2C (Renesas RIIC8, Linux /dev/i2c-8):
 *
 *   0x1E        da9292     secondary PMIC: CH1 SoC 0.8 V, CH2 DEEPX 0.75 V
 *   0x25/0x26   act8760    primary PMIC (ACT88760): Buck1..7, LDO1..6,
 *                          GPIO1..11
 *   0x44/0x48/  tps628640  DEEPX-side bucks (V2N-M1; they only answer once
 *   0x4F                   DEEPX_CORE_0P75_EN is high)
 *   0x4D        tps628640  LPDDR4X 0.6 V buck (assembly option)
 *
 * Everything this app knows about the board -- rail net names, guard
 * windows, which rails are critical, the expected DA9292 identity, the
 * expected ACT88760 MODE4 byte -- comes from ONE generated header,
 * <alp/chips/v2n_power_tree.h>, which scripts/gen_power_tree.py projects
 * from metadata/e1m_modules/v2n/power-tree.yaml.  The app never hard-codes
 * a window; it hands the generated tables to the drivers, and the drivers
 * enforce them.  That is the point of the example: an app cannot push a
 * rail outside its window or switch off a critical rail, even by mistake.
 *
 * Modes of operation:
 *
 *   (default)          Read-only dump: identity, every rail, every ACT88760
 *                      GPIO, latched DA9292 events.  Text or --json.
 *                      Exit status 1 if silicon disagrees with metadata
 *                      (DA9292 identity, ACT88760 GPIO4 MODE byte, a
 *                      power chip missing, or a present rail's live
 *                      voltage reading outside its power-tree.yaml
 *                      window -- presence alone is not "matches metadata").
 *   --write + ONE of   --set-mv <rail> <mV>     window-guarded setpoint
 *                      --enable  <rail>         enable a rail
 *                      --disable <rail>         disable (critical: refused)
 *                      --fix-gpio4-polarity     MODE4 0x88 -> 0x08 (volatile)
 *                      --deepx-rail-sequence    da9292_ch2_sequence()
 *
 * Clear-on-read registers.  Some status bits vanish the moment anyone
 * reads them, so reading them "just to look" would steal the event from
 * whoever really needs it (the kernel's nIRQ handler, a supervisor).  This
 * app never reads them unless --include-clear-on-read is given:
 *
 *   ACT88760 ADD1 0x00  SYS_STATUS      VSYSSTAT / VSYSWARN latches
 *   ACT88760 ADD1 0x04  GPIO1_8_TOGGLE  GPIO1..8 toggle bits
 *   ACT88760 ADD1 0x2B  GPIO9_11        GPIO9..11 toggle bits (and the only
 *                                       place GPIO9..11 level lives)
 *   TPS628640     0x05  STATUS          thermal / HICCUP / UVLO latches
 *
 * One exception the app cannot avoid: act8760_init() probes ADD1 by
 * reading 0x00, so the VSYS latches are consumed by init itself.  The
 * DA9292 EVENT_00/01 registers are write-1-to-clear, not clear-on-read,
 * so the app always peeks them (da9292_peek_events()).
 *
 * Bus ownership.  A kernel driver bound to one of these addresses owns
 * it: user space must not talk to it behind the driver's back.  Linux
 * only reports that ownership (EBUSY) on the I2C_SLAVE ioctl -- the
 * I2C_RDWR path the chip drivers use never sees it (src/yocto/
 * peripheral_i2c.c).  So the app front-loads an I2C_SLAVE-based probe for
 * every address and skips any the kernel owns, exactly like
 * examples/v2n/v2n-brd-i2c-bringup does.
 *
 * Who owns BRD_I2C also depends on the boot mode, strapped on ACT88760
 * GPIO5 (V2N_BOOT_CPU_SEL).  In A55-boot mode the A55 side (U-Boot, then
 * Linux) masters the bus and U-Boot runs the DEEPX rail sequence; in
 * CM33-boot mode the CM33 firmware does both (power-tree.yaml
 * `boot_modes:`).  Exactly one master per boot: do not use this app's
 * write actions while CM33 firmware is running the bus.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <alp/peripheral.h>
#include <alp/chips/act8760.h>
#include <alp/chips/da9292.h>
#include <alp/chips/tps628640.h>
#include <alp/chips/v2n_power_tree.h>

/* BRD_I2C = Linux /dev/i2c-8 (meta-alp-sdk e1m-v2n-som.dtsi aliases
 * `i2c8 = &i2c8;`).  Overridable with --bus for a board that differs. */
#define V2N_BRD_I2C_BUS_ID 8u

/* Exit codes, so a bring-up script can tell the failure classes apart. */
enum { EXIT_OK = 0, EXIT_MISMATCH = 1, EXIT_USAGE = 2, EXIT_ACTION = 3 };

/* ------------------------------------------------------------------ */
/* Board data: instantiated from the generated power-tree header        */
/* ------------------------------------------------------------------ */

/* The drivers keep only a POINTER to their limits table, so the tables
 * live in static storage for the whole run.  One set per SoM family:
 * `v2n` (no DEEPX) and `v2n-m1` (DEEPX DX-M1).  The ACT88760 rails and
 * GPIOs are identical on both (same PCB); the DEEPX rails differ. */
static const pmic_rail_limit_t v2n_act_limits[ACT8760_RAIL_COUNT] =
    V2N_POWER_ACT8760_RAIL_LIMITS_INIT;
static const pmic_rail_limit_t v2n_m1_act_limits[ACT8760_RAIL_COUNT] =
    V2N_M1_POWER_ACT8760_RAIL_LIMITS_INIT;
static const char *const act_rail_nets[ACT8760_RAIL_COUNT] = V2N_POWER_ACT8760_RAIL_NETS_INIT;
static const char *const act_gpio_nets[ACT8760_GPIO_COUNT] = V2N_POWER_ACT8760_GPIO_NETS_INIT;

static const pmic_rail_limit_t v2n_da_limits[DA9292_CH_COUNT] = V2N_POWER_DA9292_CH_LIMITS_INIT;
static const pmic_rail_limit_t v2n_m1_da_limits[DA9292_CH_COUNT] =
    V2N_M1_POWER_DA9292_CH_LIMITS_INIT;

/* DA9292 channel nets (power-tree.yaml: CH1's net is still TBD there --
 * printed as such, never invented).  v2n-m1 is the superset. */
static const char *const da_ch_nets[DA9292_CH_COUNT] = V2N_M1_POWER_DA9292_CH_NETS_INIT;

/* TPS628640 instances: net, 7-bit address, guard entry. */
struct tps_def {
	const char       *net;
	uint8_t           addr;
	pmic_rail_limit_t limit;
};

static const struct tps_def v2n_tps[] = {
	{ V2N_POWER_TPS628640_LPD4X_0V6_NET,
	  V2N_POWER_TPS628640_LPD4X_0V6_ADDR,
	  V2N_POWER_TPS628640_LPD4X_0V6_LIMIT_INIT },
};

static const struct tps_def v2n_m1_tps[] = {
	{ V2N_M1_POWER_TPS628640_DDR5_VDD2H_1V05_NET,
	  V2N_M1_POWER_TPS628640_DDR5_VDD2H_1V05_ADDR,
	  V2N_M1_POWER_TPS628640_DDR5_VDD2H_1V05_LIMIT_INIT },
	{ V2N_M1_POWER_TPS628640_VDD0V85_LPDDR_NET,
	  V2N_M1_POWER_TPS628640_VDD0V85_LPDDR_ADDR,
	  V2N_M1_POWER_TPS628640_VDD0V85_LPDDR_LIMIT_INIT },
	{ V2N_M1_POWER_TPS628640_LPD4X_0V6_NET,
	  V2N_M1_POWER_TPS628640_LPD4X_0V6_ADDR,
	  V2N_M1_POWER_TPS628640_LPD4X_0V6_LIMIT_INIT },
	{ V2N_M1_POWER_TPS628640_DDR5_VDDQ_0V5_NET,
	  V2N_M1_POWER_TPS628640_DDR5_VDDQ_0V5_ADDR,
	  V2N_M1_POWER_TPS628640_DDR5_VDDQ_0V5_LIMIT_INIT },
};

#define TPS_MAX 4u

/* Action progress lines go to stdout, or to stderr under --json so stdout
 * stays one parseable JSON document. */
static FILE *msg;

/* One family = one consistent set of the tables above. */
struct family {
	const char              *name;
	const pmic_rail_limit_t *act;
	const pmic_rail_limit_t *da;
	const struct tps_def    *tps;
	size_t                   tps_count;
};

static const struct family families[] = {
	{ "v2n", v2n_act_limits, v2n_da_limits, v2n_tps, sizeof(v2n_tps) / sizeof(v2n_tps[0]) },
	{ "v2n-m1",
	  v2n_m1_act_limits,
	  v2n_m1_da_limits,
	  v2n_m1_tps,
	  sizeof(v2n_m1_tps) / sizeof(v2n_m1_tps[0]) },
};

/* ACT88760 rail short ids, index = act8760_rail_t.  Accepted on the
 * command line next to the net name ("buck5" or "VDD09_CA55") -- Buck5/
 * Buck6's own net is "TBD" (channel<->net attribution not independently
 * verified, see power-tree.yaml), so use their "buck5"/"buck6" id instead. */
static const char *const act_rail_ids[ACT8760_RAIL_COUNT] = {
	"buck1", "buck2", "buck3", "buck4", "buck5", "buck6", "buck7",
	"ldo1",  "ldo2",  "ldo3",  "ldo4",  "ldo5",  "ldo6",
};

/* ------------------------------------------------------------------ */
/* Collected state (filled by collect(), printed as text or JSON)       */
/* ------------------------------------------------------------------ */

enum chip_kind { K_ACT, K_DA, K_TPS };

/* Presence of one I2C address: answered, absent, or owned by a kernel
 * driver (in which case the app never touches it). */
enum presence { P_ABSENT, P_PRESENT, P_KERNEL };

static const char *presence_name(enum presence p)
{
	return p == P_PRESENT ? "present" : p == P_KERNEL ? "kernel-owned" : "absent";
}

struct rail_row {
	enum chip_kind           kind;
	unsigned                 idx; /* act8760_rail_t, da9292_channel_t or tps index */
	const char              *id;
	const char              *net;
	const pmic_rail_limit_t *limit;
	uint8_t                  addr;
	bool                     read_ok;
	bool                     enabled;
	bool                     enabled_known;
	uint16_t                 mv; /* 0 = not decodable, see note */
	bool                     pg;
	bool                     pg_known;
	char                     faults[32]; /* "OV ILIM", "" = none */
	char                     note[48];
};

struct gpio_row {
	uint8_t              gpio;
	bool                 read_ok;
	bool                 full; /* false: GPIO9..11 without --include-clear-on-read */
	act8760_gpio_state_t st;
};

#define RAIL_MAX (ACT8760_RAIL_COUNT + DA9292_CH_COUNT + TPS_MAX)

static struct {
	const struct family *fam;
	bool                 fam_explicit;
	bool                 cor; /* --include-clear-on-read */

	enum presence act_p, da_p;
	enum presence tps_p[TPS_MAX];
	act8760_t     act;
	da9292_t      da;
	tps628640_t   tps[TPS_MAX];

	da9292_identity_t da_id;
	bool              da_id_ok;
	da9292_events_t   da_ev;
	bool              da_ev_ok;

	bool             act_sys_ok; /* clear-on-read extras */
	act8760_status_t act_sys;
	bool             act_tog_ok;
	uint16_t         act_toggles;
	bool             tps_status_ok[TPS_MAX];
	uint8_t          tps_status[TPS_MAX];

	struct rail_row rails[RAIL_MAX];
	size_t          rail_count;
	struct gpio_row gpios[ACT8760_GPIO_COUNT];

	char        mismatch[8][96];
	const char *mismatch_subject[8]; /* the `a` each mismatch[] was built from,
	                                   * e.g. a rail id -- lets the write gate
	                                   * tell "this rail's own mismatch" apart
	                                   * from every other one. */
	size_t      mismatch_count;
	size_t      mismatch_dropped; /* add_mismatch() calls past the array cap:
	                               * must be counted, not silently lost. */
} g;

/* Common cap/drop bookkeeping for both arities below: returns the slot to
 * snprintf() into, or NULL (having counted the drop) once mismatch[] is
 * full. */
static char *mismatch_slot(const char *subject)
{
	if (g.mismatch_count >= sizeof(g.mismatch) / sizeof(g.mismatch[0])) {
		g.mismatch_dropped++;
		return NULL;
	}
	g.mismatch_subject[g.mismatch_count] = subject;
	return g.mismatch[g.mismatch_count++];
}

static void add_mismatch(const char *fmt, const char *a, unsigned x, unsigned y)
{
	char *slot = mismatch_slot(a);
	if (slot != NULL) snprintf(slot, sizeof(g.mismatch[0]), fmt, a, x, y);
}

/* Same as add_mismatch(), one more %u -- used by check_windows() to report
 * the live reading alongside the window bounds. */
static void add_mismatch3(const char *fmt, const char *a, unsigned x, unsigned y, unsigned z)
{
	char *slot = mismatch_slot(a);
	if (slot != NULL) snprintf(slot, sizeof(g.mismatch[0]), fmt, a, x, y, z);
}

/* ------------------------------------------------------------------ */
/* Ownership probe                                                      */
/* ------------------------------------------------------------------ */

/* A zero-length write goes through alp_i2c_write() -> I2C_SLAVE ioctl,
 * which is where Linux reports a kernel-bound address as EBUSY.  It never
 * sets a register pointer, so it cannot disturb a chip (and a
 * clear-on-read register cannot be hit by accident, which a 1-byte
 * pointer-less READ could do).  Anything other than BUSY just means "not
 * kernel-owned": whether the chip is really there is decided by the
 * driver's own init read below. */
static bool kernel_owned(alp_i2c_t *bus, uint8_t addr)
{
	return alp_i2c_write(bus, addr, NULL, 0) == ALP_ERR_BUSY;
}

/* ------------------------------------------------------------------ */
/* Collection (strictly read-only)                                      */
/* ------------------------------------------------------------------ */

static struct rail_row *new_rail(enum chip_kind k, unsigned idx, uint8_t addr)
{
	struct rail_row *r = &g.rails[g.rail_count++];

	memset(r, 0, sizeof(*r));
	r->kind = k;
	r->idx  = idx;
	r->addr = addr;
	return r;
}

static void collect_act(alp_i2c_t *bus)
{
	g.act_p = P_ABSENT;
	if (kernel_owned(bus, ACT8760_I2C_ADDR_PAGE0) || kernel_owned(bus, ACT8760_I2C_ADDR_PAGE1)) {
		g.act_p = P_KERNEL;
		return;
	}
	if (act8760_init(&g.act, bus) != ALP_OK) {
		/* act8760_init() collapses every bus error -- including
		 * -EBUSY -- to ALP_ERR_NOT_READY, so a kernel driver that
		 * claimed the chip in the narrow window between the ownership
		 * probe above and this init's own reads looks identical to a
		 * genuinely absent chip.  Re-probe before reporting a
		 * hardware-absence mismatch. */
		if (kernel_owned(bus, ACT8760_I2C_ADDR_PAGE0) ||
		    kernel_owned(bus, ACT8760_I2C_ADDR_PAGE1)) {
			g.act_p = P_KERNEL;
			return;
		}
		add_mismatch("%s: ACT88760 does not answer at 0x%02X/0x%02X", "act8760", 0x25u, 0x26u);
		return;
	}
	g.act_p = P_PRESENT;

	for (unsigned i = 0; i < ACT8760_RAIL_COUNT; i++) {
		struct rail_row *r = new_rail(K_ACT, i, i < ACT8760_RAIL_BUCK7 ? 0x25u : 0x26u);

		r->id    = act_rail_ids[i];
		r->net   = act_rail_nets[i];
		r->limit = &g.fam->act[i];

		act8760_rail_state_t st;

		if (act8760_rail_get_state(&g.act, (act8760_rail_t)i, &st) != ALP_OK) continue;
		r->read_ok       = true;
		r->enabled       = st.enabled;
		r->enabled_known = true;
		r->pg            = st.pok;
		r->pg_known      = true;
		snprintf(r->faults,
		         sizeof(r->faults),
		         "%s%s%s",
		         st.ov ? "OV " : "",
		         st.ilim ? "ILIM " : "",
		         st.ilim_warn ? "ILIM_WARN " : "");
		if (i >= ACT8760_RAIL_LDO5) {
			/* LDO5/LDO6 run as load switches on this CMI: VSET is
			 * meaningless, the output follows the input rail. */
			snprintf(r->note, sizeof(r->note), "load switch");
		} else if (st.voltage_mv == 0u) {
			/* Buck1/2/7 with BAND_SEL set: VSET0 shows VSET2, so the
			 * driver refuses to decode it.  Show the raw code instead. */
			snprintf(r->note, sizeof(r->note), "BAND_SEL set, VSET 0x%02X", st.vset_raw);
		} else {
			r->mv = st.voltage_mv;
		}
	}

	for (uint8_t n = 1; n <= ACT8760_GPIO_COUNT; n++) {
		struct gpio_row *gr = &g.gpios[n - 1u];

		gr->gpio = n;
		if (n <= 8u || g.cor) {
			/* GPIO9..11 level/mask live in 0x2B (toggles clear on
			 * read), so the full read is gated by --include-clear-on-read. */
			gr->read_ok = act8760_gpio_get(&g.act, n, &gr->st) == ALP_OK;
			gr->full    = true;
		} else {
			/* Mode byte only: MODE9..11 = ADD1 0x28..0x2A. */
			uint8_t mode = 0;

			gr->read_ok     = act8760_read_reg(
			                      &g.act, ACT8760_PAGE_SYSTEM, (uint8_t)(0x1Fu + n), &mode) == ALP_OK;
			gr->st.mode_raw = mode;
			gr->st.inverted = (mode & 0x80u) != 0u;
			gr->st.mux      = mode & 0x0Fu;
		}
	}

	/* The GD32_NRST OTP defect: some units read MODE4 = 0x88 (inverted,
	 * open-drain) and hold the GD32 in reset.  Metadata says 0x08. */
	if (g.gpios[3].read_ok && g.gpios[3].st.mode_raw != V2N_POWER_ACT8760_GPIO4_EXPECTED_MODE) {
		add_mismatch("%s: MODE4 reads 0x%02X, metadata expects 0x%02X (see --fix-gpio4-polarity)",
		             "act8760 GPIO4 GD32_NRST",
		             g.gpios[3].st.mode_raw,
		             V2N_POWER_ACT8760_GPIO4_EXPECTED_MODE);
	}

	if (g.cor) {
		/* Note: act8760_init() already read 0x00 once, so VSYS latches
		 * seen here are only those set since then. */
		g.act_sys_ok = act8760_get_status(&g.act, &g.act_sys) == ALP_OK;
		g.act_tog_ok = act8760_gpio_toggles_peek(&g.act, &g.act_toggles) == ALP_OK;
	}
}

static void collect_da(alp_i2c_t *bus)
{
	g.da_p = P_ABSENT;
	if (kernel_owned(bus, DA9292_I2C_ADDR_V2N)) {
		g.da_p = P_KERNEL;
		return;
	}
	if (da9292_init(&g.da, bus, DA9292_I2C_ADDR_V2N) != ALP_OK) {
		/* Same -EBUSY race as collect_act() -- re-probe before
		 * reporting absence. */
		if (kernel_owned(bus, DA9292_I2C_ADDR_V2N)) {
			g.da_p = P_KERNEL;
			return;
		}
		add_mismatch("%s: DA9292 does not answer at 0x%02X%.0u", "da9292", DA9292_I2C_ADDR_V2N, 0u);
		return;
	}
	g.da_p = P_PRESENT;

	/* Identity check against the bench-confirmed values in metadata. */
	g.da_id_ok = da9292_get_identity(&g.da, &g.da_id) == ALP_OK;
	if (g.da_id_ok) {
		if (g.da_id.dev_id != V2N_POWER_DA9292_DEV_ID) {
			add_mismatch("%s: DEV_ID 0x%02X, metadata expects 0x%02X",
			             "da9292",
			             g.da_id.dev_id,
			             V2N_POWER_DA9292_DEV_ID);
		}
		if (g.da_id.rev_id != V2N_POWER_DA9292_REV_ID) {
			add_mismatch("%s: REV_ID 0x%02X, metadata expects 0x%02X",
			             "da9292",
			             g.da_id.rev_id,
			             V2N_POWER_DA9292_REV_ID);
		}
		if (g.da_id.cfg_rev != V2N_POWER_DA9292_CFG_REV) {
			add_mismatch("%s: CFG_REV 0x%02X, metadata expects 0x%02X",
			             "da9292",
			             g.da_id.cfg_rev,
			             V2N_POWER_DA9292_CFG_REV);
		}
	}

	/* EVENT_00/01 are write-1-to-clear: a peek reads without clearing. */
	g.da_ev_ok = da9292_peek_events(&g.da, &g.da_ev) == ALP_OK;

	for (unsigned ch = 0; ch < DA9292_CH_COUNT; ch++) {
		struct rail_row *r = new_rail(K_DA, ch, DA9292_I2C_ADDR_V2N);

		r->id    = ch == DA9292_CH1 ? "da9292.ch1" : "da9292.ch2";
		r->net   = da_ch_nets[ch];
		r->limit = &g.fam->da[ch];

		da9292_channel_state_t st;
		uint16_t               mv = 0;

		if (da9292_get_channel_state(&g.da, (da9292_channel_t)ch, &st) != ALP_OK) continue;
		r->read_ok       = true;
		r->enabled       = st.enabled;
		r->enabled_known = true;
		r->pg            = st.pg;
		r->pg_known      = true;
		snprintf(r->faults,
		         sizeof(r->faults),
		         "%s%s%s",
		         st.uv ? "UV " : "",
		         st.ov ? "OV " : "",
		         st.oc ? "OC " : "");
		/* get_voltage_mv decodes the ACTIVE VSEL through the live VSTEP. */
		if (da9292_get_voltage_mv(&g.da, (da9292_channel_t)ch, &mv) == ALP_OK) r->mv = mv;
		snprintf(r->note,
		         sizeof(r->note),
		         "VSTEP=%d VSEL=%s lo=%u hi=%u mV",
		         st.vstep,
		         st.vsel_hi ? "hi" : "lo",
		         st.vsel_lo_mv,
		         st.vsel_hi_mv);
	}
}

static void collect_tps(alp_i2c_t *bus)
{
	for (size_t i = 0; i < g.fam->tps_count; i++) {
		const struct tps_def *d = &g.fam->tps[i];

		g.tps_p[i] = P_ABSENT;
		if (kernel_owned(bus, d->addr)) {
			g.tps_p[i] = P_KERNEL;
			continue;
		}
		/* Absent is normal: 0x4D is an assembly option, and the DEEPX
		 * bucks only power up once DEEPX_CORE_0P75_EN is high. */
		if (tps628640_init(&g.tps[i], bus, d->addr, 0) != ALP_OK) {
			/* Same -EBUSY race as collect_act() -- re-probe so a
			 * kernel-claimed chip reports as kernel-owned, not
			 * silently folded into "absent". */
			if (kernel_owned(bus, d->addr)) g.tps_p[i] = P_KERNEL;
			continue;
		}
		g.tps_p[i] = P_PRESENT;

		struct rail_row *r    = new_rail(K_TPS, (unsigned)i, d->addr);
		uint8_t          ctrl = 0;
		uint16_t         mv   = 0;

		r->id    = d->net;
		r->net   = d->net;
		r->limit = &d->limit;
		if (tps628640_get_voltage_mv(&g.tps[i], &mv) != ALP_OK) continue;
		r->read_ok = true;
		r->mv      = mv;
		/* CONTROL reads back on the bench (0x6F); a 0x00 read means it
		 * did not, so the enable state is then reported as unknown. */
		if (tps628640_read_reg(&g.tps[i], TPS628640_REG_CONTROL, &ctrl) == ALP_OK && ctrl != 0u) {
			r->enabled       = (ctrl & TPS628640_CTRL_SOFTWARE_ENABLE) != 0u;
			r->enabled_known = true;
		}
		snprintf(r->note, sizeof(r->note), "CONTROL 0x%02X", ctrl);
		if (g.cor) {
			g.tps_status_ok[i] = tps628640_get_status(&g.tps[i], &g.tps_status[i]) == ALP_OK;
			if (g.tps_status_ok[i]) {
				uint8_t s = g.tps_status[i];

				snprintf(r->faults,
				         sizeof(r->faults),
				         "%s%s%s",
				         (s & TPS628640_STATUS_THERMAL_WARNING) ? "TWARN " : "",
				         (s & TPS628640_STATUS_HICCUP) ? "HICCUP " : "",
				         (s & TPS628640_STATUS_UVLO) ? "UVLO " : "");
			}
		}
	}
}

/* A rail can be PRESENT and read fine while its live voltage sits outside
 * the window power-tree.yaml declares safe -- presence alone says nothing
 * about that.  r->mv == 0 is this file's established "not decodable"
 * sentinel (a load-switch note, a BAND_SEL-aliased buck, or a read
 * failure); every chip family's real voltage floor is well above 0 mV, so
 * it never collides with a genuine reading.  Only rails with a real
 * window (voltage_writable; non-writable rails carry min_mv=max_mv=0) are
 * checked.  A rail confirmed OFF (enabled_known && !enabled) is skipped
 * too -- its live setpoint register can still read whatever it was last
 * programmed to (or a floating/discharging value) while the converter
 * itself is not regulating, so "outside the window" says nothing about a
 * disabled rail; the per-rail table's on/off column already labels it. */
static void check_windows(void)
{
	for (size_t i = 0; i < g.rail_count; i++) {
		struct rail_row         *r = &g.rails[i];
		const pmic_rail_limit_t *l = r->limit;

		if (r->mv == 0u || !l->voltage_writable) continue;
		if (r->enabled_known && !r->enabled) continue;
		if (r->mv < l->min_mv || r->mv > l->max_mv) {
			add_mismatch3("%s: reading %u mV outside its window (min %u, max %u mV)",
			              r->id,
			              r->mv,
			              l->min_mv,
			              l->max_mv);
		}
	}
}

static void collect(alp_i2c_t *bus)
{
	g.rail_count       = 0;
	g.mismatch_count   = 0;
	g.mismatch_dropped = 0;
	collect_act(bus);
	collect_da(bus);
	collect_tps(bus);
	check_windows();
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

static void print_text(void)
{
	printf("=== V2N PMIC inspect (family %s%s) ===\n",
	       g.fam->name,
	       g.fam_explicit ? "" : ", default -- pass --family");
	printf("clear-on-read registers: %s\n",
	       g.cor ? "READ (--include-clear-on-read)"
	             : "not read (ACT 0x00/0x04/0x2B, TPS 0x05; act8760_init reads 0x00 anyway)");

	printf("\nact8760 0x25/0x26: %s\n", presence_name(g.act_p));
	printf("da9292  0x1E:      %s", presence_name(g.da_p));
	if (g.da_id_ok) {
		printf("  DEV_ID 0x%02X REV_ID 0x%02X CFG_REV 0x%02X",
		       g.da_id.dev_id,
		       g.da_id.rev_id,
		       g.da_id.cfg_rev);
	}
	printf("\n");
	for (size_t i = 0; i < g.fam->tps_count; i++) {
		printf("tps628640 0x%02X:   %s (%s)\n",
		       g.fam->tps[i].addr,
		       presence_name(g.tps_p[i]),
		       g.fam->tps[i].net);
	}

	printf("\n%-11s %-16s %-4s %-4s %-6s %-3s %-13s %-4s %s\n",
	       "rail",
	       "net",
	       "addr",
	       "en",
	       "mV",
	       "PG",
	       "window mV",
	       "crit",
	       "faults / note");
	for (size_t i = 0; i < g.rail_count; i++) {
		const struct rail_row   *r = &g.rails[i];
		const pmic_rail_limit_t *l = r->limit;
		char                     win[16], mv[8];

		if (l->voltage_writable) {
			snprintf(win, sizeof(win), "%u..%u", l->min_mv, l->max_mv);
		} else {
			snprintf(win, sizeof(win), "%s", l->enable_writable ? "enable-only" : "none");
		}
		snprintf(mv, sizeof(mv), "%u", r->mv);
		printf("%-11s %-16s 0x%02X %-4s %-6s %-3s %-13s %-4s %s%s\n",
		       r->id,
		       r->net,
		       r->addr,
		       !r->read_ok        ? "ERR"
		       : r->enabled_known ? (r->enabled ? "on" : "off")
		                          : "?",
		       r->mv ? mv : "-",
		       r->pg_known ? (r->pg ? "yes" : "NO") : "-",
		       win,
		       l->critical ? "yes" : "no",
		       r->faults,
		       r->note);
	}

	if (g.act_p == P_PRESENT) {
		printf("\n%-6s %-17s %-5s %-4s %-3s %-4s %-3s %s\n",
		       "gpio",
		       "net",
		       "level",
		       "MODE",
		       "inv",
		       "mux",
		       "pp",
		       "irq");
		for (unsigned i = 0; i < ACT8760_GPIO_COUNT; i++) {
			const struct gpio_row *gr = &g.gpios[i];

			if (!gr->read_ok) {
				printf("GPIO%-2u %-17s read error\n", gr->gpio, act_gpio_nets[i]);
				continue;
			}
			printf("GPIO%-2u %-17s %-5s 0x%02X %-3s 0x%X  %-3s %s\n",
			       gr->gpio,
			       act_gpio_nets[i],
			       gr->full ? (gr->st.level ? "high" : "low") : "?",
			       gr->st.mode_raw,
			       gr->st.inverted ? "yes" : "no",
			       gr->st.mux,
			       gr->full ? (gr->st.push_pull ? "yes" : "no") : "?",
			       gr->full ? (gr->st.irq_masked ? "masked" : "unmasked") : "?");
		}
	}

	if (g.da_ev_ok) {
		printf("\nda9292 latched events (peek, not cleared): EVENT_00 0x%02X EVENT_01 0x%02X\n",
		       g.da_ev.raw_00,
		       g.da_ev.raw_01);
	}
	if (g.act_sys_ok) printf("act8760 SYS_STATUS 0x00 = 0x%02X (read-cleared)\n", g.act_sys.raw);
	if (g.act_tog_ok) printf("act8760 GPIO toggles (read-cleared) mask 0x%03X\n", g.act_toggles);
	for (size_t i = 0; i < g.fam->tps_count; i++) {
		if (g.tps_status_ok[i]) {
			printf("tps628640 0x%02X STATUS 0x%02X (read-cleared)\n",
			       g.fam->tps[i].addr,
			       g.tps_status[i]);
		}
	}

	printf("\n");
	for (size_t i = 0; i < g.mismatch_count; i++)
		printf("MISMATCH: %s\n", g.mismatch[i]);
	if (g.mismatch_dropped) {
		printf("MISMATCH: (%zu more mismatch(es) not shown -- mismatch[] cap reached)\n",
		       g.mismatch_dropped);
	}
	printf("VERDICT: %s\n",
	       g.mismatch_count ? "silicon disagrees with metadata" : "matches metadata");
}

/* Minimal JSON: every string we print is a fixed net / id name or a note
 * built from %u / %X fields, so no escaping is ever needed. */
static void print_json(void)
{
	printf(
	    "{\"family\":\"%s\",\"include_clear_on_read\":%s,", g.fam->name, g.cor ? "true" : "false");
	printf("\"chips\":{\"act8760\":\"%s\",\"da9292\":\"%s\"",
	       presence_name(g.act_p),
	       presence_name(g.da_p));
	for (size_t i = 0; i < g.fam->tps_count; i++) {
		printf(",\"tps628640@0x%02X\":\"%s\"", g.fam->tps[i].addr, presence_name(g.tps_p[i]));
	}
	printf("},");
	if (g.da_id_ok) {
		printf("\"da9292_identity\":{\"dev_id\":%u,\"rev_id\":%u,\"cfg_rev\":%u},",
		       g.da_id.dev_id,
		       g.da_id.rev_id,
		       g.da_id.cfg_rev);
	}
	if (g.da_ev_ok) {
		printf(
		    "\"da9292_events\":{\"event_00\":%u,\"event_01\":%u},", g.da_ev.raw_00, g.da_ev.raw_01);
	}
	if (g.act_sys_ok) printf("\"act8760_sys_status\":%u,", g.act_sys.raw);
	if (g.act_tog_ok) printf("\"act8760_gpio_toggles\":%u,", g.act_toggles);

	printf("\"rails\":[");
	for (size_t i = 0; i < g.rail_count; i++) {
		const struct rail_row   *r = &g.rails[i];
		const pmic_rail_limit_t *l = r->limit;

		printf("%s{\"id\":\"%s\",\"net\":\"%s\",\"addr\":%u,\"read_ok\":%s,",
		       i ? "," : "",
		       r->id,
		       r->net,
		       r->addr,
		       r->read_ok ? "true" : "false");
		printf("\"enabled\":%s,", r->enabled_known ? (r->enabled ? "true" : "false") : "null");
		if (r->mv) {
			printf("\"mv\":%u,", r->mv);
		} else {
			printf("\"mv\":null,");
		}
		printf("\"pg\":%s,", r->pg_known ? (r->pg ? "true" : "false") : "null");
		printf("\"faults\":\"%s\",\"note\":\"%s\",", r->faults, r->note);
		printf("\"window_mv\":[%u,%u],\"critical\":%s,\"voltage_writable\":%s,"
		       "\"enable_writable\":%s}",
		       l->min_mv,
		       l->max_mv,
		       l->critical ? "true" : "false",
		       l->voltage_writable ? "true" : "false",
		       l->enable_writable ? "true" : "false");
	}
	printf("],\"gpios\":[");
	for (unsigned i = 0; g.act_p == P_PRESENT && i < ACT8760_GPIO_COUNT; i++) {
		const struct gpio_row *gr = &g.gpios[i];

		printf("%s{\"gpio\":%u,\"net\":\"%s\",\"read_ok\":%s,\"mode\":%u,\"inverted\":%s,"
		       "\"mux\":%u,",
		       i ? "," : "",
		       gr->gpio,
		       act_gpio_nets[i],
		       gr->read_ok ? "true" : "false",
		       gr->st.mode_raw,
		       gr->st.inverted ? "true" : "false",
		       gr->st.mux);
		if (gr->full) {
			printf("\"level\":%s,\"push_pull\":%s,\"irq_masked\":%s}",
			       gr->st.level ? "true" : "false",
			       gr->st.push_pull ? "true" : "false",
			       gr->st.irq_masked ? "true" : "false");
		} else {
			printf("\"level\":null,\"push_pull\":null,\"irq_masked\":null}");
		}
	}
	printf("],\"mismatches\":[");
	for (size_t i = 0; i < g.mismatch_count; i++)
		printf("%s\"%s\"", i ? "," : "", g.mismatch[i]);
	printf("],\"mismatches_dropped\":%zu}\n", g.mismatch_dropped);
}

/* ------------------------------------------------------------------ */
/* Write actions                                                        */
/* ------------------------------------------------------------------ */

/* Find a rail by id ("buck5", "da9292.ch2") or net ("VDD09_CA55"). */
static struct rail_row *find_rail(const char *name)
{
	for (size_t i = 0; i < g.rail_count; i++) {
		if (strcasecmp(g.rails[i].id, name) == 0 || strcasecmp(g.rails[i].net, name) == 0) {
			return &g.rails[i];
		}
	}
	return NULL;
}

/* Hand the generated tables to every driver.  Without this, EVERY
 * control write in the three drivers returns ALP_ERR_NOSUPPORT -- the
 * drivers are fail-closed, and the table is the only key. */
static void install_limits(void)
{
	if (g.act_p == P_PRESENT) {
		(void)act8760_set_limits(&g.act, g.fam->act, V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK);
	}
	if (g.da_p == P_PRESENT) (void)da9292_set_limits(&g.da, g.fam->da);
	for (size_t i = 0; i < g.fam->tps_count; i++) {
		if (g.tps_p[i] == P_PRESENT) (void)tps628640_set_limits(&g.tps[i], &g.fam->tps[i].limit);
	}
}

static alp_status_t rail_set_mv(const struct rail_row *r, uint16_t mv)
{
	switch (r->kind) {
	case K_ACT:
		return act8760_rail_set_voltage_mv(&g.act, (act8760_rail_t)r->idx, mv);
	case K_DA:
		return da9292_set_voltage_mv(&g.da, (da9292_channel_t)r->idx, mv);
	case K_TPS:
		return tps628640_set_voltage_mv(&g.tps[r->idx], mv);
	}
	return ALP_ERR_INVAL;
}

static alp_status_t rail_set_enable(const struct rail_row *r, bool on)
{
	switch (r->kind) {
	case K_ACT:
		return act8760_rail_set_enable(&g.act, (act8760_rail_t)r->idx, on);
	case K_DA:
		return da9292_set_enable(&g.da, (da9292_channel_t)r->idx, on);
	case K_TPS:
		return tps628640_software_enable(&g.tps[r->idx], on);
	}
	return ALP_ERR_INVAL;
}

/* GPIO4 (GD32_NRST) OTP defect fix.  Acts ONLY on the exact defect byte:
 * anything else is either already correct (no-op) or an unknown state we
 * refuse to touch.  The write is volatile -- the next power cycle brings
 * the OTP value back, so a defective unit needs this on every boot. */
static int fix_gpio4(void)
{
	act8760_gpio_state_t st;

	if (act8760_gpio_get(&g.act, 4, &st) != ALP_OK) {
		fprintf(stderr, "GPIO4: MODE4 read failed\n");
		return EXIT_ACTION;
	}
	if (st.mode_raw == V2N_POWER_ACT8760_GPIO4_EXPECTED_MODE) {
		fprintf(msg, "GPIO4: MODE4 already 0x%02X (OTP correct) -- nothing to do\n", st.mode_raw);
		return EXIT_OK;
	}
	if (st.mode_raw != 0x88u) {
		fprintf(stderr,
		        "GPIO4: MODE4 = 0x%02X is not the known 0x88 defect -- refusing\n",
		        st.mode_raw);
		return EXIT_ACTION;
	}
	/* Clears MODEx bit7 only, read-modify-write + read-back inside the
	 * driver; gated by the polarity-writable mask from metadata. */
	alp_status_t s = act8760_gpio_set_polarity(&g.act, 4, false);

	if (s == ALP_OK && act8760_gpio_get(&g.act, 4, &st) == ALP_OK &&
	    st.mode_raw == V2N_POWER_ACT8760_GPIO4_EXPECTED_MODE) {
		fprintf(
		    msg, "GPIO4: MODE4 0x88 -> 0x%02X (volatile; reverts at power cycle)\n", st.mode_raw);
		return EXIT_OK;
	}
	fprintf(stderr,
	        "GPIO4: polarity write failed (%s), MODE4 now 0x%02X\n",
	        alp_status_name(s),
	        st.mode_raw);
	return EXIT_ACTION;
}

/* ---- DEEPX rail sequence (V2N-M1) ---------------------------------- */

/* RZ/V2N pin -> Linux GPIO line: pinctrl-rzg2l numbers lines port*8+pin
 * (P64 = 52, P65 = 53).  That numbering is assumed, so the kernel's own
 * line NAME must match the pad ("P6_4" / "P64") before anything is
 * driven -- a wrong mapping would drive an unrelated pad. */
#define RZ_GPIO_LINE(port, pin) ((port) * 8u + (pin))
#define P64_LINE                RZ_GPIO_LINE(6u, 4u) /* DEEPX_CORE_0P75_EN, output */
#define P65_LINE                RZ_GPIO_LINE(6u, 5u) /* DEEPX_PWR_EN_REQ, input */

/* Find the SoC pin controller's gpiochip (label "<addr>.pinctrl") unless
 * the operator named one with --gpiochip. */
static int find_pinctrl_chip(int forced)
{
	for (int n = forced >= 0 ? forced : 0; n < (forced >= 0 ? forced + 1 : 16); n++) {
		char                 path[32];
		struct gpiochip_info ci;
		int                  fd;

		snprintf(path, sizeof(path), "/dev/gpiochip%d", n);
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		memset(&ci, 0, sizeof(ci));
		int ok = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci) == 0;

		close(fd);
		if (ok && ci.lines > P65_LINE && (forced >= 0 || strstr(ci.label, "pinctrl") != NULL)) {
			fprintf(msg, "gpiochip%d: label \"%s\", %u lines\n", n, ci.label, ci.lines);
			return n;
		}
	}
	return -1;
}

/* A line some kernel consumer (a driver, a hog, another process) already
 * holds is not ours to drive.  Also report whether the line is currently
 * an output: an output P64 means firmware already drove it. */
static bool line_free(int chip, unsigned line, const char *pad, bool *is_output)
{
	char                     path[32];
	struct gpio_v2_line_info li;

	snprintf(path, sizeof(path), "/dev/gpiochip%d", chip);
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0) return false;
	memset(&li, 0, sizeof(li));
	li.offset = line;
	int ok    = ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) == 0;

	close(fd);
	if (!ok) return false;
	*is_output = (li.flags & GPIO_V2_LINE_FLAG_OUTPUT) != 0u;
	fprintf(msg,
	        "  line %u name \"%s\" consumer \"%s\" %s %s\n",
	        line,
	        li.name,
	        li.consumer,
	        (li.flags & GPIO_V2_LINE_FLAG_USED) ? "USED" : "free",
	        *is_output ? "output" : "input");
	/* pad = "P64": accept the kernel name "P64" or "P6_4". */
	char alt[8];
	snprintf(alt, sizeof(alt), "P%c_%c", pad[1], pad[2]);
	if (strcmp(li.name, pad) != 0 && strcmp(li.name, alt) != 0) {
		fprintf(stderr, "  line %u is named \"%s\", not %s -- refusing\n", line, li.name, pad);
		return false;
	}
	return (li.flags & GPIO_V2_LINE_FLAG_USED) == 0u;
}

/* The driver's only OS dependency besides GPIO: a microsecond delay. */
static void delay_us(void *user, uint32_t us)
{
	(void)user;
	struct timespec ts = { .tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000L };

	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

static const char *const seq_step_names[] = {
	"OK",
	"ERR_ARGS",
	"ERR_IDENTITY",
	"ERR_STATUS01",
	"ERR_PREREAD",
	"ERR_CLEAR_VSTEP",
	"ERR_PROGRAM_VOUT",
	"ERR_CH1_DISTURBED",
	"ERR_EVENTS",
	"ERR_NO_REQUEST",
	"ERR_VSTEP_AT_ENABLE",
	"ERR_ENABLE",
	"ERR_PG_TIMEOUT",
	"ERR_PG_DROPPED",
};

static int deepx_sequence(int forced_chip)
{
	/* Refusals first -- nothing below here may run on a board it was
	 * not written for. */
	if (strcmp(g.fam->name, "v2n-m1") != 0) {
		fprintf(stderr, "deepx: only on --family v2n-m1 (v2n has no DEEPX rail)\n");
		return EXIT_ACTION;
	}
	struct rail_row *ch2 = find_rail("da9292.ch2");

	if (ch2 == NULL || !ch2->read_ok) {
		fprintf(stderr, "deepx: DA9292 CH2 state unreadable\n");
		return EXIT_ACTION;
	}
	/* The rail is already up (the boot owner ran the sequence).
	 * Re-running it would REQUEST P64 from user space, and a fresh line
	 * request starts as an input: DEEPX_CORE_0P75_EN would drop under a
	 * live DEEPX.  Refuse instead. */
	if (ch2->enabled) {
		fprintf(stderr,
		        "deepx: CH2 already enabled -- the boot owner ran the sequence; refusing\n");
		return EXIT_ACTION;
	}

	int chip = find_pinctrl_chip(forced_chip);

	if (chip < 0) {
		fprintf(stderr, "deepx: no pinctrl gpiochip found (use --gpiochip N)\n");
		return EXIT_ACTION;
	}
	bool p64_out = false, p65_out = false;
	bool p64_ok = line_free(chip, P64_LINE, "P64", &p64_out);
	bool p65_ok = line_free(chip, P65_LINE, "P65", &p65_out);

	if (!p64_ok || !p65_ok) {
		fprintf(stderr, "deepx: P64/P65 misnamed or owned by another consumer -- refusing\n");
		return EXIT_ACTION;
	}
	if (p64_out) {
		fprintf(stderr, "deepx: P64 is already an output (firmware drove it) -- refusing\n");
		return EXIT_ACTION;
	}

	/* pin_id encoding of the Yocto GPIO backend: (chip << 16) | line. */
	alp_gpio_t *req = alp_gpio_open(((uint32_t)chip << 16) | P65_LINE);
	alp_gpio_t *en  = alp_gpio_open(((uint32_t)chip << 16) | P64_LINE);

	if (req == NULL || en == NULL ||
	    alp_gpio_configure(req, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE) != ALP_OK ||
	    alp_gpio_configure(en, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE) != ALP_OK ||
	    alp_gpio_write(en, false) != ALP_OK) {
		fprintf(stderr, "deepx: could not claim P64/P65\n");
		if (req) alp_gpio_close(req);
		if (en) alp_gpio_close(en);
		return EXIT_ACTION;
	}

	/* The sequence itself is the same OS-agnostic driver function U-Boot
	 * mirrors.  M1_RESET (PA6) is left to its boot owner: m1_reset = NULL. */
	struct da9292_ch2_seq_cfg cfg = {
		.target_mv       = 750u,
		.expected_dev_id = V2N_POWER_DA9292_DEV_ID,
		.pwr_en_req      = req,
		.core_en         = en,
		.m1_reset        = NULL,
		.req_timeout_ms  = 500u,
		.pg_timeout_ms   = 20u,
		.pg_settle_ms    = 5u,
		.delay           = delay_us,
		.delay_user      = NULL,
	};
	struct da9292_ch2_seq_result res;
	sigset_t                     set;
	int                          sig = 0;

	/* Block SIGINT/SIGTERM/SIGHUP BEFORE the sequence: a Ctrl-C (or a
	 * closed terminal) half-way through must not kill the process between
	 * CH2_EN and the PG check.  They are collected with sigwait() below
	 * once the rail is up.  An unclean death (SIGKILL) releases P64,
	 * which floats DEEPX_CORE_0P75_EN under a live CH2. */
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);

	memset(&res, 0, sizeof(res));
	alp_status_t s = da9292_ch2_sequence(&g.da, &cfg, &res);

	fprintf(msg,
	        "deepx: %s step %s  CTRL_01 0x%02X VOUT_CH2 0x%02X/0x%02X STATUS 0x%02X/0x%02X "
	        "EVENT 0x%02X/0x%02X CFG_00 0x%02X%s\n",
	        alp_status_name(s),
	        (unsigned)res.step < sizeof(seq_step_names) / sizeof(seq_step_names[0])
	            ? seq_step_names[res.step]
	            : "?",
	        res.ctrl_01,
	        res.vout_ch2_00,
	        res.vout_ch2_01,
	        res.status_00,
	        res.status_01,
	        res.event_00,
	        res.event_01,
	        res.pmc_cfg_00,
	        res.already_programmed ? " (already programmed)" : "");

	if (s != ALP_OK) {
		/* The driver already drove P64 low on every abort path. */
		alp_gpio_close(en);
		alp_gpio_close(req);
		return EXIT_ACTION;
	}

	/* Success.  Linux's RZ pin controller returns a RELEASED line to
	 * input, so exiting now would float DEEPX_CORE_0P75_EN under a live
	 * DEEPX.  Hold the lines until SIGINT/SIGTERM, then power down in
	 * order: P64 low first, then CH2_EN off (CH2 is not critical). */
	fprintf(msg,
	        "deepx: rail up -- holding P64 high; Ctrl-C powers the DEEPX rail down in order\n");
	fflush(stdout);

	(void)sigwait(&set, &sig);

	(void)alp_gpio_write(en, false);
	s = da9292_set_enable(&g.da, DA9292_CH2, false);
	fprintf(msg, "deepx: P64 low, CH2 disable: %s\n", alp_status_name(s));
	alp_gpio_close(en);
	alp_gpio_close(req);
	return s == ALP_OK ? EXIT_OK : EXIT_ACTION;
}

/* ------------------------------------------------------------------ */
/* Command line                                                         */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	fprintf(stderr,
	        "usage: v2n-pmic-inspect [--family v2n|v2n-m1] [--bus N] [--json]\n"
	        "                        [--include-clear-on-read]\n"
	        "                        [--write (--set-mv RAIL MV | --enable RAIL |\n"
	        "                                  --disable RAIL | --fix-gpio4-polarity |\n"
	        "                                  --deepx-rail-sequence [--gpiochip N])]\n"
	        "RAIL = id (buck1..buck7, ldo1..ldo6, da9292.ch1/ch2, TPS net) or net name.\n");
}

enum action { A_NONE, A_SET_MV, A_ENABLE, A_DISABLE, A_FIX_GPIO4, A_DEEPX };

int main(int argc, char **argv)
{
	unsigned    bus_id = V2N_BRD_I2C_BUS_ID;
	bool        json = false, write = false;
	enum action act     = A_NONE;
	int         actions = 0, gpiochip = -1;
	const char *rail_name = NULL;
	long        set_mv    = 0;

	g.fam = &families[1]; /* v2n-m1 is a superset for reading */
	msg   = stdout;

	for (int i = 1; i < argc; i++) {
		const char *a    = argv[i];
		bool        more = i + 1 < argc;

		if (strcmp(a, "--json") == 0) {
			json = true;
		} else if (strcmp(a, "--include-clear-on-read") == 0) {
			g.cor = true;
		} else if (strcmp(a, "--write") == 0) {
			write = true;
		} else if (strcmp(a, "--family") == 0 && more) {
			const char *f = argv[++i];

			g.fam = NULL;
			for (size_t k = 0; k < sizeof(families) / sizeof(families[0]); k++) {
				if (strcmp(f, families[k].name) == 0) g.fam = &families[k];
			}
			if (g.fam == NULL) {
				usage();
				return EXIT_USAGE;
			}
			g.fam_explicit = true;
		} else if (strcmp(a, "--bus") == 0 && more) {
			bus_id = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(a, "--gpiochip") == 0 && more) {
			gpiochip = atoi(argv[++i]);
		} else if (strcmp(a, "--set-mv") == 0 && i + 2 < argc) {
			act       = A_SET_MV;
			rail_name = argv[++i];
			set_mv    = strtol(argv[++i], NULL, 0);
			actions++;
		} else if ((strcmp(a, "--enable") == 0 || strcmp(a, "--disable") == 0) && more) {
			act       = a[2] == 'e' ? A_ENABLE : A_DISABLE;
			rail_name = argv[++i];
			actions++;
		} else if (strcmp(a, "--fix-gpio4-polarity") == 0) {
			act = A_FIX_GPIO4;
			actions++;
		} else if (strcmp(a, "--deepx-rail-sequence") == 0) {
			act = A_DEEPX;
			actions++;
		} else {
			usage();
			return EXIT_USAGE;
		}
	}
	/* Two keys for any write: --write AND exactly one action, AND an
	 * explicit family (the family picks the guard windows). */
	if (json) msg = stderr;
	if (actions > 1 || (actions == 1) != write || (write && !g.fam_explicit) ||
	    (act == A_SET_MV && (set_mv <= 0 || set_mv > 5000))) {
		usage();
		return EXIT_USAGE;
	}

	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = bus_id,
	    .bitrate_hz = 400000u,
	});

	if (bus == NULL) {
		fprintf(stderr, "cannot open /dev/i2c-%u\n", bus_id);
		return EXIT_USAGE;
	}

	collect(bus);
	if (act == A_NONE) {
		if (json) {
			print_json();
		} else {
			print_text();
		}
		alp_i2c_close(bus);
		return g.mismatch_count ? EXIT_MISMATCH : EXIT_OK;
	}

	/* Writes only on a board that matches its metadata.  Two allowed
	 * exceptions: the GPIO4 defect, when fixing it is the action; and,
	 * for --set-mv specifically, this exact rail's own out-of-window
	 * setpoint mismatch -- fixing THAT mismatch is the whole point of
	 * --set-mv, so refusing to write because of it would make the flag
	 * unable to ever do its job.  Any OTHER mismatch (a different rail,
	 * an identity/GPIO mismatch) still blocks, matching check_windows()'s
	 * add_mismatch3() subject being exactly the rail id. */
	size_t tolerated = (act == A_FIX_GPIO4 && g.gpios[3].st.mode_raw == 0x88u) ? 1u : 0u;
	if (act == A_SET_MV) {
		for (size_t i = 0; i < g.mismatch_count; i++) {
			if (g.mismatch_subject[i] != NULL && strcmp(g.mismatch_subject[i], rail_name) == 0) {
				tolerated++;
			}
		}
	}
	int rc = EXIT_ACTION;

	if (g.mismatch_count > tolerated) {
		print_text();
		fprintf(stderr, "refusing to write: fix the mismatches above first\n");
		rc = EXIT_MISMATCH;
	} else if (g.act_p != P_PRESENT && act != A_DEEPX) {
		fprintf(stderr, "refusing to write: ACT88760 not available to user space\n");
	} else {
		install_limits();
		if (act == A_FIX_GPIO4) {
			rc = fix_gpio4();
		} else if (act == A_DEEPX) {
			rc = g.da_p == P_PRESENT ? deepx_sequence(gpiochip) : EXIT_ACTION;
		} else {
			struct rail_row *r = find_rail(rail_name);

			if (r == NULL) {
				fprintf(stderr, "unknown or absent rail \"%s\"\n", rail_name);
			} else if (act == A_DISABLE && r->kind != K_ACT) {
				/* A single DEEPX rail off under a running DX-M1 (P64 high,
				 * M1_RESET maybe released) leaves its other rails up out of
				 * order.  Only the ordered power-down of
				 * --deepx-rail-sequence (P64 low, then CH2_EN off) may drop
				 * a DEEPX rail; DA9292 CH1 / TPS 0x4D are critical anyway. */
				fprintf(stderr,
				        "%s: single-rail disable of a DA9292 / TPS628640 rail is refused\n",
				        r->id);
			} else {
				/* The DRIVER decides; the app only reports.  A critical
				 * disable or an out-of-window value comes back as
				 * NOSUPPORT / OUT_OF_RANGE, never as a bus write. */
				alp_status_t s = act == A_SET_MV ? rail_set_mv(r, (uint16_t)set_mv)
				                                 : rail_set_enable(r, act == A_ENABLE);

				fprintf(msg,
				        "%s %s: %s%s\n",
				        r->id,
				        act == A_SET_MV   ? "set-mv"
				        : act == A_ENABLE ? "enable"
				                          : "disable",
				        alp_status_name(s),
				        (act == A_DISABLE && r->limit->critical) ? " (critical rail)" : "");
				rc = s == ALP_OK ? EXIT_OK : EXIT_ACTION;
			}
		}
		/* Show the after-state. */
		collect(bus);
		if (json) {
			print_json();
		} else {
			print_text();
		}
	}
	alp_i2c_close(bus);
	return rc;
}
