/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wdt.h
 * @brief Alp SDK watchdog abstraction.
 *
 * Backends:
 *   - Zephyr   : `wdt_*` driver class.
 *   - Yocto    : `/dev/watchdogN` ioctl.
 *   - Baremetal: vendor HAL WDT peripheral.
 *
 * Most SoCs disable the watchdog after a feed-miss reset so the boot
 * code can choose whether to re-arm it; the ALP wrapper does **not**
 * mask that — apps are expected to know whether they re-installed the
 * timeout after a recovery boot.
 *
 * Typical usage:
 * @code
 *     alp_wdt_t *wdt = alp_wdt_open(&(alp_wdt_config_t){
 *         .wdt_id     = ALP_E1M_WDT0,     // E1M; use ALP_E1M_X_WDT0 on E1M-X
 *         .timeout_ms = 5000,
 *         .on_timeout = ALP_WDT_RESET_SOC,
 *     });
 *     while (running) {
 *         do_work();
 *         alp_wdt_feed(wdt);    // every iteration; must be < 5 s
 *     }
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2.  v0.9.0: wdt_id moved into alp_wdt_config_t so alp_wdt_open(const alp_wdt_config_t *) matches every other config-taking open (pre-1.0 signature change).
 *      v0.17.0 (#1637): alp_wdt_open() now enforces one live handle per
 *      wdt_id (before this, two subsystems could each open the same
 *      instance).  alp_wdt_close() no longer implicitly calls the
 *      equivalent of alp_wdt_disable() on the Zephyr backend -- closing
 *      one of those handles used to disarm the WHOLE watchdog device
 *      (Zephyr's wdt_* driver class has no per-channel disable), so a
 *      second subsystem holding the same wdt_id silently lost its
 *      protection with no error on either side.  Close now only
 *      releases the handle; call alp_wdt_disable() explicitly first for
 *      a best-effort SoC-wide disable.  alp_wdt_config_t additively
 *      gains on_expire + user so ALP_WDT_INTERRUPT_ONLY has a way to
 *      actually notify the app (previously accepted and silently inert
 *      on Zephyr; the Yocto backend now rejects it with
 *      ALP_ERR_NOSUPPORT instead of silently ignoring it, matching the
 *      Linux watchdog ABI's real capability). Pre-1.0, additive.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_WDT_H
#define ALP_WDT_H

#include <stdint.h>

#include <alp/cap_instance.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What happens when the watchdog fires. */
typedef enum {
	ALP_WDT_RESET_SOC      = 0, /**< Full SoC reset (default; safest). */
	ALP_WDT_RESET_CPU      = 1, /**< Core reset only — peripherals keep state. */
	ALP_WDT_INTERRUPT_ONLY = 2  /**< Generate an IRQ; no reset. */
} alp_wdt_action_t;

/** Opaque watchdog handle.  Allocate via @ref alp_wdt_open. */
typedef struct alp_wdt alp_wdt_t;

/**
 * @brief Watchdog expiry notification.
 *
 * Fires when the deadline is missed under @ref ALP_WDT_INTERRUPT_ONLY,
 * IF the underlying driver actually delivers it -- keep the body short
 * regardless; do not block, allocate, or take a mutex.  Never fires
 * under @ref ALP_WDT_RESET_SOC / @ref ALP_WDT_RESET_CPU (the reset
 * pre-empts the CPU before any callback could run).
 *
 * @warning Delivery is driver-dependent, not an SDK guarantee.  The
 *          Zephyr `wdt_*` driver class only promises to call back
 *          "when the watchdog expires" -- how, and whether at all, is
 *          per-driver.  On the CMSDK APB watchdog (the E1M-AEN801's
 *          `alp-wdt0`), the callback is wired from the SoC's NMI
 *          handler and reached only under `CONFIG_RUNTIME_NMI`, which
 *          defaults `n` and is NOT emitted by the SDK's own
 *          `board.yaml` -> Kconfig mapping -- an app that wants
 *          @c on_expire to fire on that target must select
 *          `CONFIG_RUNTIME_NMI=y` itself (see
 *          `examples/aen/aen-wdt-feed/prj.conf`).  When it does fire
 *          there, it runs from the NMI handler, not a plain ISR --
 *          treat it as at least as restrictive as ISR context (no
 *          `k_sem_give` / `k_msgq_put` / `LOG_*` assumptions beyond
 *          what the target's NMI entry actually permits).  A driver
 *          that never calls back at all is silent, not an error --
 *          @ref alp_wdt_open still returns a valid handle, because the
 *          SDK cannot detect at open time whether a given Zephyr WDT
 *          driver honours the callback.
 *
 * @param[in] wdt   The handle whose deadline fired.
 * @param[in] user  The @c user pointer from @ref alp_wdt_config_t.
 */
typedef void (*alp_wdt_expiry_cb_t)(alp_wdt_t *wdt, void *user);

/** Configuration passed to @ref alp_wdt_open. */
typedef struct {
	uint32_t wdt_id;     /**< Form-factor WDT instance ID: ALP_E1M_WDT0..1 or ALP_E1M_X_WDT0..1. */
	uint32_t timeout_ms; /**< Feed deadline in milliseconds; must be non-zero. */
	alp_wdt_action_t on_timeout; /**< Action when the deadline is missed. */
	/** Required when @c on_timeout == @ref ALP_WDT_INTERRUPT_ONLY;
	 *  ignored for the reset actions.  @ref alp_wdt_open fails with
	 *  @ref ALP_ERR_INVAL if INTERRUPT_ONLY is requested with this
	 *  NULL -- an interrupt-only watchdog nobody can observe neither
	 *  resets the SoC nor notifies anyone, which is worse than not
	 *  offering the mode. */
	alp_wdt_expiry_cb_t on_expire;
	void               *user; /**< Forwarded to @c on_expire; otherwise unused. */
} alp_wdt_config_t;

/**
 * @brief Default-initialize an @ref alp_wdt_config_t for watchdog @p id.
 *
 * Identity from @p id; canonical defaults: @c timeout_ms = 1000 (a
 * conservative 1 s feed deadline — @c timeout_ms must be non-zero, so
 * zero-init is not valid here and the default supplies a usable value),
 * @c on_timeout = @ref ALP_WDT_RESET_SOC (full SoC reset, the safest
 * action). Shorten @c timeout_ms for a tighter deadline after expansion.
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_WDT_CONFIG_DEFAULT(id) \
	((alp_wdt_config_t){ .wdt_id = (id), .timeout_ms = 1000u, .on_timeout = ALP_WDT_RESET_SOC })

/**
 * @brief Install a watchdog timeout and arm the timer.
 *
 * The watchdog starts feeding-required immediately on return.  Apps
 * must call @ref alp_wdt_feed before @c timeout_ms elapses or the
 * configured @c on_timeout action triggers.
 *
 * @note A watchdog instance is **exclusive**: at most one handle may be
 *       open on a given @c wdt_id at a time, and a second open returns
 *       NULL with @ref ALP_ERR_BUSY until the owner calls
 *       @ref alp_wdt_close.  This one-owner-per-instance rule is what
 *       actually protects a shared device: Zephyr's `wdt_*` driver
 *       class has no per-channel disable, so without it one
 *       subsystem's close could disarm a channel it does not own --
 *       see @ref alp_wdt_close's own doc for what close does and does
 *       not disarm.  Two subsystems that both need a deadline must
 *       either use two instances or share one handle.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL with non-zero
 *                 @c timeout_ms; @c wdt_id must be a valid watchdog
 *                 index on the active SoM (ALP_E1M_WDT0..1 or
 *                 ALP_E1M_X_WDT0..1); @c on_expire must be non-NULL
 *                 when @c on_timeout == @ref ALP_WDT_INTERRUPT_ONLY.
 * @return Open handle on success;
 *         NULL with @ref alp_last_error set to:
 *           @ref ALP_ERR_INVAL (NULL @p cfg; zero @c timeout_ms; or
 *             INTERRUPT_ONLY requested with @c on_expire NULL);
 *           @ref ALP_ERR_OUT_OF_RANGE (@c wdt_id is a valid C index
 *             for the pool but exceeds the SoC's actual watchdog
 *             count, e.g. @c ALP_SOC_WDT_COUNT == 1);
 *           @ref ALP_ERR_BUSY (another handle already owns
 *             @c cfg->wdt_id -- checked before any backend runs, so
 *             this can never race a concurrent open on the same
 *             wdt_id; on the Zephyr backend a residual internal
 *             reclaim can also surface this if a non-SDK Zephyr
 *             consumer, e.g. CONFIG_TASK_WDT, still holds the same
 *             underlying device.  A watchdog that refuses to disarm
 *             at all once armed -- Zephyr's own watchdog.h: "not all
 *             watchdogs can be restarted after they are disabled" --
 *             surfaces that reclaim's own failure code here instead,
 *             not BUSY);
 *           @ref ALP_ERR_NOT_READY (the underlying device isn't
 *             ready);
 *           @ref ALP_ERR_NOSUPPORT (the backend cannot honour
 *             @c on_timeout -- e.g. the Yocto backend rejects
 *             INTERRUPT_ONLY, which the Linux watchdog ABI has no way
 *             to deliver);
 *           or another backend-reported code if the SoC rejected the
 *           requested timeout (too long for the hardware).
 */
alp_wdt_t *alp_wdt_open(const alp_wdt_config_t *cfg);

/**
 * @brief Reset the watchdog timer.
 *
 * Apps must feed faster than @c timeout_ms or the configured action
 * triggers.  Cheap call — typically a single MMIO write.
 *
 * @param[in] wdt  Handle from @ref alp_wdt_open.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_wdt_feed(alp_wdt_t *wdt);

/**
 * @brief Disable the watchdog if the SoC supports it.
 *
 * Many M-class watchdogs are write-once-armed and cannot be disabled
 * without a reset.  @ref ALP_ERR_NOSUPPORT is the expected return on
 * such hardware; the caller can treat it as informational.
 *
 * @param[in] wdt  Handle from @ref alp_wdt_open.
 * @return ALP_OK on success;
 *         ALP_ERR_NOSUPPORT if the SoC's WDT is one-shot;
 *         ALP_ERR_NOT_READY if @p wdt is closed.
 */
alp_status_t alp_wdt_disable(alp_wdt_t *wdt);

/**
 * @brief Release the handle.  NULL is a no-op.
 *
 * @par What this does NOT do
 * This does **not** disable the watchdog (see docs/abi-markers.md for
 * the v0.17.0 behaviour-change note on why).  Call @ref alp_wdt_disable
 * explicitly BEFORE close() if a best-effort, SoC-wide disable is
 * genuinely what the caller wants; its documented @ref
 * ALP_ERR_NOSUPPORT return tells you when the hardware can't honour
 * that.
 *
 * @par Backend-specific behaviour
 *   - Zephyr: releases the handle only.  The installed timeout keeps
 *     running -- Zephyr also has no per-channel *uninstall* -- so a
 *     caller that stops feeding after close() still gets @ref
 *     ALP_WDT_RESET_SOC / @ref ALP_WDT_RESET_CPU (the hardware reset
 *     needs no live handle to fire).  @ref ALP_WDT_INTERRUPT_ONLY asks
 *     the driver for no reset at all (`WDT_FLAG_RESET_NONE`); close()
 *     additionally unregisters the ISR trampoline's owner before
 *     returning, so a deadline that fires after close() no longer
 *     calls @c on_expire (no owner to resolve to).  Whether the
 *     watchdog itself stays quiet too is driver-dependent: some
 *     in-tree Zephyr WDT drivers honour `WDT_FLAG_RESET_NONE`; others
 *     do not and arm a reset regardless of the requested mode -- e.g.
 *     the CMSDK APB watchdog the E1M-AEN801's `alp-wdt0` uses stores
 *     the flags but never reads them back, and always programs a
 *     reset-enabled timeout.  This SDK does not paper over that: it
 *     forwards the request the Zephyr WDT API defines and cannot make
 *     a driver honour a flag it ignores.  A caller for whom the
 *     post-close window (silent @c on_expire loss, and on
 *     flag-ignoring hardware, a possible reset) is unacceptable must
 *     call @ref alp_wdt_disable before close(), or keep feeding until
 *     it no longer needs the deadline.  Separately: a *later*
 *     @ref alp_wdt_open on the same @c wdt_id that reclaims a stale
 *     `wdt_setup()` (see its own @ref ALP_ERR_BUSY entry) can disarm
 *     this handle's still-running post-close timeout as a side effect
 *     of that reclaim -- if the reclaim's own re-install then fails,
 *     that protection is not restored; the failed @ref alp_wdt_open
 *     call's return does not distinguish this from a reclaim attempt
 *     that never touched the device.
 *   - Yocto: closes this handle's own `/dev/watchdogN`, attempting a
 *     best-effort disarm (`WDIOS_DISABLECARD`, plus the magic-close
 *     write if the driver advertised it) first -- scoped to this
 *     handle's own device node, never another handle's.
 *
 * @param[in] wdt  Handle from @ref alp_wdt_open, or NULL.
 */
void alp_wdt_close(alp_wdt_t *wdt);

/**
 * @brief Query the capabilities of an opened watchdog handle.
 *
 * @param wdt  Handle from @ref alp_wdt_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p wdt is NULL.
 */
const alp_capabilities_t *alp_wdt_capabilities(const alp_wdt_t *wdt);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_WDT_H */
