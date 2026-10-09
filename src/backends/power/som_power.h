/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * SoM power-domain runtime (#2784, unit U5): INTERNAL interface.
 *
 * Not a public header.  The portable surface is alp_power_domain_policy_set(),
 * alp_power_domain_info() and alp_power_boot_wake_info() in <alp/power.h>,
 * answered by the three op wrappers declared at the bottom.  Everything above
 * them is what the STOP backend (U7), the chip adapters and the bench example
 * use.
 *
 * What the layer does
 * -------------------
 * Before STOP / STANDBY it quiesces the on-module consumers (Wi-Fi/BLE
 * coprocessor, Ethernet PHY, external flash / RAM, temperature sensor, RTC
 * clock-out, backlight) so they stop burning power through the sleep, records
 * what it did, and puts everything back on the cold-boot wake.  The registry of
 * domains is built at compile time from the `alp,som-power-domain` nodes the
 * board generator emits (metadata/e1m_modules/aen/on-module-links.yaml), so a
 * SKU that does not fit a part has no node and the domain reads as absent.
 *
 * Who drives the pins
 * -------------------
 * Where a driver owns a domain (the CC3501E context, the TMP112 / RV-3028
 * chip contexts, the OSPI flash controller) the layer calls THROUGH that
 * driver, via a hook bound with alp_som_power_bind(), so the driver's own
 * state (e.g. cc3501e_t::initialised) stays correct.  With no driver bound it
 * drives the devicetree pins (and, for the two I2C parts, the devicetree I2C
 * address) directly.
 */

#ifndef ALP_BACKENDS_POWER_SOM_POWER_H
#define ALP_BACKENDS_POWER_SOM_POWER_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/peripheral.h>
#include <alp/power.h>

/** Software-tracked state of one domain.  Never inferred from the chip: an
 *  unpowered PHY reads stale MDIO data, not 0xFFFF. */
typedef enum {
	ALP_SOM_PD_ACTIVE         = 0, /**< Running (or never touched). */
	ALP_SOM_PD_QUIESCED       = 1, /**< Held by the layer. */
	ALP_SOM_PD_RESTORE_FAILED = 2, /**< Restore reported an error. */
} alp_som_pd_state_t;

/** Driver hooks for one domain.  A bound hook REPLACES the default action for
 *  that domain; call alp_som_power_pin_quiesce() / _pin_restore() from inside a
 *  hook to compose with the default pin action instead of re-implementing it. */
typedef struct {
	/** Quiesce.  @p rail_off is true for the opt-in RAIL_OFF policy. */
	alp_status_t (*quiesce)(void *ctx, bool rail_off);
	/** Restore.  @p early is true on the cold-boot wake path (no sleeping
	 *  settles allowed, kernel services limited); the CC3501E hook is never
	 *  invoked early because its boot settle is seconds long. */
	alp_status_t (*restore)(void *ctx, bool rail_off, bool early);
} alp_som_power_hooks_t;

#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/** Bench-only OFF-profile knobs of the STOP backend (defaults from Kconfig; the bench app may
 *  change them in RAM before a sleep, so one image can try each vendor difference). */
typedef struct {
	bool vtor_self;  /**< OFF vtor_address = SCB->VTOR (resume at this image, no SES->ATOC). */
	bool mram_seram; /**< OFF memory_blocks |= MRAM | SERAM. */
	bool lfxo;       /**< OFF aon_clk_src = LFXO. */
	bool stby_76_8;  /**< OFF stby_clk_freq = 76.8 MHz. */
} alp_som_bench_knobs_t;
extern alp_som_bench_knobs_t alp_som_bench_knobs;
#endif

/** Hardware wake paths a STOP / STANDBY cycle armed, bits of
 *  alp_som_pd_record_t::armed_hw.  The STOP backend (alif_se_power.c) writes
 *  them, the cold-boot wake decode reads them. */
#define ALP_SOM_ARM_LPTIMER   0x00000001u /**< LPTIMER underflow (short timed wake). */
#define ALP_SOM_ARM_RTC_TIMER 0x00000002u /**< RV-3028 countdown the SDK started. */
#define ALP_SOM_ARM_RTC_INT   0x00000004u /**< RV-3028 /INT -> P15_0 armed by the caller. */
/** Record flag (not a wake path): the reset syndrome's NSRST bit was probed before the
 *  sleep and does clear, so a set bit at the next boot really is a pin reset. */
#define ALP_SOM_REC_NSRST_TRUSTED 0x00000100u

/** On-disk shape of the BKRAM wake record.  Fixed-width, no padding. */
typedef struct {
	uint32_t magic;        /**< ALP_SOM_PD_RECORD_MAGIC. */
	uint32_t mode;         /**< alp_power_mode_t the quiesce ran for. */
	uint32_t quiesced;     /**< ALP_POWER_DOMAIN_BIT set quiesced. */
	uint32_t rail_off;     /**< Subset of @c quiesced taken with RAIL_OFF. */
	uint32_t prior_active; /**< Per-domain saved bit: backlight was on / RTC EERD was set. */
	uint32_t wake_source;  /**< ALP_POWER_WAKE_* that fired; the wake decode fills it. */
	uint32_t slept_ms;     /**< Sleep duration; the wake decode fills it. */
	uint32_t armed;        /**< ALP_POWER_WAKE_* the STOP backend armed (0 before U7 arms). */
	uint32_t armed_hw;     /**< ALP_SOM_ARM_* wake paths armed. */
	uint32_t
	    timed_bit; /**< ALP_POWER_WAKE_* a timed wake reports (the source the caller asked for). */
	uint32_t armed_ms;    /**< Timed-wake length actually programmed, ms (0 = none). */
	uint32_t entry_rtc_s; /**< RV-3028 seconds since 2000-01-01 at entry; 0 = unreadable. */
	uint32_t entry_ccvr;  /**< LPRTC CCVR at entry (coarse, units unproven); 0 = unread. */
	uint32_t image_id;    /**< alp_som_pd_image_id() of the image that wrote the record. */
	uint32_t crc;         /**< CRC-32 (IEEE) over every field above. */
} alp_som_pd_record_t;

#define ALP_SOM_PD_RECORD_MAGIC \
	0x41504d45u /* "APME": layout changed, an older record reads as foreign */

/* ---- Hook binding -------------------------------------------------------- */

/** Bind driver hooks for @p domain.  @p hooks must outlive the binding.
 *  ALP_ERR_INVAL for a bad domain / NULL hooks, ALP_ERR_BUSY while quiesced. */
alp_status_t
alp_som_power_bind(alp_power_domain_t domain, const alp_som_power_hooks_t *hooks, void *ctx);

/** Drop a binding (the default pin action applies again). */
void alp_som_power_unbind(alp_power_domain_t domain);

/** The context bound to @p domain by alp_som_power_bind(), or NULL. */
void *alp_som_power_bound_ctx(alp_power_domain_t domain);

/** The default (no driver) pin action for @p domain, for hooks to compose with. */
alp_status_t alp_som_power_pin_quiesce(alp_power_domain_t domain, bool rail_off);
/** @p prior is the domain's saved pre-quiesce bit (backlight on / RTC EERD set);
 *  hooks that compose with this pass true for domains that do not use it. */
alp_status_t
alp_som_power_pin_restore(alp_power_domain_t domain, bool rail_off, bool early, bool prior);

/* ---- Quiesce / restore --------------------------------------------------- */

/**
 * Quiesce the domains whose policy and default modes call for it, consumers
 * first, and write the BKRAM record.  STOP and STANDBY quiesce; SLEEP and
 * DEEP_SLEEP return ALP_OK touching nothing (v1 scope).  RUN is accepted ONLY
 * for the bench / test cycle and quiesces every domain regardless of its
 * default modes.
 *
 * On a domain failure everything already quiesced, AND the failing domain's own
 * partial step, is restored in reverse and the error is returned.  A domain whose
 * rollback also fails reads ALP_SOM_PD_RESTORE_FAILED, is reported in
 * @p rollback_failed (may be NULL), and stays in the record so a later
 * alp_som_power_restore() can retry it.
 *
 * RUN-mode cycle caveat: the Ethernet PHY's 50 MHz RMII reference oscillator (Y3)
 * stops with the PHY, so bring the interface down (net_if_down()) before a RUN
 * cycle; the MAC would otherwise run without its reference clock.  The STOP path
 * has no such issue because the MAC is off.
 */
alp_status_t alp_som_power_quiesce(alp_power_mode_t mode, uint32_t *rollback_failed);

/**
 * Restore every domain named by the BKRAM record, in reverse quiesce order,
 * then clear the record.  A failing domain is reported in @p failed (and
 * reads ALP_SOM_PD_RESTORE_FAILED) and does not stop the others.
 *
 * @return ALP_OK; ALP_ERR_IO when any domain failed; ALP_ERR_NOT_READY when
 *         there is no valid record (nothing is touched).
 */
alp_status_t alp_som_power_restore(uint32_t *failed);

/** Quiesce(RUN) -> hold @p hold_ms -> restore.  The bench / test hook. */
alp_status_t alp_som_power_cycle_run(uint32_t hold_ms, uint32_t *failed);

/** Software-tracked state of @p domain. */
alp_som_pd_state_t alp_som_power_state(alp_power_domain_t domain);

/** Bitmap (ALP_POWER_DOMAIN_BIT) of domains currently quiesced. */
uint32_t alp_som_power_quiesced(void);

/** Policy last set for @p domain (AUTO until set). */
alp_power_domain_policy_t alp_som_power_policy(alp_power_domain_t domain);

/**
 * Cold-boot restore, the body of the early SYS_INIT.  Restores only when the
 * BKRAM record is valid (magic + CRC, plus STOP_MODE_STAT for a STOP/STANDBY
 * record where the register is present); on a plain POR it touches nothing.
 * Captures the result for alp_power_boot_wake_info().  Exposed so tests can
 * call it directly.  Always returns 0 (restore failures are reported, never
 * fatal).
 */
int alp_som_power_boot_restore(void);

/**
 * Second cold-boot pass: restore the I2C-backed domains (temperature sensor,
 * RTC clock-out) recorded by alp_som_power_boot_restore().  Runs from its own
 * SYS_INIT after the I2C controller has initialised and before the sensor and
 * Ethernet drivers.  A no-op when pass 1 found no valid record.
 */
int alp_som_power_boot_restore_i2c(void);

/**
 * Mux and pad-configure every pad the layer drives (the node's pinctrl-0
 * "default" state).  Applied automatically before the first pad drive; callable
 * early by code that drives the same pads itself.  ALP_ERR_NOT_READY when the
 * state is missing from the devicetree, ALP_ERR_IO when it fails to apply -- the
 * layer then refuses to drive any pad.
 */
alp_status_t alp_som_power_pads_apply(void);

/** STOP_MODE_STAT word (0x1A60F000), bit 4 = last reset was a STOP wake.  Weak so a
 *  host test can replace it; only built where the devicetree has the stop_mode node. */
uint32_t alp_som_power_stop_mode_read(void);

/** Test-only: forget policies, bindings, states and the boot capture. */
void alp_som_power_reset_for_test(void);

/* ---- BKRAM record store ---------------------------------------------------- */

/** CRC-32 over the record's covered fields. */
uint32_t alp_som_pd_record_crc(const alp_som_pd_record_t *rec);

/** BKRAM shadow (the boot-time clock restore can disturb the block): copy the SDK data
 *  out to RAM and serve every accessor from there until alp_som_pd_shadow_end(). */
void alp_som_pd_shadow_begin(void);
/** Prove the block writable (alp_som_pd_bkram_selftest) and copy the shadow back.  False
 *  when the block fails: the shadow stays authoritative and the block is dead for sleep. */
bool alp_som_pd_shadow_end(void);
/** Non-destructive write/readback test of the block (words past the SDK layout). */
bool alp_som_pd_bkram_selftest(void);
/** True unless the data is still served from the RAM shadow (BKRAM judged unusable). */
bool alp_som_pd_bkram_live(void);

/** True when @p rec has the right magic and CRC. */
bool alp_som_pd_record_valid(const alp_som_pd_record_t *rec);

/** Copy the stored record out (valid or not).  Returns true when valid. */
bool alp_som_pd_store_load(alp_som_pd_record_t *out);

/** Seal (magic + CRC) and store @p rec. */
void alp_som_pd_store_save(alp_som_pd_record_t *rec);

/** Invalidate the stored record. */
void alp_som_pd_store_clear(void);

/** Test-only: store @p rec verbatim, bypassing the seal. */
void alp_som_pd_store_poke(const alp_som_pd_record_t *rec);

/** Bench-only retention counter sharing the BKRAM region
 *  (CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH).  BKRAM is SDK-reserved; this
 *  exists so a bench image can prove it survived STOP, and is absent from a
 *  product build. */
typedef struct {
	uint32_t magic;
	uint32_t count;
	uint32_t image; /**< alp_som_pd_image_id() of the image that wrote the counter */
	uint32_t crc;
} alp_som_pd_bench_t;

/** Identity of the running image (CRC-32 of its ROM region).  Weak: 0 where there is no
 *  such region, so identity checks pass trivially.  Stamped into the record and the bench
 *  cell; a different value found in BKRAM means the contents belong to ANOTHER image (a
 *  fresh flash), and must not be read as this one's sleep. */
uint32_t alp_som_pd_image_id(void);

/** True when the bench cell carries a counter written by another image (or by an older
 *  layout).  Bench scratch option only; false otherwise. */
bool alp_som_pd_bkram_foreign(void);

/** Forget a foreign bench cell and diag: counter 0 under this image, PRE invalidated. */
void alp_som_pd_bkram_adopt(void);

uint32_t alp_som_pd_bench_count(void);
void     alp_som_pd_bench_set(uint32_t count);

/** Bench-only register snapshots kept in BKRAM so they survive the very STOP they
 *  document (same option as the counter).  Slot PRE is written just before the WFI,
 *  slot BOOT by the earliest init hook of the next boot; main() prints both.  A
 *  slot is a magic, a sequence number and ALP_SOM_PD_DIAG_WORDS raw register words;
 *  the meaning of each word is the index list in alif_se_power_hw.c. */
#define ALP_SOM_PD_DIAG_WORDS 56u
#define ALP_SOM_PD_DIAG_PRE   0u
#define ALP_SOM_PD_DIAG_BOOT  1u

typedef struct {
	uint32_t magic;
	uint32_t seq;   /**< boot number: BOOT = previous BOOT + 1; PRE = the boot that wrote it */
	uint32_t cycle; /**< the bench counter (cycle number) when the slot was written */
	uint32_t w[ALP_SOM_PD_DIAG_WORDS];
} alp_som_pd_diag_t;

/** Store @p words (@p n <= ALP_SOM_PD_DIAG_WORDS, the rest zero) into @p slot. */
void alp_som_pd_diag_save(unsigned slot, const uint32_t *words, unsigned n);
#ifdef CONFIG_ALP_SDK_SOM_POWER_BKRAM_BENCH_SCRATCH
/** Overwrite word @p idx of an existing @p slot (no sequence change). */
void alp_som_pd_diag_patch(unsigned slot, unsigned idx, uint32_t value);
/** Invalidate @p slot (main does this to PRE once it has printed it). */
void alp_som_pd_diag_invalidate(unsigned slot);
#else
static inline void alp_som_pd_diag_patch(unsigned slot, unsigned idx, uint32_t value)
{
	(void)slot;
	(void)idx;
	(void)value;
}
static inline void alp_som_pd_diag_invalidate(unsigned slot)
{
	(void)slot;
}
#endif
/** Copy @p slot out; true when it carries the magic. */
bool alp_som_pd_diag_load(unsigned slot, alp_som_pd_diag_t *out);

/* ---- Wake services for the STOP backend ----------------------------------- */

struct gpio_dt_spec;

/** The `wake-gpios` input of @p domain (the RV-3028 /INT pad P15_0 for the RTC
 *  domain), or NULL when the domain is absent or has none. */
const struct gpio_dt_spec *alp_som_power_wake_gpio(alp_power_domain_t domain);

/** True when the RV-3028 has its alarm or countdown interrupt enabled
 *  (CONTROL_2 AIE | TIE).  ALP_ERR_NOT_READY with no RTC domain / I2C bus. */
alp_status_t alp_som_power_rtc_int_armed(bool *armed);

/** RV-3028 interrupt service by devicetree I2C: reports the enabled, latched
 *  countdown / alarm flags (RV3028C7_WAKE_TF / _AF layout: 0x08 / 0x04) and
 *  clears every latched TF / AF / UF.  Needs no chip context, so it runs on the
 *  cold-boot wake path. */
alp_status_t alp_som_power_rtc_wake_service(uint8_t *flags);

/** M55-HE reset syndrome (AON.RTSS_HE_RESET.RESETSYNDROME), read and acknowledged:
 *  0 = POR or Secure-Enclave-initiated, 1 = the NSRST pin was asserted, 4 = reset
 *  request to the power domain.  Weak: 0 where there is no such register. */
uint32_t alp_som_power_reset_syndrome_take(void);

/** Acknowledge STOP_MODE_STAT (VBAT_STOP_MODE_REG bit 4), once the boot decode is done, so a
 *  later reset is not read as a STOP wake.  Writes exactly the STAT bit, never bit 0
 *  (STOP_MODE_CTRL enters stop mode).  Returns false when it did not clear.  Weak: true
 *  where there is no such register. */
bool alp_som_power_stop_mode_stat_clear(void);

/** True when the syndrome's NSRST bit can be trusted as a pin-reset marker (it reads 0,
 *  or clears when acknowledged).  Probed before the sleep.  Weak: false where there is no
 *  such register, so nothing is ever classified as a pin reset there. */
bool alp_som_power_reset_syndrome_trusted(void);

/** True when an enabled RV-3028 countdown / alarm flag (TF / AF) is already latched.
 *  Read only: nothing is cleared, so the wake decode still sees it. */
alp_status_t alp_som_power_rtc_flags_pending(bool *pending);

/** Clear a stale UF (time-update flag) when UIE is off; nothing else is touched. */
alp_status_t alp_som_power_rtc_clear_stale_uf(void);

/** Stop a countdown / alarm an earlier cycle left running or latched: clears TE, TIE, AIE and
 *  writes 0 to TF, AF (and UF when UIE is off).  PORF / EVF / BSF / CLKF are untouched.  Called
 *  only before this backend arms its own countdown. */
alp_status_t alp_som_power_rtc_clear_stale_wake(void);

/** Read-only dump of RV-3028 STATUS 0Eh, CONTROL_1 0Fh, CONTROL_2 10h, Event Control 13h and
 *  the EEPROM mirrors 35h (CLKOUT) and 37h (BACKUP) into @p regs[6], in that order.  Nothing
 *  is written, EEPROM included. */
alp_status_t alp_som_power_rtc_regs(uint8_t regs[6]);

/** RV-3028 calendar as seconds since 2000-01-01 00:00:00. */
alp_status_t alp_som_power_rtc_seconds(uint32_t *seconds);

/** True once an RV-3028 chip context is bound (alp_som_power_bind_rv3028), the
 *  precondition for the countdown calls below. */
bool alp_som_power_rtc_countdown_ready(void);

/** Start a one-shot countdown of @p seconds with INT enabled, through
 *  rv3028c7_timer_start().  @p actual_s (optional) receives the length the part
 *  was programmed with (long requests round up to whole minutes).
 *  ALP_ERR_NOT_READY when no chip context is bound. */
alp_status_t alp_som_power_rtc_countdown_start(uint32_t seconds, uint32_t *actual_s);

/** Stop the countdown and silence it, through rv3028c7_timer_stop(). */
alp_status_t alp_som_power_rtc_countdown_cancel(void);

/** Wake decode, part 1: runs from the early cold-boot SYS_INIT on the cycle's
 *  record (a working copy), before any timer driver initialises, with no I2C.
 *  Fills rec->wake_source / rec->slept_ms where it can.  Weak: the STOP backend
 *  overrides it; the default leaves the record untouched. */
void alp_som_power_wake_decode_early(alp_som_pd_record_t *rec);

/** Wake decode, part 2: the I2C pass (RV-3028 flags, slept time).  Weak, as above.
 *  Returns false to DISCARD the cycle's wake information (the record cannot be
 *  trusted, e.g. a STANDBY record found after the RTC lost power); the domains were
 *  already restored and are not affected. */
bool alp_som_power_wake_decode_i2c(alp_som_pd_record_t *rec);

/** True when the RV-3028 reports its power-on-reset flag (STATUS bit 0, read only,
 *  not cleared here): the RTC lost power, so the module was power-cycled.
 *  ALP_ERR_NOT_READY with no RTC domain / I2C bus. */
alp_status_t alp_som_power_rtc_porf(bool *porf);

/* ---- Op wrappers the power-class vtables point at ------------------------ */

struct gpio_dt_spec;

/**
 * Logical level (1 = asserted) a pad actually carries, read back through its GPIO
 * port after the layer drives it; negative when it cannot be read.  Weak so a host
 * test can replace it.  Needs the pad's input buffer, which the pinctrl-0 group
 * enables (pad config REN, see zephyr/soc/alif/ensemble/pinctrl_soc.h and
 * drivers/pinctrl/pinctrl_alif.c -- the LP-pad register layout is taken from that
 * driver and is TBD against the HWRM).  Without the buffer the read is meaningless
 * and the layer's holds are not self-verifiable.
 */
int alp_som_power_pad_read(const struct gpio_dt_spec *spec);

struct alp_power_backend_state;
alp_status_t alp_som_power_ops_policy_set(struct alp_power_backend_state *state,
                                          alp_power_domain_t              domain,
                                          alp_power_domain_policy_t       policy);
alp_status_t alp_som_power_ops_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out);
alp_status_t alp_som_power_ops_boot_wake_info(alp_power_boot_info_t *out);

#endif /* ALP_BACKENDS_POWER_SOM_POWER_H */
