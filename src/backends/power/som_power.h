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

/** On-disk shape of the BKRAM wake record.  Fixed-width, no padding. */
typedef struct {
	uint32_t magic;        /**< ALP_SOM_PD_RECORD_MAGIC. */
	uint32_t mode;         /**< alp_power_mode_t the quiesce ran for. */
	uint32_t quiesced;     /**< ALP_POWER_DOMAIN_BIT set quiesced. */
	uint32_t rail_off;     /**< Subset of @c quiesced taken with RAIL_OFF. */
	uint32_t prior_active; /**< Subset that was active before (backlight). */
	uint32_t wake_source;  /**< ALP_POWER_WAKE_* that fired; U7 fills it. */
	uint32_t slept_ms;     /**< Sleep duration; U7 fills it. */
	uint32_t crc;          /**< CRC-32 (IEEE) over every field above. */
} alp_som_pd_record_t;

#define ALP_SOM_PD_RECORD_MAGIC 0x41504d44u /* "APMD" */

/* ---- Hook binding -------------------------------------------------------- */

/** Bind driver hooks for @p domain.  @p hooks must outlive the binding.
 *  ALP_ERR_INVAL for a bad domain / NULL hooks, ALP_ERR_BUSY while quiesced. */
alp_status_t
alp_som_power_bind(alp_power_domain_t domain, const alp_som_power_hooks_t *hooks, void *ctx);

/** Drop a binding (the default pin action applies again). */
void alp_som_power_unbind(alp_power_domain_t domain);

/** The default (no driver) pin action for @p domain, for hooks to compose with. */
alp_status_t alp_som_power_pin_quiesce(alp_power_domain_t domain, bool rail_off);
alp_status_t alp_som_power_pin_restore(alp_power_domain_t domain, bool rail_off, bool early);

/* ---- Quiesce / restore --------------------------------------------------- */

/**
 * Quiesce the domains whose policy and default modes call for it, consumers
 * first, and write the BKRAM record.  STOP and STANDBY quiesce; SLEEP and
 * DEEP_SLEEP return ALP_OK touching nothing (v1 scope).  RUN is accepted ONLY
 * for the bench / test cycle and quiesces every domain regardless of its
 * default modes.  On a domain failure everything already quiesced is restored
 * in reverse and the error is returned (nothing stays held).
 */
alp_status_t alp_som_power_quiesce(alp_power_mode_t mode);

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

/** Test-only: forget policies, bindings, states and the boot capture. */
void alp_som_power_reset_for_test(void);

/* ---- BKRAM record store (backing is a placeholder until U7) -------------- */

/** CRC-32 over the record's covered fields. */
uint32_t alp_som_pd_record_crc(const alp_som_pd_record_t *rec);

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

/* ---- Op wrappers the power-class vtables point at ------------------------ */

struct alp_power_backend_state;
alp_status_t alp_som_power_ops_policy_set(struct alp_power_backend_state *state,
                                          alp_power_domain_t              domain,
                                          alp_power_domain_policy_t       policy);
alp_status_t alp_som_power_ops_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out);
alp_status_t alp_som_power_ops_boot_wake_info(alp_power_boot_info_t *out);

#endif /* ALP_BACKENDS_POWER_SOM_POWER_H */
