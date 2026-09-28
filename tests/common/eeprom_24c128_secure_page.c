/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit checks for eeprom_24c128_secure_page_write() and
 * eeprom_24c128_secure_page_lock() (both [UNTESTED] / [PAPER-ONLY] against
 * silicon -- see the header's @par Verification status block).
 *
 * Unlike every other chips/ test in this repo, this file does NOT link
 * alp::sdk -- it compiles chips/eeprom_24c128/eeprom_24c128.c directly and
 * supplies its OWN recording-fake implementations of the four external
 * symbols that file calls (alp_i2c_write, alp_i2c_write_read,
 * alp_delay_us, alp_delay_ms), instead of the real backend. That is what
 * makes the EXACT wire bytes these two functions emit -- and their call
 * ORDER -- checkable at all: an earlier revision of this file used
 * bus == NULL as a NACK-equivalent double (the technique
 * tests/common/eeprom_24c128_read_identity.c still uses) and could only
 * check the argument/state contract, not the frame content or ordering --
 * and both were wrong. eeprom_24c128_secure_page_write() put the wrong
 * selector in the wrong byte and overran the page by one byte;
 * eeprom_24c128_secure_page_lock() put the Lock Status selector in the
 * SECOND pointer byte instead of the first (turning the "lock" into a data
 * write that clobbers schema_version + sku[0]) and polled for a
 * write-complete ACK by writing to the page it had just locked, which a
 * CORRECT lock always NAKs -- so a real bench run would have reported a
 * successful lock as ALP_ERR_TIMEOUT. A monotonic sequence counter
 * (`g_seq`) stamped into every recorded write, write_read, AND
 * alp_delay_ms call additionally proves the lock command happens BEFORE
 * the post-lock wait, which happens BEFORE the confirming Lock Status
 * read -- catching, e.g., a confirm read moved ahead of the lock write
 * (which would read the PRE-lock state and misreport every real first-time
 * lock as ALP_ERR_IO) or the wait being silently deleted.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto     -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_eeprom_24c128_secure_page
 *   ctest --test-dir build -R alp_test_eeprom_24c128_secure_page
 *
 * or, under -DALP_OS=baremetal, the target is instead
 * alp_test_baremetal_eeprom_24c128_secure_page.
 */

#include <stdint.h>
#include <string.h>

#include "alp/chips/eeprom_24c128.h"
#include "alp/peripheral.h"

#include "test_assert.h"

/* ------------------------------------------------------------------------
 * Recording fakes for alp_i2c_write() / alp_i2c_write_read() /
 * alp_delay_us() / alp_delay_ms(). `alp_i2c_t` is opaque (`struct alp_i2c`
 * is never defined in the public header), so `ctx.bus` below just needs to
 * be a non-NULL pointer these fakes never dereference -- it is not linked
 * against any real backend.
 * ------------------------------------------------------------------------ */

#define REC_MAX_CALLS 8
#define REC_MAX_LEN   72 /* >= 2 + EEPROM_24C128_SECURE_PAGE_BYTES */

typedef struct {
	uint8_t  addr;
	uint8_t  data[REC_MAX_LEN];
	size_t   len;
	uint32_t seq; /* this call's position in g_seq's global order */
} rec_call_t;

/* Monotonic counter stamped into every recorded event (a write, a
 * write_read, or an alp_delay_ms call) -- the only way to check ORDER
 * across the three, since g_writes[]/g_write_reads[] are otherwise
 * independent arrays. 0 is never a real seq value (rec_reset() sets the
 * counter to 0 and the first ++g_seq lands on 1), so a delay record whose
 * seq is still 0 means alp_delay_ms was never called. */
static uint32_t g_seq;

static rec_call_t   g_writes[REC_MAX_CALLS];
static int          g_write_count;
static rec_call_t   g_write_reads[REC_MAX_CALLS]; /* records only the WRITE half */
static int          g_write_read_count;
static uint32_t     g_delay_ms_total; /* sum of every alp_delay_ms() argument */
static uint32_t     g_delay_seq;      /* seq of the LAST alp_delay_ms call; 0 = never called */
static uint8_t      g_next_read_byte;
static alp_status_t g_next_write_status;
static alp_status_t g_next_write_read_status;

static void rec_reset(void)
{
	g_seq                    = 0;
	g_write_count            = 0;
	g_write_read_count       = 0;
	g_delay_ms_total         = 0;
	g_delay_seq              = 0;
	g_next_read_byte         = 0;
	g_next_write_status      = ALP_OK;
	g_next_write_read_status = ALP_OK;
}

alp_status_t alp_i2c_write(alp_i2c_t *bus, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)bus;
	if (g_write_count < REC_MAX_CALLS && len <= REC_MAX_LEN) {
		g_writes[g_write_count].addr = addr;
		memcpy(g_writes[g_write_count].data, data, len);
		g_writes[g_write_count].len = len;
		g_writes[g_write_count].seq = ++g_seq;
		g_write_count++;
	}
	return g_next_write_status;
}

alp_status_t alp_i2c_write_read(alp_i2c_t     *bus,
                                uint8_t        addr,
                                const uint8_t *wdata,
                                size_t         wlen,
                                uint8_t       *rdata,
                                size_t         rlen)
{
	(void)bus;
	if (g_write_read_count < REC_MAX_CALLS && wlen <= REC_MAX_LEN) {
		g_write_reads[g_write_read_count].addr = addr;
		memcpy(g_write_reads[g_write_read_count].data, wdata, wlen);
		g_write_reads[g_write_read_count].len = wlen;
		g_write_reads[g_write_read_count].seq = ++g_seq;
		g_write_read_count++;
	}
	if (rdata != NULL && rlen > 0) {
		memset(rdata, g_next_read_byte, rlen);
	}
	return g_next_write_read_status;
}

void alp_delay_us(uint32_t us)
{
	(void)us;
	++g_seq; /* keeps ordering consistent even though no test asserts on this one */
}

/* eeprom_24c128_secure_page_lock() waits via alp_delay_ms(), not
 * alp_delay_us() -- a fixed ~20 ms wait belongs on the
 * yielding primitive, not the non-yielding busy-wait alp_delay_us() is
 * documented for. This is the double for that wait; an
 * earlier revision of this file recorded nothing here at all, so
 * deleting the wait from the driver still passed every assertion. */
void alp_delay_ms(uint32_t ms)
{
	g_delay_ms_total += ms;
	g_delay_seq = ++g_seq;
}

/* Initialised ctx via the real eeprom_24c128_init() (so its own probe read
 * is exercised too), then drops that probe call from the recording so
 * every test below starts from a clean slate. */
static eeprom_24c128_t make_ctx(void)
{
	rec_reset();
	static int      dummy_bus;
	eeprom_24c128_t ctx;
	alp_status_t s = eeprom_24c128_init(&ctx, (alp_i2c_t *)&dummy_bus, EEPROM_24C128_I2C_ADDR_LOW);
	ALP_ASSERT_EQ_INT(s, ALP_OK);
	rec_reset();
	return ctx;
}

static uint8_t alt_addr(void)
{
	return (uint8_t)(EEPROM_24C128_I2C_ADDR_LOW + EEPROM_24C128_ALT_ADDR_OFFSET);
}

static eeprom_24c128_t z_make_ctx(uint8_t addr, bool initialised)
{
	eeprom_24c128_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.initialised = initialised;
	ctx.bus         = NULL; /* Never dereferenced when ctx == NULL / !initialised
                              * short-circuits before the bus is ever touched. */
	ctx.addr        = addr;
	return ctx;
}

/* ---- eeprom_24c128_secure_page_write: argument/state contract ---- */

static void test_write_null_ctx(void)
{
	uint8_t page[EEPROM_24C128_SECURE_PAGE_BYTES];
	memset(page, 0xAB, sizeof(page));
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_write(NULL, page, sizeof(page)), ALP_ERR_NOT_READY);
}

static void test_write_null_data(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, true);
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_write(&ctx, NULL, EEPROM_24C128_SECURE_PAGE_BYTES),
	                  ALP_ERR_INVAL);
}

static void test_write_uninitialised_ctx(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, false);
	uint8_t         page[EEPROM_24C128_SECURE_PAGE_BYTES];
	memset(page, 0xAB, sizeof(page));
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_write(&ctx, page, sizeof(page)), ALP_ERR_NOT_READY);
}

/* A short OR long buffer must be rejected before it ever reaches the bus --
 * the length check is what makes the header's documented safety property
 * (this function can never memcpy past a caller-supplied buffer) actually
 * true, since a bare `const uint8_t *` parameter carries no size on its
 * own. */
static void test_write_wrong_len_rejected(void)
{
	eeprom_24c128_t ctx = make_ctx();
	uint8_t         page[EEPROM_24C128_SECURE_PAGE_BYTES];
	memset(page, 0xAB, sizeof(page));

	ALP_ASSERT_EQ_INT(
	    eeprom_24c128_secure_page_write(&ctx, page, EEPROM_24C128_SECURE_PAGE_BYTES - 1),
	    ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(
	    eeprom_24c128_secure_page_write(&ctx, page, EEPROM_24C128_SECURE_PAGE_BYTES + 1),
	    ALP_ERR_INVAL);
	/* Neither rejected call reached the bus. */
	ALP_ASSERT_EQ_INT(g_write_count, 0);
}

/* ---- eeprom_24c128_secure_page_write: the exact wire frame ----
 *
 * This is the check that actually matters: a 2-byte pointer {selector,
 * in-page offset} = {0x00, 0x00}, THEN exactly EEPROM_24C128_SECURE_PAGE_BYTES
 * data bytes -- the SAME 2-byte pointer convention
 * eeprom_24c128_read_identity() uses (bench-verified), not the 3-byte
 * "selector + op-code + offset" frame an earlier revision emitted (which
 * rotated every byte of the page by one on write). */
static void test_write_frame_is_exact(void)
{
	eeprom_24c128_t ctx = make_ctx();
	uint8_t         payload[EEPROM_24C128_SECURE_PAGE_BYTES];
	for (size_t i = 0; i < sizeof(payload); ++i)
		payload[i] = (uint8_t)(0xA0u + i);

	alp_status_t s = eeprom_24c128_secure_page_write(&ctx, payload, sizeof(payload));
	ALP_ASSERT_EQ_INT(s, ALP_OK);

	ALP_ASSERT_TRUE(g_write_count >= 1);
	ALP_ASSERT_EQ_INT(g_writes[0].addr, alt_addr());
	ALP_ASSERT_EQ_INT(g_writes[0].len, 2 + EEPROM_24C128_SECURE_PAGE_BYTES);
	ALP_ASSERT_EQ_INT(g_writes[0].data[0], 0x00); /* Secure Data Page selector, FIRST byte */
	ALP_ASSERT_EQ_INT(g_writes[0].data[1], 0x00); /* in-page offset */
	ALP_ASSERT_EQ_INT(memcmp(g_writes[0].data + 2, payload, sizeof(payload)), 0);

	/* The post-write ACK poll (eeprom_24c128_write's own path, unaffected by
     * this review) is a SEPARATE, second alp_i2c_write call, 2-byte
     * address-only -- it must not be mistaken for a second copy of the page. */
	ALP_ASSERT_EQ_INT(g_write_count, 2);
	ALP_ASSERT_EQ_INT(g_writes[1].len, 2);
}

/* ---- eeprom_24c128_secure_page_lock: the exact wire frame + no page poll ----
 *
 * {0x04, 0x00, 0xFF} -- Lock Status selector in the FIRST pointer byte
 * (matching eeprom_24c128_read_identity()'s convention for the SAME
 * selector), not the second (a selector in the second byte turns
 * this into a plain data write at Secure-Data-Page offset 4, clobbering
 * schema_version + sku[0]). And exactly ONE alp_i2c_write call total: no
 * address-only poll against the page this call just locked -- that
 * poll is itself a write to the Secure Data Page, which a CORRECT lock
 * always NAKs, so it would report success as ALP_ERR_TIMEOUT.
 *
 * Also asserts the two properties review found missing from an earlier
 * revision of this test: the post-lock wait actually
 * happens (deleting it from the driver drops g_delay_seq back to 0 and
 * fails the `g_delay_seq != 0` assertion below), and the three events
 * happen in the right ORDER -- lock write, then the wait, then the
 * confirming read (reordering the confirm read ahead of the write, which
 * would misreport every real first-time lock as ALP_ERR_IO by reading the
 * PRE-lock state, fails the seq-ordering assertion below even though the
 * frame content and call counts stay identical). */
static void test_lock_frame_is_exact_and_never_polls_the_page(void)
{
	eeprom_24c128_t ctx = make_ctx();
	g_next_read_byte    = 0x02u; /* Lock Status confirm read: bit 1 set = locked */

	alp_status_t s = eeprom_24c128_secure_page_lock(&ctx);
	ALP_ASSERT_EQ_INT(s, ALP_OK);

	ALP_ASSERT_EQ_INT(g_write_count, 1); /* the lock command, and NOTHING else */
	ALP_ASSERT_EQ_INT(g_writes[0].addr, alt_addr());
	ALP_ASSERT_EQ_INT(g_writes[0].len, 3);
	ALP_ASSERT_EQ_INT(g_writes[0].data[0], 0x04); /* Lock Status selector, FIRST byte */
	ALP_ASSERT_EQ_INT(g_writes[0].data[1], 0x00);
	ALP_ASSERT_EQ_INT(g_writes[0].data[2], 0xFFu); /* the lock trigger byte */

	/* Exactly one confirming read: Lock Status, {sel, 0x00} pointer, 1 byte
     * out. This is the ONLY state check the datasheet describes as safe. */
	ALP_ASSERT_EQ_INT(g_write_read_count, 1);
	ALP_ASSERT_EQ_INT(g_write_reads[0].addr, alt_addr());
	ALP_ASSERT_EQ_INT(g_write_reads[0].len, 2);
	ALP_ASSERT_EQ_INT(g_write_reads[0].data[0], 0x04);
	ALP_ASSERT_EQ_INT(g_write_reads[0].data[1], 0x00);

	/* The wait that replaced the removed ACK poll actually ran, and
     * waited at least as long as the driver's own post-lock wait budget
     * (EEPROM_LOCK_WAIT_MS = 20 ms; private to eeprom_24c128.c, so this is a
     * literal cross-check against the driver's documented budget, not an
     * include. This used to be computed inline as
     * (EEPROM_WRITE_POLL_STEP_US / 1000) * EEPROM_WRITE_POLL_MAX, integer
     * division that rounds to 0 -- silently deleting the wait -- for any
     * EEPROM_WRITE_POLL_STEP_US below 1000; it is now its own named,
     * non-derived constant). */
	ALP_ASSERT_TRUE(g_delay_seq != 0);
	ALP_ASSERT_TRUE(g_delay_ms_total >= 20u);

	/* ORDER, not just presence/count -- the lock write happens
     * before the wait, which happens before the confirming read. */
	ALP_ASSERT_TRUE(g_writes[0].seq < g_delay_seq);
	ALP_ASSERT_TRUE(g_delay_seq < g_write_reads[0].seq);
}

static void test_lock_reports_io_when_confirm_reads_unlocked(void)
{
	eeprom_24c128_t ctx = make_ctx();
	g_next_read_byte    = 0x00u; /* bit 1 clear -- the lock did not take */

	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_lock(&ctx), ALP_ERR_IO);
}

/* ---- eeprom_24c128_secure_page_lock: argument/state contract ---- */

static void test_lock_null_ctx(void)
{
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_lock(NULL), ALP_ERR_NOT_READY);
}

static void test_lock_uninitialised_ctx(void)
{
	eeprom_24c128_t ctx = z_make_ctx(EEPROM_24C128_I2C_ADDR_LOW, false);
	ALP_ASSERT_EQ_INT(eeprom_24c128_secure_page_lock(&ctx), ALP_ERR_NOT_READY);
}

/* A NAK on the lock command itself (as opposed to the confirm read) must
 * propagate, not be swallowed as ALP_OK -- this is the PERMANENT,
 * IRREVERSIBLE call. */
static void test_lock_write_failure_propagates(void)
{
	eeprom_24c128_t ctx = make_ctx();
	g_next_write_status = ALP_ERR_NOT_READY;
	alp_status_t rc     = eeprom_24c128_secure_page_lock(&ctx);
	ALP_ASSERT_TRUE(rc != ALP_OK);
}

int main(void)
{
	test_write_null_ctx();
	test_write_null_data();
	test_write_uninitialised_ctx();
	test_write_wrong_len_rejected();
	test_write_frame_is_exact();

	test_lock_frame_is_exact_and_never_polls_the_page();
	test_lock_reports_io_when_confirm_reads_unlocked();
	test_lock_null_ctx();
	test_lock_uninitialised_ctx();
	test_lock_write_failure_propagates();

	ALP_TEST_SUMMARY();
}
