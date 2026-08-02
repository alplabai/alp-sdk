/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * blink -- the canonical "first program": toggle an LED, forever.
 *
 * Every E1M SoM (E1M-AEN801, and the E1M-X family: V2N / V2N-M1) is a
 * bare compute module -- it has no LED of its own.  The LED lives on
 * the CARRIER BOARD it's plugged into, so "which pin is the LED" is
 * not a fact about the SoM: metadata/e1m_modules/E1M-AEN801.yaml (the
 * SoM's own metadata) carries no LED key at all.  It's a fact about
 * the board.yaml `preset:` this app builds against -- here `e1m-evk`
 * / `e1m-x-evk` (see board.yaml next to this file).  A different
 * carrier for the same SoM could wire no LED at all, or wire it
 * somewhere else entirely.
 *
 * Both Alp Lab EVKs put their only user-visible LED on an RGB
 * cluster riding PWM-capable pads -- neither has a dedicated single-
 * colour GPIO LED.  E1M makes GPIO a universal *secondary* function
 * on every digital pad (e1m-spec STANDARD.md "GPIO secondary"): a pad
 * whose default function is PWM can still be driven as a plain
 * digital output by opening its parallel `ALP_E1M_GPIO_<class><N>`
 * index instead of its peripheral id.  This demo claims the RGB
 * cluster's RED channel that way -- the same physical pad
 * `alp_pwm_open(ALP_E1M_PWM0)` would claim for a real PWM fade is
 * opened here instead as `alp_gpio_open(ALP_E1M_GPIO_PWM0)`.  Never
 * hold both against the same pad at once -- it's the same silicon
 * underneath.  (examples/peripheral-io/pwm-led-fade drives the RGB
 * GREEN channel instead, on ALP_E1M_PWM3 -- a different pad.)
 *
 * This demo opens its pin by the portable `BOARD_PIN_LED_RED` alias
 * from <alp/board.h>, not a raw pin number.  The facade selects the
 * active board's generated routes header at compile time from the
 * ALP_BOARD_<SLUG> define the build emits from board.yaml's
 * `preset:`, so this exact source builds unchanged for either EVK:
 *
 *   E1M EVK:   BOARD_PIN_LED_RED = EVK_PIN_LED_RED  = ALP_E1M_GPIO_PWM0
 *   E1M-X EVK: BOARD_PIN_LED_RED = XEVK_PIN_LED_RED = ALP_E1M_X_GPIO_PWM5
 *
 * Want green or blue instead?  Both EVK route tables declare the
 * whole RGB set -- EVK_PIN_LED_GREEN / EVK_PIN_LED_BLUE in
 * include/alp/boards/alp_e1m_evk_routes.h, XEVK_PIN_LED_GREEN /
 * XEVK_PIN_LED_BLUE in alp_e1m_x_evk_routes.h -- but only RED has a
 * cross-EVK BOARD_* alias in <alp/board.h> today.  Open the EVK_* /
 * XEVK_* macro directly (under an `#if defined(ALP_BOARD_E1M_EVK)` /
 * `ALP_BOARD_E1M_X_EVK` guard, same as alp/board.h does) for the
 * other two channels.
 *
 * native_sim wires a GPIO-emul controller in boards/ so the open /
 * configure / write path runs under CI -- see that overlay's header
 * comment for why the E1M-X variant's higher pin index degrades to a
 * clean "opened nothing, still exits" run there instead of a real
 * toggle.
 */

#include <stdio.h>

#include "alp/peripheral.h"

#include "alp/board.h"

/* Half-period of the blink, in milliseconds -- 200 ms each way gives a
 * ~2.5 Hz flash, fast enough to read as "alive" at a glance and slow
 * enough not to look like a solid LED. */
#define ALP_BLINK_PERIOD_MS 200u

/* How many toggles get a console line before the "[blink] done" marker
 * prints.  Only the automated harness cares; the LED keeps blinking
 * afterwards either way.  Kept small so a twister run finishes fast. */
#define ALP_BLINK_SELFTEST_TOGGLES 10u

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[blink] init led=BOARD_PIN_LED_RED\n");

	/* alp_gpio_open() returns a bare handle pointer, not an
	 * alp_status_t -- that return type was frozen at v0.1
	 * ([ABI-STABLE], <alp/peripheral.h>) with no room for a second
	 * out-of-band status.  NULL means "failed"; call alp_last_error()
	 * to learn WHY (unrouted pin, exhausted handle pool, ...). */
	alp_gpio_t *led = alp_gpio_open(BOARD_PIN_LED_RED);
	if (led == NULL) {
		/* Deliberately NOT "[blink] done": that string is the
		 * twister harness's success marker (testcase.yaml).  A
		 * failure path that still printed it would make the test
		 * pass while nothing toggled -- which is exactly the hole
		 * this example must not have, being the first thing a
		 * customer copies. */
		printf("[blink] failed: open status=%d\n", (int)alp_last_error());
		return 1;
	}

	alp_status_t s = alp_gpio_configure(led, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	if (s != ALP_OK) {
		printf("[blink] failed: configure status=%d\n", (int)s);
		alp_gpio_close(led);
		return 1;
	}

	/* Park the other two RGB channels OFF.
	 *
	 * The LED this demo drives is one colour channel of a shared RGB
	 * package (D19 on the E1M EVK).  Whatever ran before us may have
	 * left a sibling channel driven -- a previous app, or the resident
	 * boot image, since a debugger load does not reset the GPIO
	 * blocks.  Measured on the bench: the resident slot0 image left
	 * BOARD_PIN_LED_GREEN's pad configured as an output and held
	 * HIGH, so the board showed a SOLID GREEN behind this demo's
	 * blinking channel and "what colour is it" had no clean answer.
	 *
	 * Driving them low costs two GPIO writes and makes the demo
	 * self-isolating: what you see is what this app is doing.  Failing
	 * to park a sibling is NOT fatal -- the demo's own channel still
	 * works -- so this warns and continues rather than returning. */
	const uint32_t siblings[2] = { BOARD_PIN_LED_GREEN, BOARD_PIN_LED_BLUE };

	for (int i = 0; i < 2; i++) {
		alp_gpio_t *other = alp_gpio_open(siblings[i]);

		if (other == NULL) {
			printf("[blink] warn: sibling %d open failed: status=%d\n", i, (int)alp_last_error());
			continue;
		}
		if (alp_gpio_configure(other, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE) == ALP_OK) {
			(void)alp_gpio_write(other, false);
		}
		alp_gpio_close(other);
	}

	/* The point of the demo: toggle the LED so a human watching the
	 * board sees it blink -- so this loop NEVER ENDS.  That is
	 * deliberate and it is the whole difference between a first-blink
	 * demo and a self-test.
	 *
	 * An earlier draft stopped after 10 toggles (~2 s) and returned.
	 * On the bench that is indistinguishable from a dead board: the
	 * pad was verified toggling at the pin, but by the time anyone
	 * looked the app had exited with the LED off.  A blink example
	 * whose blink you have to catch inside a two-second window is not
	 * a blink example.
	 *
	 * The twister harness still gets its deterministic marker: the
	 * "[blink] done" line below prints once, after the first
	 * ALP_BLINK_SELFTEST_TOGGLES toggles, and the loop then keeps
	 * running forever.  A native_sim run under twister matches on that
	 * line and exits; real hardware keeps blinking. */
	bool     on    = false;
	unsigned count = 0;

	for (;;) {
		on = !on;
		s  = alp_gpio_write(led, on);
		if (s != ALP_OK) {
			printf("[blink] failed: write status=%d\n", (int)s);
			alp_gpio_close(led);
			return 1;
		}

		if (count < ALP_BLINK_SELFTEST_TOGGLES) {
			printf("[blink] led=%d status=%d\n", (int)on, (int)s);
			if (++count == ALP_BLINK_SELFTEST_TOGGLES) {
				/* Success marker: unreachable from any failure
				 * path above, so a green harness run means the
				 * LED was really driven. */
				printf("[blink] done\n");
			}
		}

		alp_delay_ms(ALP_BLINK_PERIOD_MS);
	}
}
