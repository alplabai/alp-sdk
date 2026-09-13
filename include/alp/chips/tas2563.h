/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tas2563.h
 * @brief Texas Instruments TAS2563 smart Class-D mono speaker amp.
 *
 * @par Driver status: [partial-impl] -- connectivity probe + reset
 *   sequencing, mode control, output level, tuning-blob replay, I2S/
 *   IV-sense configuration, fault-pin handling.  Not implemented:
 *   PPC3 export parsing (by design, see @ref tas2563_load_tuning) and
 *   write verification via `I2C_CKSUM` (algorithm undocumented, see
 *   the same function's doc).  Matches `driver_status: partial` in
 *   `metadata/chips/tas2563.yaml`.
 *
 * @par Why an in-tree driver, not the upstream one: an upstream Zephyr
 *   TAS2563 driver exists (zephyrproject-rtos/zephyr#103148, merged
 *   2026-05-01) but is not present in this repo's pinned Zephyr
 *   revision (`west.yml`, v4.4.x) -- that PR landed after the pin.  By
 *   maintainer decision (#2077), this repo keeps its own portable
 *   `<alp/chips>` driver over `alp_i2c`/`alp_gpio` for now rather than
 *   adopting the upstream one.
 *
 * @par Verification status: [UNTESTED] -- driver compiles, passes NULL-arg
 *   smokes, and passes register-protocol ZTests against a fake that
 *   models this part's paged register map from the datasheet
 *   (tests/zephyr/chips/src/fake_tas2563.c).  That is still a paper
 *   check: the fake was built from the same document the driver was,
 *   so the two agreeing proves consistency, not correctness.  No HiL
 *   silicon bring-up yet, and no speaker has ever been connected.
 *   Treat all numbers + lifecycle sequencing as paper-correct only
 *   until the v1.0 verification sweep lands.
 *
 * @par Datasheet: every register address, bit field, reset value and
 *   mode encoding below is cited to TI **SLASET3D** ("TAS2563 6.1W
 *   Boosted Class-D Audio Amplifier With Integrated DSP and IV
 *   Sense", April 2019, revised January 2024).  Section, table and
 *   page references in this header and in `chips/tas2563/tas2563.c`
 *   are to that revision.  Nothing here has been read back off
 *   silicon or heard through a speaker.
 *
 * Digital input I2S + I2C control + algorithm-driven smart-amp
 * features (DRC, DSP, EQ, IV-sense feedback for excursion +
 * thermal protection).  This driver is intentionally thin --
 * smart-amp tuning runs in TI's PPC3 (PurePath Console) host
 * tool, which generates a binary "tuning blob" that the host
 * MCU streams into the chip via I2C.
 *
 * @par Safe by default.  This part can drive roughly 10 W peak into a
 *   4 ohm load (SLASET3D §1 "Features", p.1), so nothing in this API
 *   starts the Class-D stage on its own:
 *     - @ref tas2563_init leaves the amp in **software shutdown**
 *       (`PWR_CTL.MODE = 10b`, which is also the part's own reset
 *       value -- SLASET3D §7.5.4 Table 7-104, p.66).  Only an
 *       explicit @ref tas2563_set_mode with @ref TAS2563_MODE_ACTIVE
 *       makes the amp switch.
 *     - @ref tas2563_set_mode refuses any encoding outside the three
 *       modes in @ref tas2563_mode_t; in particular it will not let a
 *       caller land on `MODE = 11b`, which runs load diagnostics into
 *       the speaker terminals and then leaves the device ACTIVE
 *       (SLASET3D §7.3.11.5, p.35).
 *     - @ref tas2563_load_tuning refuses any record that writes
 *       `SW_RESET`, `PWR_CTL`, `MISC` (`IRQZ_POL`) or `TG_CFG0`, so a
 *       caller-supplied tuning blob cannot bring the amp out of
 *       shutdown, reset away the caller's configuration, invert the
 *       fault pin under @ref tas2563_fault_asserted, or arm the tone
 *       generator behind the caller's back.
 *     - The IV-sense blocks are also powered down at reset
 *       (`PWR_CTL.VSNS_PD`/`ISNS_PD`, both `1h` -- SLASET3D §7.5.4
 *       Table 7-104, p.66); @ref tas2563_configure_iv_sense powers
 *       them up explicitly.
 *
 *   @warning The output level is NOT low by default, and this is the
 *   one place a caller with speakers attached has to act.
 *   `PB_CFG1.AMP_LEVEL` powers up at `10h` = 16.0 dBV / 8.92 Vpk
 *   (SLASET3D §7.5.5 Table 7-105, p.67) -- roughly 9.9 W peak into
 *   4 ohm, near the top of the part's range, and only 6 dB below the
 *   `1Ch` = 22 dBV / 17.8 Vpk maximum.  Call
 *   @ref tas2563_set_amp_level with a level you have chosen for your
 *   enclosure BEFORE the first @ref tas2563_set_mode with
 *   @ref TAS2563_MODE_ACTIVE.  A tuning blob may also set this
 *   register -- deliberately, since output level is what a smart-amp
 *   tuning is for -- so set the level AFTER
 *   @ref tas2563_load_tuning, not before, if you use both.
 *
 * @par Shared SD_N / IRQZ nets -- a real board, not a driver bug.
 *   This context and its API model ONE physical chip, but a board is
 *   free to tie AMP.ENABLE and/or AMP.FAULT together across two amps
 *   (the E1M-EVK does: `metadata/boards/e1m-evk.yaml`, both nets
 *   shared between U27 and U28).  This driver does not detect or guard
 *   that sharing -- doing so across independent @ref tas2563_t
 *   instances would need a registry this driver does not keep.
 *   Instead, the contract is on the caller:
 *     - Pass @p sd_n to **at most one** instance per physical SD_N net.
 *       Every other instance on that net passes NULL and leaves SD_N
 *       ownership -- including @ref tas2563_deinit and
 *       @ref tas2563_set_hw_enable, both of which toggle the pin -- to
 *       the caller or to the one instance that owns it.  Driving a
 *       shared SD_N low resets EVERY amp on that net (SLASET3D
 *       §7.3.11.1: "All registers loose state in this mode").
 *     - A shared IRQZ pin tells you *that* one of the amps on it
 *       faulted, never *which*.  Disambiguating requires reading each
 *       instance's own @ref tas2563_read_faults over I2C.
 *     - Initialise the instance that owns @p sd_n FIRST.  @ref
 *       tas2563_init releases hardware shutdown for every amp on a
 *       shared net the moment it drives SD_N high, so an instance
 *       initialised with @p sd_n = NULL on that same net must not run
 *       before the owning instance's @ref tas2563_init has already
 *       done so.
 *
 * @par GPIO write-before-AND-after-configure ordering matters here.
 *   `alp_gpio_configure(sd_n, ALP_GPIO_OUTPUT, ...)` sets direction
 *   only -- Zephyr's `GPIO_OUTPUT` flag is documented upstream (not in
 *   this repo) as "no change to the output state." That is true of the
 *   FLAG, but not of every backend's data register: on the AEN801 EVK,
 *   SD_N is a `snps,designware-gpio` pin, and that backend's
 *   `dw_pin_config()` (`drivers/gpio/gpio_dw.c`) switches direction to
 *   output BEFORE touching the data register, and only touches it at
 *   all for `GPIO_OUTPUT_INIT_HIGH`/`_LOW` -- neither of which
 *   `ALP_GPIO_OUTPUT` alone carries.  Configuring before writing at all
 *   would therefore expose the data register's reset-time level as an
 *   output the instant direction flips, before any write ever runs --
 *   an unplanned pulse on a pin that may be shared with another amp
 *   (see above).  That reset-time level is not verified as `0` on this
 *   board's actual silicon (AE822): the DesignWare GPIO IP's data
 *   register reset value is a synthesis-time parameter, not something
 *   read back here, so it is possible in principle for it to reset
 *   `1` instead -- the fix below is correct regardless of which, since
 *   it never relies on the reset value being any particular level.
 *   @ref tas2563_init writes high BEFORE configuring to close that
 *   window on gpio_dw: `alp_gpio_write()` has no dependency on the pin
 *   already being configured, and `gpio_dw_port_set_bits_raw()` writes
 *   the data register unconditionally, independent of direction.  That
 *   first write is not sufficient on every backend, though: Zephyr's
 *   `gpio_emul` masks a write against the pins CURRENTLY configured as
 *   output, so a write issued before configure is silently DROPPED
 *   there IF the pin is not already configured as output from some
 *   earlier state (rather than redundant, as on gpio_dw) -- the
 *   opposite failure mode, and one this fake's shared per-device state
 *   makes order-dependent across test cases (see
 *   `test_tas2563_init_writes_sd_n_before_configuring_it` in
 *   `tests/zephyr/chips/src/test_audio.c`, which pre-arms the pin so
 *   the drop is guaranteed rather than incidental).  @ref
 *   tas2563_init therefore writes high a SECOND time, after
 *   configuring, so the final state is correct on both kinds of
 *   backend; the second write costs one redundant register access on
 *   gpio_dw, where the first already landed.  Verified against the
 *   Zephyr/DesignWare and `gpio_emul` backends only -- the
 *   `sw_fallback` and `testing` GPIO backends have no real pin to
 *   glitch either way, but the CC3501E GPIO proxy's behaviour on a
 *   write issued before its remote side has ever been configured is
 *   UNVERIFIED.
 *
 * @par @p sd_n must be declared GPIO_ACTIVE_HIGH in its devicetree
 *   pin-array entry, matching SD_N's real polarity (high = enabled).
 *   Zephyr only applies a `GPIO_ACTIVE_LOW` pin-array entry's
 *   inversion inside `gpio_pin_configure()` (it updates the driver's
 *   `invert` bitmask there, not before) -- so on an ACTIVE_LOW entry,
 *   the pre-configure write above would go out at the RAW (uninverted)
 *   level while the post-configure write would correctly invert,
 *   blipping the physical pin from one level to the other and ending
 *   on the wrong one.  This is not a live bug on the E1M-EVK: no
 *   in-tree board declares `P5_2` (AMP_ENABLE) in its `alp,pin-array`
 *   at all today (both in-tree callers of @ref tas2563_init pass
 *   `sd_n = NULL`, per "Shared SD_N / IRQZ nets" above), and this
 *   header's own test overlay declares its `sd_n` test pin
 *   `GPIO_ACTIVE_HIGH` (`tests/zephyr/chips/boards/native_sim_native_64.overlay`).
 *   It is a real precondition on any future caller that DOES pass a
 *   real `sd_n`, recorded here rather than left implicit.
 *
 * v0.3 driver scope:
 *   - I2C connectivity probe (read CHIP_ID).
 *   - Software shutdown / mute / unmute via the MODE_CTRL
 *     register.
 *   - Hardware enable via SD_N (an external GPIO; the caller
 *     supplies an alp_gpio_t handle bound to the EVK's
 *     `EVK_PIN_AMP_ENABLE`).
 *
 * v0.3.x adds:
 *   - tuning-blob replay (@ref tas2563_load_tuning): a book/page/
 *     register record stream, applied one register per I2C
 *     transaction.  Converting a TI PPC3 export into that record
 *     stream is a host-side step and is deliberately NOT in this
 *     driver -- see @ref tas2563_load_tuning.
 *   - an output-level setter (@ref tas2563_set_amp_level) -- see the
 *     warning above.
 *   - I2S configuration (@ref tas2563_configure_i2s) derived from the
 *     same @ref alp_i2s_config_t the caller opened the host bus with,
 *     plus the IV-sense return path (@ref tas2563_configure_iv_sense):
 *     the host's I2S TX feeds the amp's SDIN, the amp's SDOUT feeds
 *     IV-sense data back on the host's I2S RX.
 *   - fault-pin handling (@ref tas2563_configure_fault_pin,
 *     @ref tas2563_fault_asserted, @ref tas2563_read_faults,
 *     @ref tas2563_clear_faults) for the open-drain IRQ_N on the EVK's
 *     `EVK_PIN_AMP_FAULT`, routed through host SoC GPIO with an
 *     internal pull-up.
 *
 * I2C addresses (TAS2563 Table 7-3):
 *   AD0/SPICLK = GND       -> 0x4C
 *   AD0/SPICLK = 10k to GND -> 0x4D
 *   AD0/SPICLK = 10k to VDD -> 0x4E
 *   AD0/SPICLK = VDD       -> 0x4F
 *   Global broadcast        -> 0x48 (write-only; see
 *                                    TAS2563_I2C_ADDR_BROADCAST --
 *                                    NOT accepted by tas2563_init(), #1846)
 */

#ifndef ALP_CHIPS_TAS2563_H
#define ALP_CHIPS_TAS2563_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/i2s.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TAS2563_I2C_ADDR_GND_DIRECT 0x4Cu
#define TAS2563_I2C_ADDR_GND_PULL   0x4Du
#define TAS2563_I2C_ADDR_VDD_PULL   0x4Eu
#define TAS2563_I2C_ADDR_VDD_DIRECT 0x4Fu
/** Global broadcast/general-call address (write-only per the
 *  datasheet).  Documented for reference only -- tas2563_init()
 *  rejects it with ALP_ERR_INVAL.  0x48 does not pin down exactly one
 *  physical chip the way a strap address does, and every bus-touching
 *  op this driver exposes (init, read_revision, set_mode) both reads
 *  AND writes through ctx->addr -- select_page() writes the page
 *  register first on every one of them.  Whatever answers at 0x48
 *  (every TAS2563 on the bus, an unrelated device strapped there by
 *  coincidence -- e.g. an INA236 on a real EVK pre-respin, #1846 --
 *  or nothing) is undefined for a per-instance context, regardless of
 *  direction. */
#define TAS2563_I2C_ADDR_BROADCAST 0x48u

/** Minimum settle time after EITHER a hardware reset (SDZ going high) OR
 *  a software reset (the self-clearing `SW_RESET` bit, SLASET3D §7.5.3),
 *  before the next I2C access, in microseconds.  One constant for both
 *  because SLASET3D §9.2 "Power Supply Sequencing" states one floor for
 *  both: "After a hardware or software reset additional commands to the
 *  device should be delayed for 100 uS to allow the OTP to load. The
 *  above sequence should be completed before any I2C operation."  I2C is
 *  disabled for the whole time the part sits in Hardware Shutdown
 *  (§7.3.11.1).  This value is chosen deliberately above that 100 us
 *  floor.
 *
 *  @ref tas2563_init applies it twice when it owns @c sd_n (once after
 *  driving SDZ high, once after its own software reset) and once when it
 *  does not (software reset only).  A caller that owns SD_N externally
 *  (passes @c sd_n as NULL) must additionally wait at least this long,
 *  after releasing SDZ, before calling @ref tas2563_init -- see that
 *  function's precondition below. */
#define TAS2563_RESET_SETTLE_US 200u

/** Operating-mode enum mapped onto the chip's `PWR_CTL.MODE[1:0]`
 *  field (SLASET3D §7.5.4 Table 7-104, p.66; §7.3.11.6 Table 7-8,
 *  p.35).  The fourth documented encoding, `11b` "Load Diagnostics
 *  followed by device ACTIVE", is deliberately absent: it drives the
 *  speaker terminals and then leaves the amp switching, which is not
 *  something a caller should be able to reach by passing a stray
 *  integer.  @ref tas2563_set_mode rejects it. */
typedef enum {
	TAS2563_MODE_ACTIVE   = 0x00, /**< Audio amplification active. */
	TAS2563_MODE_MUTE     = 0x01, /**< PWM muted, register state preserved. */
	TAS2563_MODE_SHUTDOWN = 0x02, /**< Software shutdown, lowest IDD. */
} tas2563_mode_t;

/**
 * @name `PB_CFG1.AMP_LEVEL` bounds accepted by @ref tas2563_set_amp_level
 *
 * SLASET3D §7.5.5 Table 7-105 (p.67) enumerates the level codes in
 * 0.5 dBV steps from `01h` = 8.5 dBV (3.76 Vpk) to `1Ch` = 22 dBV
 * (17.8 Vpk), marks `1Dh`-`1Fh` Reserved, and does not list `00h` at
 * all.  Power-on value is `10h` = 16.0 dBV (8.92 Vpk).  Codes between
 * the two bounds are passed through unnamed: naming 28 of them would
 * be a second copy of a datasheet table, and a second copy is a second
 * thing to get wrong.
 * @{
 */
#define TAS2563_AMP_LEVEL_MIN 0x01u /**< 8.5 dBV (3.76 Vpk), the quietest listed. */
#define TAS2563_AMP_LEVEL_POR 0x10u /**< 16.0 dBV (8.92 Vpk), the power-on value. */
#define TAS2563_AMP_LEVEL_MAX 0x1Cu /**< 22 dBV (17.8 Vpk), the loudest listed. */
/** @} */

/**
 * Which TDM/I2S receive slot the amp takes its playback audio from --
 * `TDM_CFG2.RX_SCFG[5:4]` (SLASET3D §7.5.10 Table 7-110, p.70).  The
 * part is mono, so a stereo host stream has to be told which half this
 * chip plays; a stereo pair (the EVK's U27 at 0x4D and U28 at 0x4E)
 * uses one LEFT and one RIGHT, or SLOT_FROM_ADDR on both.
 */
typedef enum {
	TAS2563_RX_SLOT_FROM_ADDR = 0x0, /**< Slot = I2C address offset. */
	TAS2563_RX_LEFT           = 0x1, /**< Mono, left channel. */
	TAS2563_RX_RIGHT          = 0x2, /**< Mono, right channel. */
	TAS2563_RX_DOWNMIX        = 0x3, /**< Stereo downmix (L+R)/2. */
} tas2563_rx_channel_t;

/**
 * @name Latched fault bits returned by @ref tas2563_read_faults
 *
 * The four latched-interrupt registers are passed through
 * byte-aligned, so a bit's position here is its position in the
 * datasheet register: `INT_LTCH0` (0x24) occupies bits 0..7,
 * `INT_LTCH1` (0x25) bits 8..15, `INT_LTCH3` (0x26) bits 16..23 and
 * `INT_LTCH4` (0x27) bits 24..31.  Nothing is re-encoded, so each
 * macro can be checked against its datasheet table directly.
 *
 * SLASET3D §7.5.36 Table 7-136 p.82-83 (0x24), §7.5.37 Table 7-137
 * p.83-84 (0x25), §7.5.38 Table 7-138 p.84 (0x26), §7.5.39
 * Table 7-139 p.85 (0x27).
 * @{
 */
#define TAS2563_FAULT_OVER_TEMP         (1u << 0)  /**< 0x24[0] over-temperature error. */
#define TAS2563_FAULT_OVER_CURRENT      (1u << 1)  /**< 0x24[1] Class-D over-current error. */
#define TAS2563_FAULT_TDM_CLOCK         (1u << 2)  /**< 0x24[2] TDM clock error. */
#define TAS2563_FAULT_LIMITER_ACTIVE    (1u << 3)  /**< 0x24[3] limiter active. */
#define TAS2563_FAULT_VBAT_BELOW_INFL   (1u << 4)  /**< 0x24[4] VBAT below limiter inflection. */
#define TAS2563_FAULT_LIMITER_MAX_ATTN  (1u << 5)  /**< 0x24[5] limiter max attenuation. */
#define TAS2563_FAULT_LIMITER_INF_HOLD  (1u << 6)  /**< 0x24[6] limiter infinite hold. */
#define TAS2563_FAULT_LIMITER_MUTE      (1u << 7)  /**< 0x24[7] limiter muted the audio. */
#define TAS2563_FAULT_VBAT_BROWNOUT     (1u << 8)  /**< 0x25[0] VBAT brown-out detected. */
#define TAS2563_FAULT_BROWNOUT_ACTIVE   (1u << 9)  /**< 0x25[1] brown-out protection active. */
#define TAS2563_FAULT_BROWNOUT_SHUTDOWN (1u << 10) /**< 0x25[2] brown-out triggered shutdown. */
#define TAS2563_FAULT_SPK_OPEN_LOAD     (1u << 11) /**< 0x25[3] load-diag status 01b: open load. */
#define TAS2563_FAULT_SPK_SHORT_LOAD    (1u << 12) /**< 0x25[4] load-diag status 10b: short load. */
#define TAS2563_FAULT_LOAD_DIAG_DONE    (1u << 13) /**< 0x25[5] load diagnostics completed. */
#define TAS2563_FAULT_DEVICE_POWER_UP   (1u << 16) /**< 0x26[0] device power-up event. */
#define TAS2563_FAULT_CP_PG             (1u << 17) /**< 0x26[1] charge-pump power-good event. */
#define TAS2563_FAULT_BOOST_OV_CLAMP    (1u << 18) /**< 0x26[2] boost over-voltage clamp. */
#define TAS2563_FAULT_DC_DETECT         (1u << 19) /**< 0x26[3] DC detect. */
#define TAS2563_FAULT_PLL_LOCK          (1u << 20) /**< 0x26[4] PLL lock event. */
#define TAS2563_FAULT_VBAT_POR          (1u << 21) /**< 0x26[5] VBAT power-on-reset. */
#define TAS2563_FAULT_BOOST_CLOCK       (1u << 22) /**< 0x26[6] boost clock error. */
#define TAS2563_FAULT_DAC_MOD_CLOCK     (1u << 23) /**< 0x26[7] DAC modulator clock error. */
#define TAS2563_FAULT_ASI2_CLOCK        (1u << 27) /**< 0x27[3] ASI2 clock error. */
#define TAS2563_FAULT_PDM_MIC_CLOCK     (1u << 28) /**< 0x27[4] PDM mic clock error. */
#define TAS2563_FAULT_DEVICE_POWER_DOWN (1u << 31) /**< 0x27[7] device power-down event. */

/** The faults the datasheet states drive the part into software
 *  shutdown on its own: TDM clock error, die over-temperature and
 *  Class-D over-current (SLASET3D §7.3.12, p.35-36), plus the
 *  brown-out protection shutdown flag (§7.5.37 Table 7-137, p.83).
 *  Provided so a fault handler can ask "did the amp stop?" without
 *  re-deriving the set. */
#define TAS2563_FAULT_SHUTDOWN_CAUSES \
	(TAS2563_FAULT_OVER_TEMP | TAS2563_FAULT_OVER_CURRENT | TAS2563_FAULT_TDM_CLOCK | \
	 TAS2563_FAULT_BROWNOUT_SHUTDOWN)
/** @} */

/**
 * One register write in a tuning stream.  The device's memory map is
 * paged, so a coefficient write is only addressable as the triple
 * (book, page, register).  SLASET3D §7.3.10 "Register Organization"
 * (p.34) states the two inner sizes -- "Each page contains 128 bytes
 * and each book contains 256 pages" -- and does not state a book
 * count; that 256 books exist comes from `BOOK[7:0]`, whose field
 * table enumerates 00h..FFh (§7.5.62 Table 7-162, p.94).
 */
typedef struct {
	uint8_t book; /**< Value for the `BOOK` register, 0x7F on page 0. */
	uint8_t page; /**< Value for the `PAGE` register, 0x00. */
	uint8_t reg;  /**< Target register, 0x01..0x7E (see @ref tas2563_load_tuning). */
	uint8_t val;  /**< Byte to write. */
} tas2563_tuning_reg_t;

typedef struct {
	bool        initialised;
	alp_i2c_t  *bus;
	uint8_t     addr;
	alp_gpio_t *sd_n;  /**< AMP.ENABLE pin (active-high; drive
	                        low to assert SD_N hardware shutdown). */
	alp_gpio_t *irq_n; /**< AMP.FAULT pin (open-drain, active low);
	                        NULL until @ref tas2563_configure_fault_pin. */
} tas2563_t;

/**
 * @brief Probe the chip, release hardware shutdown if we own SD_N,
 *        software-reset it, and leave the amp in software shutdown.
 *
 * This RELEASES hardware shutdown when it owns @p sd_n -- it does not
 * perform a hardware reset.  If SDZ was already high (R138's pull-up
 * on the E1M-EVK, or a warm restart that never asserted it), driving
 * it high again is a no-op electrically; SLAA954 "TAS2563 End System
 * Integration Guide" §3.1 Case 1's recommended hardware reset (an
 * actual low-then-high pulse) is then the CALLER's responsibility, not
 * this function's -- pulsing a pin this function may not exclusively
 * own (see "Shared SD_N / IRQZ nets" above) is not this function's
 * call to make.
 *
 * This DOES unconditionally perform a software reset (`SW_RESET`,
 * SLASET3D §7.5.3) after book 0 / page 0 is selected and before
 * anything else is configured -- the other half of SLAA954 §3.1 Case
 * 1's recommendation, and the one part of it this function can always
 * do regardless of what it does or does not own.  `SW_RESET` returns
 * every register to its POR default, so this runs before any other
 * write in this function, followed by the @ref TAS2563_RESET_SETTLE_US
 * wait §9.2 requires.  Nothing in SLASET3D says the write itself is
 * NACKed or otherwise handled specially -- it is a normal single-byte
 * write like any other register write in this driver, though whether
 * it actually ACKs on real silicon is unverified.
 *
 * @warning Calling this again on an already-initialised amp wipes any
 *   tuning (@ref tas2563_load_tuning), I2S configuration (@ref
 *   tas2563_configure_i2s) and IV-sense configuration (@ref
 *   tas2563_configure_iv_sense) already loaded -- the software reset
 *   above returns every register to its POR default, unconditionally.
 *
 * @param[out] ctx       Driver context (output; populated on success).
 * @param[in]  bus       Open I2C bus handle the amp sits on.
 * @param[in]  addr_7bit 7-bit I2C address of a single strap-selected
 *                       chip (TAS2563_I2C_ADDR_GND_DIRECT ..
 *                       TAS2563_I2C_ADDR_VDD_DIRECT).
 *                       TAS2563_I2C_ADDR_BROADCAST is rejected --
 *                       validated here, once, before ctx->addr is
 *                       ever assigned; every later call trusts
 *                       ctx->initialised instead of re-checking the
 *                       address.  0x48 does not identify a single
 *                       chip, and this context both reads and writes
 *                       through ctx->addr, so neither direction is
 *                       safe there (#1846).
 * @param[in]  sd_n      Open GPIO handle bound to AMP.ENABLE.  May
 *                       be NULL if the caller drives SD_N
 *                       elsewhere (or if the pin is tied permanently
 *                       to V+).  @b Must not be passed to more than
 *                       one @ref tas2563_t instance that shares the
 *                       same physical SD_N net -- see "Shared SD_N /
 *                       IRQZ nets" in this file's overview.  Must be
 *                       declared `GPIO_ACTIVE_HIGH` in its devicetree
 *                       pin-array entry -- see "sd_n must be declared
 *                       GPIO_ACTIVE_HIGH" in this file's overview.
 *
 * @pre SDZ must have been high for at least @ref
 *      TAS2563_RESET_SETTLE_US before the first I2C access (SLASET3D
 *      §7.3.11.1 / §9.2).  If @p sd_n is non-NULL, this function drives
 *      SDZ and applies that wait itself.  If @p sd_n is NULL, SD_N is
 *      owned elsewhere and the CALLER must have released it at least
 *      that long ago before calling this function -- it has no way to
 *      know when an externally-owned pin actually went high.
 *
 * @note This does not select, and cannot report, ROM vs Smart
 *   Amp/Tuning mode (see @ref tas2563_set_amp_level's note) -- not
 *   because it avoids touching the relevant registers (the software
 *   reset above touches every register), but because SLASET3D names
 *   no register for that distinction at all, so there is nothing this
 *   function could set even if it tried.
 *
 * @return ALP_OK on a successful probe.
 * @retval ALP_ERR_INVAL  ctx or bus is NULL, or addr_7bit is not one
 *                        of the four strap constants.
 * @retval (other)        Whatever status the I2C connectivity probe's
 *                        bus call returned, propagated verbatim --
 *                        typically ALP_ERR_IO (NACK / bus fault),
 *                        ALP_ERR_NOT_READY (bus handle closed) or
 *                        ALP_ERR_NOSUPPORT.  See @ref alp_i2c_write
 *                        and @ref alp_i2c_write_read for the full set.
 *
 * sd_n configure/write failures also pass their own status straight
 * through, same as the connectivity probe.
 *
 * After the probe succeeds, init writes `PWR_CTL.MODE = 10b`
 * (software shutdown) rather than assuming it.  The unconditional
 * software reset above already restores `PWR_CTL` to its POR default
 * (`Eh`, `MODE = 10b` -- SLASET3D §7.5.4 Table 7-104, p.66), so this
 * write is redundant on every path that reaches it today -- kept
 * anyway as an explicit, cheap statement of the state this function
 * hands back, in case a future change ever makes the software reset
 * above conditional again.
 */
alp_status_t tas2563_init(tas2563_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit, alp_gpio_t *sd_n);

/** @brief Read the chip's revision register (a no-op-ish sanity check). */
alp_status_t tas2563_read_revision(tas2563_t *ctx, uint8_t *rev_out);

/**
 * @brief Switch operating mode via a read-modify-write of
 *        `PWR_CTL.MODE[1:0]`.
 *
 * Touches bits 1..0 only; `ISNS_PD`/`VSNS_PD` (bits 3..2) and the
 * rest of `PWR_CTL` are preserved, so changing mode does not disturb
 * the IV-sense power state set by @ref tas2563_configure_iv_sense
 * (SLASET3D §7.5.4 Table 7-104, p.66).
 *
 * @param[in] ctx   Initialised context.
 * @param[in] mode  One of @ref tas2563_mode_t.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY ctx is NULL or not initialised.
 * @retval ALP_ERR_INVAL     @p mode is not one of the three
 *                           @ref tas2563_mode_t encodings -- notably
 *                           `11b`, load diagnostics followed by
 *                           device ACTIVE, is refused.
 */
alp_status_t tas2563_set_mode(tas2563_t *ctx, tas2563_mode_t mode);

/**
 * @brief Drive AMP.ENABLE high (resume) or low (hardware shutdown).
 *
 * Bypasses MODE_CTRL -- when SD_N is low, the chip is in HW
 * shutdown regardless of MODE_CTRL.  Useful for fast power-down
 * during a fault or for staging the boot sequence.
 *
 * @warning If this instance's @p sd_n net is shared with another amp
 *   (see "Shared SD_N / IRQZ nets" above), this also puts THAT amp
 *   into hardware shutdown.
 */
alp_status_t tas2563_set_hw_enable(tas2563_t *ctx, bool enable);

/**
 * @brief Set the Class-D output level (`PB_CFG1.AMP_LEVEL`).
 *
 * The one knob in this API that decides how loud the part gets, and
 * the reason it exists is that the power-on value is not quiet:
 * `10h` = 16.0 dBV / 8.92 Vpk, roughly 9.9 W peak into 4 ohm and only
 * 6 dB below the maximum (SLASET3D §7.5.5 Table 7-105, p.67).  A
 * caller with speakers attached should call this with a level chosen
 * for the enclosure before the first @ref tas2563_set_mode with
 * @ref TAS2563_MODE_ACTIVE -- and after @ref tas2563_load_tuning if
 * both are used, since a tuning may legitimately set this register
 * too.
 *
 * Read-modify-write of bits 5..1 only; `DIS_DC_BLOCKER` (bit 6) and
 * the reserved bits keep their values.
 *
 * @note @b Mode-dependent, per TI's application notes (not SLASET3D
 *   itself): SLAA953 §1.9 (p.9), a note box, states in full: "Amplifier
 *   Level cannot be changed in Smart Amp/Tuning Mode. In order to
 *   change it, user must enter ROM mode in Test and Measurement panel
 *   in the Device Home page."  "Test and Measurement panel" and
 *   "Device Home page" are PPC3 GUI elements -- this is a PPC3-tool
 *   workflow note, not a documented register-level rule, and SLASET3D
 *   itself never mentions ROM mode or this restriction at all.  This
 *   function does not, and cannot, enforce or even detect it: SLASET3D's
 *   register map has no field this driver could read to tell ROM mode
 *   from Smart Amp/Tuning mode (searched exhaustively -- see @ref
 *   tas2563_load_tuning's write verification note for the same kind of
 *   gap; the one register whose name suggests it, `DSP Mode & TDM_DET`
 *   at 0x11 / §7.5.19, is read-only TDM clock-detection readback
 *   (`FS_RATIO`/`FS_RATE`, correctly documented, despite its title) and
 *   has no mode-select field).  A caller in Smart Amp/Tuning mode that
 *   calls this and gets `ALP_OK` should not assume the level actually
 *   changed on the part -- confirm via PPC3, outside this driver's
 *   knowledge, per SLAA953's workflow note above.
 *
 * @param[in] ctx         Initialised context.
 * @param[in] level_code  Datasheet `AMP_LEVEL[4:0]` code,
 *                        @ref TAS2563_AMP_LEVEL_MIN ..
 *                        @ref TAS2563_AMP_LEVEL_MAX, in 0.5 dBV steps
 *                        from 8.5 dBV.  Not a dB value -- the
 *                        datasheet's own code, so a reader can check
 *                        it against Table 7-105 without arithmetic.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY    ctx is NULL or not initialised.
 * @retval ALP_ERR_OUT_OF_RANGE @p level_code is `00h`, or falls in the
 *                              Reserved `1Dh`-`1Fh` range, or is
 *                              wider than the 5-bit field.
 */
alp_status_t tas2563_set_amp_level(tas2563_t *ctx, uint8_t level_code);

/**
 * @brief Tell the amp what the host I2S bus is doing.
 *
 * Translates the @ref alp_i2s_config_t the caller passed to
 * @ref alp_i2s_open into the amp's `TDM_CFG0`/`TDM_CFG1`/`TDM_CFG2`
 * registers so both ends of SDIN agree on rate, word width and
 * framing.  The amp is a receiver on this link -- it never sources
 * SBCLK or FSYNC -- so this is bookkeeping, not clock configuration.
 *
 * What is written (all read-modify-write, reserved bits preserved):
 *   - `TDM_CFG0.SAMP_RATE[3:1]` from @c sample_rate_hz.  The encoding
 *     is shared between the 44.1 kHz and 48 kHz families (e.g. `100b`
 *     is "44.1/48 kHz"), so 44100 and 48000 write the same value
 *     (SLASET3D §7.5.8 Table 7-108, p.69).
 *   - `TDM_CFG1.RX_OFFSET[5:1]` and `TDM_CFG1.RX_JUSTIFY` from
 *     @c format.  The fields are defined in §7.5.9 Table 7-109 (p.69)
 *     but the offset VALUES come from §7.4.2 (p.41), which states the
 *     mapping outright: `RX_OFFSET` is "typically set to a value of 0
 *     for Left Justified format and 1 for an I2S format".  So: one
 *     SBCLK of offset and left justification for
 *     @ref ALP_I2S_FMT_I2S; zero offset and left justification for
 *     @ref ALP_I2S_FMT_LEFT_JUSTIFIED; zero offset and RIGHT
 *     justification for @ref ALP_I2S_FMT_RIGHT_JUSTIFIED.  That last
 *     one is derived, not read off a table -- p.41's sentence covers
 *     only the first two, and `RX_JUSTIFY` is what selects
 *     justification within the slot.
 *   - `TDM_CFG2.RX_WLEN[3:2]` and `TDM_CFG2.RX_SLEN[1:0]` from
 *     @c word_bits, and `TDM_CFG2.RX_SCFG[5:4]` from @p channel
 *     (§7.5.10 Table 7-110, p.70).
 *
 * What is deliberately left alone: `TDM_CFG0.AUTO_RATE` keeps its
 * reset value (`0b`, auto rate detection *enabled*), so the amp still
 * measures the incoming clock and `SAMP_RATE` acts as a declaration
 * rather than an override; `CLASSD_SYNC`/`RAMP_RATE` stay at reset,
 * which is what makes the 44.1-vs-48 distinction irrelevant above
 * (§7.5.8 Table 7-108, p.69).  Slot length is set equal to word
 * length because @ref alp_i2s_config_t has no separate slot field --
 * a frame that carries 24-bit words in 32-bit slots needs a direct
 * `TDM_CFG2` write, not this helper.  Table 7-110's 20-bit `RX_WLEN`
 * encoding is not mapped: `word_bits` is documented 16/24/32, so no
 * host bus this pairs with can be opened at 20 bits, and 20 lands in
 * @ref ALP_ERR_OUT_OF_RANGE with every other unencodable width.
 *
 * Two Table 7-23 (§7.4.2, p.40) caveats the rate mapping does not
 * change but a caller should know: that table marks the `000b`
 * (7.35/8 kHz) and `010b` (22.05/24 kHz) encodings **Reserved**, while
 * the Table 7-108 field description lists them as real rates -- the
 * driver follows the field table and emits both, see
 * `chips/tas2563/tas2563.c`'s `samp_rate_code()` for the full
 * reasoning.  And 192 kHz is internally down-sampled to 96 kHz
 * (p.40), so content above 40 kHz must not be applied at that rate.
 *
 * @param[in] ctx      Initialised context.
 * @param[in] host_cfg The config the host I2S bus was opened with.
 * @param[in] channel  Which receive slot this mono amp plays.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY    ctx is NULL or not initialised.
 * @retval ALP_ERR_INVAL        @p host_cfg is NULL, or @p channel is
 *                              not a @ref tas2563_rx_channel_t value.
 * @retval ALP_ERR_OUT_OF_RANGE @c sample_rate_hz or @c word_bits has
 *                              no encoding in Table 7-108/7-110.
 * @retval ALP_ERR_NOSUPPORT    @c format is a PCM frame-sync format
 *                              (@ref ALP_I2S_FMT_PCM_SHORT /
 *                              @ref ALP_I2S_FMT_PCM_LONG).  `TDM_CFG1`
 *                              has no field expressing a short- or
 *                              long-frame-sync PCM frame, so there is
 *                              nothing to write; refused rather than
 *                              silently framed as I2S.
 */
alp_status_t tas2563_configure_i2s(tas2563_t              *ctx,
                                   const alp_i2s_config_t *host_cfg,
                                   tas2563_rx_channel_t    channel);

/**
 * @brief Enable or disable the IV-sense return path on SDOUT.
 *
 * The amp measures speaker voltage and current and transmits them
 * back to the host on its TDM TX pin, which is the host's I2S RX.
 * Two things have to line up for that data to appear: the sense
 * blocks must be powered (`PWR_CTL.VSNS_PD`/`ISNS_PD`, both `1h` =
 * powered *down* at reset -- SLASET3D §7.5.4 Table 7-104, p.66), and
 * each measurement needs a transmit slot (`TDM_CFG5.VSNS_TX` +
 * `VSNS_SLOT[5:0]`, `TDM_CFG6.ISNS_TX` + `ISNS_SLOT[5:0]` --
 * §7.5.13 Table 7-113 and §7.5.14 Table 7-114, p.71).  This does
 * both, in that order, so a slot is never enabled against a
 * powered-down sense block.
 *
 * Sample width on the wire is `TDM_CFG2.IVMON_LEN[7:6]`, left at its
 * reset value `01b` = 16 bits (§7.5.10 Table 7-110, p.70).
 *
 * @param[in] ctx     Initialised context.
 * @param[in] enable  true powers up both sense blocks and enables
 *                    both transmit slots; false disables both slots
 *                    and powers both blocks back down.
 * @param[in] v_slot  Voltage-sense transmit slot, 0..63.  Ignored
 *                    when @p enable is false.
 * @param[in] i_slot  Current-sense transmit slot, 0..63.  Ignored
 *                    when @p enable is false.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY    ctx is NULL or not initialised.
 * @retval ALP_ERR_OUT_OF_RANGE @p v_slot or @p i_slot exceeds 63, the
 *                              width of the 6-bit slot fields.
 */
alp_status_t
tas2563_configure_iv_sense(tas2563_t *ctx, bool enable, uint8_t v_slot, uint8_t i_slot);

/**
 * @brief Bind the amp's open-drain IRQ_N to a host GPIO and point the
 *        chip's IRQZ output at the latched interrupt registers.
 *
 * IRQ_N is an open-drain output that pulls low on an unmasked fault
 * and therefore needs a pull-up to IOVDD; the part has a 20 kOhm
 * internal one behind `MISC_CFG1.IRQZ_PU` (bit 3, reset `0h` =
 * disabled -- SLASET3D §7.3.12 Figure 7-10 p.36, §7.5.6 Table 7-106
 * p.68, §7.3.12 Table 7-12 p.37).  The host pin is configured as an
 * input with its own internal pull-up as well, matching the EVK
 * routing of `EVK_PIN_AMP_FAULT`.
 *
 * `INT & CLK CFG.IRQZ_PIN_CFG[1:0]` is set to `01b`, "assert on any
 * unmasked latched interrupts" -- also the reset value -- so a fault
 * holds the pin low until @ref tas2563_clear_faults, rather than
 * pulsing (§7.5.43 Table 7-143, p.86).
 *
 * The four `INT_MASK` registers are left at their reset values, which
 * already leave the safety-critical events unmasked: over-temperature
 * and over-current (`INT_MASK0` = `FCh`), VBAT brown-out and speaker
 * open/short load (`INT_MASK1` = `A6h`), VBAT POR (`INT_MASK2` =
 * `DFh`) -- SLASET3D §7.5.28-§7.5.31, p.77-80.  TDM clock error
 * (`INT_MASK0[2]`) is masked at reset; a caller who wants it on the
 * pin has to write `INT_MASK0` directly.
 *
 * @param[in] ctx                  Initialised context.
 * @param[in] irq_n                Open GPIO handle bound to
 *                                 AMP.FAULT.  Stored in @p ctx.
 * @param[in] chip_internal_pullup Enable the amp's own 20 kOhm
 *                                 pull-up.  Pass false when the board
 *                                 already fits an external pull-up on
 *                                 IRQ_N.
 *
 * @note If @p irq_n's net is shared with another amp (see "Shared
 *   SD_N / IRQZ nets" above), a pin assertion means only that ONE of
 *   the amps on it faulted -- read each instance's own @ref
 *   tas2563_read_faults over I2C to find out which.
 *
 * @return ALP_OK, or the underlying bus/GPIO status.
 * @retval ALP_ERR_NOT_READY ctx is NULL or not initialised.
 * @retval ALP_ERR_INVAL     @p irq_n is NULL.
 */
alp_status_t
tas2563_configure_fault_pin(tas2563_t *ctx, alp_gpio_t *irq_n, bool chip_internal_pullup);

/**
 * @brief Is the fault pin currently asserted?
 *
 * A pin-level read only -- no I2C traffic, so it is cheap enough for
 * a polling loop or an IRQ callback.  IRQ_N is active low
 * (SLASET3D §7.3.12 Table 7-13, p.37), so a low pin reads as
 * asserted.
 *
 * @param[in]  ctx          Initialised context with a fault pin bound.
 * @param[out] asserted_out Receives true when IRQ_N is low.
 *
 * @return ALP_OK, or the underlying GPIO status.
 * @retval ALP_ERR_NOT_READY  ctx is NULL or not initialised.
 * @retval ALP_ERR_INVAL      @p asserted_out is NULL.
 * @retval ALP_ERR_NOSUPPORT  No fault pin was bound.
 */
alp_status_t tas2563_fault_asserted(tas2563_t *ctx, bool *asserted_out);

/**
 * @brief Read the four latched-interrupt registers as one bitmask.
 *
 * Reads `INT_LTCH0` (0x24), `INT_LTCH1` (0x25), `INT_LTCH3` (0x26)
 * and `INT_LTCH4` (0x27) and packs them byte-aligned into bits
 * 0..7 / 8..15 / 16..23 / 24..31 -- see the `TAS2563_FAULT_*` macros.
 *
 * @warning SLASET3D CONTRADICTS ITSELF ON WHAT THIS READ DOES TO THE
 *   LATCHES, so this function promises neither behaviour.  §7.3.12
 *   (p.36) says plainly: "Reading the latched fault status register
 *   (INT_LTCH[7:0]) clears the register."  The field tables for the
 *   same registers say the opposite -- every bit in §7.5.36-§7.5.39
 *   (p.82-85) is described as "cleared using CLR_INTP_LTCH", which is
 *   what @ref tas2563_clear_faults writes.  If p.36 is right this call
 *   is destructive: a second reader, or a retry after a transient bus
 *   error, sees zero and the fault is gone.  If the field tables are
 *   right, bits persist until @ref tas2563_clear_faults.  Nobody has
 *   run this on silicon and no test can decide it (the fake models the
 *   field-table behaviour, which is a choice, not evidence), so treat
 *   one read as possibly the only chance to see a given fault: capture
 *   the returned mask and work from that copy rather than re-reading
 *   and assuming the second read agrees.
 *
 * Does not require a fault pin; the latched registers are readable
 * whether or not IRQ_N is wired.
 *
 * @param[in]  ctx        Initialised context.
 * @param[out] faults_out Receives the packed fault bitmask.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY ctx is NULL or not initialised.
 * @retval ALP_ERR_INVAL     @p faults_out is NULL.
 */
alp_status_t tas2563_read_faults(tas2563_t *ctx, uint32_t *faults_out);

/**
 * @brief Clear every latched fault and release IRQ_N.
 *
 * Sets the self-clearing `CLR_INTP_LTCH` bit (bit 2 of `INT & CLK
 * CFG`, 0x30 -- SLASET3D §7.5.43 Table 7-143, p.86; §7.3.12
 * Table 7-11, p.37).  A fault whose cause is still present will
 * simply latch again on the next event.
 *
 * @param[in] ctx  Initialised context.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY ctx is NULL or not initialised.
 */
alp_status_t tas2563_clear_faults(tas2563_t *ctx);

/**
 * @brief Replay a tuning register stream into the chip.
 *
 * @warning Calling @ref tas2563_init again on an already-tuned amp
 *   wipes whatever this function loaded -- @ref tas2563_init's
 *   unconditional software reset returns every register to its POR
 *   default, tuning coefficients included.  Reload the tuning after
 *   any re-init, not just the first init.
 *
 * Smart-amp tuning (EQ, DRC, excursion and thermal models) lives in
 * on-chip DSP coefficient pages that TI's PPC3 host tool produces.
 * This function applies such a tuning as an explicit sequence of
 * (book, page, register, value) writes: it tracks the currently
 * selected book and page and only re-selects them when a record
 * changes them, then writes each register with a single-byte write
 * (SLASET3D §7.3.6 Figure 7-6, p.33).  One transaction per register is
 * DELIBERATE CONSERVATISM, not an undocumented behaviour: SLASET3D
 * §7.3.5 "Single-Byte and Multiple-Byte Transfers" (p.32) does state
 * that "the register issued then serves as the starting point, and
 * the amount of data subsequently transmitted... determines to how
 * many registers are written," i.e. sequential addressing does
 * auto-increment.  This driver has no silicon on which to have
 * exercised that path, so it stays on the single-byte write it has
 * verified against the fake, pending that confirmation.
 *
 * @par Not included: parsing TI's PPC3 export container.
 *   Converting a PPC3 `.bin`/`.cfg` export into a
 *   @ref tas2563_tuning_reg_t array is a host-side step, not part of
 *   this driver: the export format is a TI tool format, described
 *   nowhere in SLASET3D, and writing a parser for it without a real
 *   export to check against would be guesswork.  Feed this function
 *   an array your build produced from the export.  Per-unit speaker
 *   calibration (measuring each assembled unit's actual speaker
 *   parameters and provisioning a matching tuning) is a separate,
 *   out-of-scope concern this function has no part in either -- it
 *   only replays whatever @ref tas2563_tuning_reg_t array the caller's
 *   provisioning step already produced.
 *
 * @par Cost.  Four bytes of storage per register written.  A large
 *   DSP tuning is thousands of registers, so a big blob is expensive
 *   in flash in this form; a packed encoding is the follow-up, and it
 *   depends on first confirming incremental multi-byte writes on real
 *   silicon.
 *
 * @par Write verification: not implemented, and not guessed at.
 *   SLASET3D §7.3.2 "Device Mode and Address Selection" (p.29) itself
 *   calls `I2C_CKSUM` a CRC and says it should be checked, over each
 *   device's own local address, after writing multiple devices via the
 *   global broadcast address (where individual ACK/NACK cannot be used
 *   since every device on the bus answers at once).  TI's application
 *   notes (SLAA765 §16, p.25) separately show the command syntax for
 *   reading it back -- examples only, no algorithm.  SLASET3D §7.5.61
 *   (p.93) documents the register -- 8 bits, RW,
 *   "updated on writes to other registers on all books and pages,"
 *   writing it resets it to the written value -- but nowhere specifies
 *   the checksum ALGORITHM: not what it sums (register address, data,
 *   both?), not whether `PAGE`/`BOOK` select writes count as "other
 *   registers," not an initial value beyond the POR `0h`.  Without
 *   that, a host-side expected value cannot be computed, only guessed,
 *   so this function does not attempt it.  A future implementation
 *   needs the algorithm from TI support or from reading it back
 *   against known writes on real silicon.
 *
 * Book and page are restored to 0 before returning, on both the
 * success and the failure path, because every other function in this
 * driver assumes book 0 / page 0.
 *
 * @param[in]  ctx               Initialised context.
 * @param[in]  records           Tuning records, applied in order.
 * @param[in]  count             Number of records.  Zero is a no-op
 *                               that still returns ALP_OK.
 * @param[out] failed_index_out  On a non-OK return caused by a record,
 *                               receives the index of the offending
 *                               record.  Untouched on ALP_OK.  May be
 *                               NULL.
 *
 * @return ALP_OK, or the underlying bus status.
 * @retval ALP_ERR_NOT_READY ctx is NULL or not initialised.
 * @retval ALP_ERR_INVAL     @p records is NULL with a non-zero
 *                           @p count; or a record targets `PAGE`
 *                           (0x00) or `BOOK` (0x7F), which are this
 *                           function's own bookkeeping registers; or
 *                           a record targets one of the driver-owned
 *                           control registers in book 0 / page 0 --
 *                           `SW_RESET` (0x01), `PWR_CTL` (0x02),
 *                           `MISC` (0x32, `IRQZ_POL`) or `TG_CFG0`
 *                           (0x3F).  Each of those changes something
 *                           the caller believes it configured, with
 *                           no readback saying so: the operating mode,
 *                           a full register reset mid-load, the
 *                           polarity @ref tas2563_fault_asserted
 *                           assumes, or the tone generator.
 *                           `PB_CFG1` (0x03, `AMP_LEVEL`) is
 *                           deliberately NOT blocked -- output level
 *                           is what a smart-amp tuning is for, and
 *                           blocking it would refuse real PPC3
 *                           exports; use @ref tas2563_set_amp_level
 *                           afterwards instead.
 */
alp_status_t tas2563_load_tuning(tas2563_t                  *ctx,
                                 const tas2563_tuning_reg_t *records,
                                 size_t                      count,
                                 size_t                     *failed_index_out);

/**
 * @brief Release the driver context.  Drops SD_N before returning.
 *
 * When no SD_N pin was supplied there is no hardware line to drop, so
 * deinit falls back to writing software shutdown instead of leaving a
 * possibly-active amplifier behind.
 *
 * @warning If this instance's @p sd_n net is shared with another amp
 *   (see "Shared SD_N / IRQZ nets" above), this also puts THAT amp
 *   into hardware shutdown -- do not call with a shared @p sd_n while
 *   another instance on the same net needs to keep running.
 */
void tas2563_deinit(tas2563_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TAS2563_H */
