/*
 * src/platform/panel_retry.h -- the HX8394 panel bring-up retry, pure logic
 * (no Zephyr), so tests/host/test_panel_retry.c can drive it with stubs.
 * src/platform/panel.c supplies the real ops.
 */
#ifndef TR_PANEL_RETRY_H
#define TR_PANEL_RETRY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	void *ctx;
	/* Attempt 0: the first init. Attempt > 0: reset the panel and re-run
	 * the driver's own init. 0 = success. */
	int (*init)(void *ctx, unsigned attempt);
	bool (*bl_on)(void *ctx);   /* backlight pin is an output, driven high */
	int (*bl_force)(void *ctx); /* make it so (static high, never a pulse) */
} tr_panel_ops_t;

/* The result word (tr_panel_init_tries): bits 0..7 attempts made, bit 8 the
 * panel is up with the backlight on, bit 9 the backlight had to be forced
 * after a successful init, bits 16..23 the last error as a positive errno. */
#define TR_PANEL_OK          (1u << 8)
#define TR_PANEL_BL_FORCED   (1u << 9)
#define TR_PANEL_TRIES(w)    ((w) & 0xffu)
#define TR_PANEL_ERR(w)      (((w) >> 16) & 0xffu)
#define TR_PANEL_ERR_BL_STUCK 5u /* EIO: forced, and the pin still reads off */

static inline uint32_t tr_panel_bringup(const tr_panel_ops_t *ops, unsigned max_tries)
{
	unsigned n  = 0;
	int      rc = -1;

	while (n < max_tries && rc != 0) {
		rc = ops->init(ops->ctx, n++);
	}
	if (rc != 0) {
		return n | ((uint32_t)(rc < 0 ? -rc : rc) & 0xffu) << 16;
	}
	if (ops->bl_on(ops->ctx)) {
		return n | TR_PANEL_OK;
	}
	(void)ops->bl_force(ops->ctx);
	if (!ops->bl_on(ops->ctx)) {
		return n | TR_PANEL_BL_FORCED | TR_PANEL_ERR_BL_STUCK << 16;
	}
	return n | TR_PANEL_OK | TR_PANEL_BL_FORCED;
}

/* src/platform/panel.c: the device ops, run once from main() before the
 * display opens; returns (and publishes as tr_panel_init_tries) the word. */
uint32_t tr_panel_up(void);

#endif /* TR_PANEL_RETRY_H */
