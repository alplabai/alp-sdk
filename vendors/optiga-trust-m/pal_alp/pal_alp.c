/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OPTIGA Trust M host-library platform abstraction layer (PAL) on the
 * portable alp surface: <alp/peripheral.h>'s alp_i2c_* for the bus and
 * alp_uptime_ms / alp_delay_ms for time.  One PAL, so the same
 * chips/optiga_trust_m driver runs on the A55 (Linux, alp_i2c over
 * i2c-dev) and on an MCU core (Zephyr, alp_i2c over the Zephyr I2C
 * driver) -- upstream's own Linux and Zephyr PALs each open the bus
 * behind the SDK's back (/dev/i2c-N, DT_ALIAS(optiga_i2c)).
 *
 * Threadless: upstream's PALs run the library's scheduler from a timer
 * thread / work queue.  Here pal_os_event_register_callback_oneshot()
 * only records the callback and its due time, and the caller drains it
 * with alp_optiga_pal_poll() from its own wait loop
 * (chips/optiga_trust_m/optiga_trust_m.c).  I2C completions are
 * delivered synchronously from pal_i2c_write/read, as upstream's Linux
 * and Zephyr PALs also do.
 *
 * Single instance: the host library carries exactly one IFX I2C
 * context (ifx_i2c_context_0 -> optiga_pal_i2c_context_0), so one
 * OPTIGA per image.  alp_optiga_pal_bind() points it at a bus.
 *
 * No VDD / RESET GPIOs: on V2N/V2M SE_RST is not wired to the SoC (it
 * hangs off the GD32 supervisor), so the library is built with
 * OPTIGA_USE_SOFT_RESET and the pal_gpio_* calls are no-ops.
 */

#include <stdlib.h>
#include <string.h>

#include "alp/peripheral.h"
#include "pal.h"
#include "pal_alp.h"
#include "pal_gpio.h"
#include "pal_i2c.h"
#include "pal_ifx_i2c_config.h"
#include "pal_logger.h"
#include "pal_os_event.h"
#include "pal_os_lock.h"
#include "pal_os_memory.h"
#include "pal_os_timer.h"

/* ---------------------------------------------------------------- config */

pal_i2c_t  optiga_pal_i2c_context_0 = { NULL, NULL, NULL, 0x30 };
pal_gpio_t optiga_vdd_0             = { NULL };
pal_gpio_t optiga_reset_0           = { NULL };

void alp_optiga_pal_bind(alp_i2c_t *bus, uint8_t addr_7bit)
{
	optiga_pal_i2c_context_0.p_i2c_hw_config = bus;
	optiga_pal_i2c_context_0.slave_address   = addr_7bit;
}

pal_status_t pal_init(void)
{
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_deinit(void)
{
	return PAL_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ gpio */

pal_status_t pal_gpio_init(const pal_gpio_t *p_gpio_context)
{
	(void)p_gpio_context;
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_gpio_deinit(const pal_gpio_t *p_gpio_context)
{
	(void)p_gpio_context;
	return PAL_STATUS_SUCCESS;
}

void pal_gpio_set_high(const pal_gpio_t *p_gpio_context)
{
	(void)p_gpio_context;
}

void pal_gpio_set_low(const pal_gpio_t *p_gpio_context)
{
	(void)p_gpio_context;
}

/* ------------------------------------------------------------------- i2c */

static void i2c_done(const pal_i2c_t *p, optiga_lib_status_t event)
{
	((upper_layer_callback_t)p->upper_layer_event_handler)(p->p_upper_layer_ctx, event);
}

pal_status_t pal_i2c_init(const pal_i2c_t *p_i2c_context)
{
	if (p_i2c_context == NULL || p_i2c_context->p_i2c_hw_config == NULL) return PAL_STATUS_FAILURE;
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_i2c_deinit(const pal_i2c_t *p_i2c_context)
{
	(void)p_i2c_context;
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_i2c_write(const pal_i2c_t *p_i2c_context, uint8_t *p_data, uint16_t length)
{
	if (p_i2c_context == NULL || p_i2c_context->p_i2c_hw_config == NULL || p_data == NULL ||
	    length == 0u) {
		return PAL_STATUS_FAILURE;
	}
	alp_status_t s = alp_i2c_write(
	    (alp_i2c_t *)p_i2c_context->p_i2c_hw_config, p_i2c_context->slave_address, p_data, length);
	/* A NACK is normal (the part is busy or asleep): report it as a bus
	 * error and the physical layer polls again, as on every PAL. */
	i2c_done(p_i2c_context, s == ALP_OK ? PAL_I2C_EVENT_SUCCESS : PAL_I2C_EVENT_ERROR);
	return s == ALP_OK ? PAL_STATUS_SUCCESS : PAL_STATUS_FAILURE;
}

pal_status_t pal_i2c_read(const pal_i2c_t *p_i2c_context, uint8_t *p_data, uint16_t length)
{
	if (p_i2c_context == NULL || p_i2c_context->p_i2c_hw_config == NULL || p_data == NULL ||
	    length == 0u) {
		return PAL_STATUS_FAILURE;
	}
	alp_status_t s = alp_i2c_read(
	    (alp_i2c_t *)p_i2c_context->p_i2c_hw_config, p_i2c_context->slave_address, p_data, length);
	i2c_done(p_i2c_context, s == ALP_OK ? PAL_I2C_EVENT_SUCCESS : PAL_I2C_EVENT_ERROR);
	return s == ALP_OK ? PAL_STATUS_SUCCESS : PAL_STATUS_FAILURE;
}

pal_status_t pal_i2c_set_bitrate(const pal_i2c_t *p_i2c_context, uint16_t bitrate)
{
	/* The bus speed is the board's (DT / i2c-dev); alp_i2c has no
	 * runtime rate knob, so accept the request as-is. */
	(void)bitrate;
	if (p_i2c_context == NULL) return PAL_STATUS_FAILURE;
	if (p_i2c_context->upper_layer_event_handler != NULL) {
		i2c_done(p_i2c_context, PAL_I2C_EVENT_SUCCESS);
	}
	return PAL_STATUS_SUCCESS;
}

/* ----------------------------------------------------------------- timer */

uint32_t pal_os_timer_get_time_in_microseconds(void)
{
	return (uint32_t)(alp_uptime_ms() * 1000u);
}

uint32_t pal_os_timer_get_time_in_milliseconds(void)
{
	return (uint32_t)alp_uptime_ms();
}

void pal_os_timer_delay_in_milliseconds(uint16_t milliseconds)
{
	alp_delay_ms(milliseconds);
}

pal_status_t pal_timer_init(void)
{
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_timer_deinit(void)
{
	return PAL_STATUS_SUCCESS;
}

/* ----------------------------------------------------------------- event */

static pal_os_event_t pal_os_event_0;
static uint64_t       event_due_ms;

void pal_os_event_start(pal_os_event_t   *p_pal_os_event,
                        register_callback callback,
                        void             *callback_args)
{
	if (p_pal_os_event->is_event_triggered == 0u) {
		p_pal_os_event->is_event_triggered = TRUE;
		pal_os_event_register_callback_oneshot(p_pal_os_event, callback, callback_args, 1000);
	}
}

void pal_os_event_stop(pal_os_event_t *p_pal_os_event)
{
	p_pal_os_event->is_event_triggered = 0u;
}

pal_os_event_t *pal_os_event_create(register_callback callback, void *callback_args)
{
	if (callback != NULL && callback_args != NULL) {
		pal_os_event_start(&pal_os_event_0, callback, callback_args);
	}
	return &pal_os_event_0;
}

void pal_os_event_destroy(pal_os_event_t *pal_os_event)
{
	(void)pal_os_event;
}

void pal_os_event_register_callback_oneshot(pal_os_event_t   *p_pal_os_event,
                                            register_callback callback,
                                            void             *callback_args,
                                            uint32_t          time_us)
{
	p_pal_os_event->callback_registered = callback;
	p_pal_os_event->callback_ctx        = callback_args;
	/* Round up: a 500 us wait must not fire in the same millisecond. */
	event_due_ms = alp_uptime_ms() + (time_us + 999u) / 1000u;
}

void pal_os_event_trigger_registered_callback(void)
{
	register_callback cb               = pal_os_event_0.callback_registered;
	void             *ctx              = pal_os_event_0.callback_ctx;
	pal_os_event_0.callback_registered = NULL;
	pal_os_event_0.callback_ctx        = NULL;
	if (cb != NULL) cb(ctx);
}

bool alp_optiga_pal_poll(void)
{
	if (pal_os_event_0.callback_registered == NULL) return false;
	if (alp_uptime_ms() < event_due_ms) return false;
	pal_os_event_trigger_registered_callback();
	return true;
}

/* ------------------------------------------------------------------ lock */

void pal_os_lock_create(pal_os_lock_t *p_lock, uint8_t lock_type)
{
	p_lock->type = lock_type;
	p_lock->lock = 0u;
}

void pal_os_lock_destroy(pal_os_lock_t *p_lock)
{
	(void)p_lock;
}

/* Callers serialise on the driver's single instance, so the library's
 * own lock only has to be honest about re-entry, never block. */
pal_status_t pal_os_lock_acquire(pal_os_lock_t *p_lock)
{
	if (p_lock->lock != 0u) return PAL_STATUS_FAILURE;
	p_lock->lock = 1u;
	return PAL_STATUS_SUCCESS;
}

void pal_os_lock_release(pal_os_lock_t *p_lock)
{
	p_lock->lock = 0u;
}

void pal_os_lock_enter_critical_section(void)
{
}

void pal_os_lock_exit_critical_section(void)
{
}

/* ---------------------------------------------------------------- memory */

void *pal_os_malloc(uint32_t block_size)
{
	return malloc(block_size);
}

void *pal_os_calloc(uint32_t number_of_blocks, uint32_t block_size)
{
	return calloc(number_of_blocks, block_size);
}

void pal_os_free(void *block)
{
	free(block);
}

void pal_os_memcpy(void *p_destination, const void *p_source, uint32_t size)
{
	memcpy(p_destination, p_source, size);
}

void pal_os_memset(void *p_buffer, uint32_t value, uint32_t size)
{
	memset(p_buffer, (int)value, size);
}

/* ---------------------------------------------------------------- logger */

pal_logger_t logger_console = { 0 };

pal_status_t pal_logger_init(void *p_logger_context)
{
	(void)p_logger_context;
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_logger_deinit(void *p_logger_context)
{
	(void)p_logger_context;
	return PAL_STATUS_SUCCESS;
}

pal_status_t
pal_logger_write(void *p_logger_context, const uint8_t *p_log_data, uint32_t log_data_length)
{
	(void)p_logger_context;
	(void)p_log_data;
	(void)log_data_length;
	return PAL_STATUS_SUCCESS;
}

pal_status_t pal_logger_read(void *p_logger_context, uint8_t *p_log_data, uint32_t log_data_length)
{
	(void)p_logger_context;
	(void)p_log_data;
	(void)log_data_length;
	return PAL_STATUS_FAILURE;
}
