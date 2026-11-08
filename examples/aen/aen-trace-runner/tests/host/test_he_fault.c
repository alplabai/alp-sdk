/* tests/host/test_he_fault.c -- the HE's crash record (src/ipc/tr_he_fault.h): the console tail ring
 * keeps the last TR_HE_TAIL characters in order across a wrap, a fatal error is read back once with
 * its fields, cold SRAM reads as nothing, and taking the record re-arms it. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_he_fault.h"

static tr_he_fault_t      rec;
static tr_he_fault_snap_t snap;

static void puts_tail(const char *s)
{
	while (*s) {
		tr_he_fault_tail_put(&rec, *s++);
	}
}

int main(void)
{
	/* Cold SRAM: garbage, magics not ours -> nothing, and the take arms the ring. */
	memset((void *)&rec, 0xA5, sizeof(rec));
	assert(!tr_he_fault_take(&rec, &snap));
	assert(snap.tail_len == 0u && snap.tail[0] == 0);
	assert(rec.tail_magic == TR_HE_TAIL_MAGIC && rec.tail_pos == 0u && rec.magic == 0u);

	/* A short console: read back whole, in order, NUL-terminated. */
	puts_tail("m55 boot #1\n");
	assert(!tr_he_fault_take(&rec, &snap));
	assert(snap.tail_len == 12u && strcmp(snap.tail, "m55 boot #1\n") == 0);
	/* Taking re-armed the ring: a second take sees nothing. */
	assert(!tr_he_fault_take(&rec, &snap) && snap.tail_len == 0u);

	/* More than the ring holds: the LAST TR_HE_TAIL characters, oldest first. */
	for (unsigned i = 0; i < TR_HE_TAIL + 100u; i++) {
		tr_he_fault_tail_put(&rec, (char)('a' + i % 26u));
	}
	assert(!tr_he_fault_take(&rec, &snap));
	assert(snap.tail_len == TR_HE_TAIL);
	for (unsigned i = 0; i < TR_HE_TAIL; i++) {
		unsigned n = 100u + i; /* index into the written sequence */

		assert(snap.tail[i] == (char)('a' + n % 26u));
	}
	assert(snap.tail[TR_HE_TAIL] == 0);

	/* Exactly full, no wrap yet. */
	for (unsigned i = 0; i < TR_HE_TAIL; i++) {
		tr_he_fault_tail_put(&rec, (char)('A' + i % 26u));
	}
	assert(!tr_he_fault_take(&rec, &snap) && snap.tail_len == TR_HE_TAIL && snap.tail[0] == 'A');

	/* A fatal error: the fields come back, the tail with it, once. */
	puts_tail("E: ***** BUS FAULT *****\n");
	tr_he_fault_record(
	    &rec, 3u, 0x58012344u, 0x58012301u, 0x00008200u, 0x40000000u, 0x0u, 0x4A011000u, 123456u);
	assert(tr_he_fault_take(&rec, &snap));
	assert(snap.fault && snap.reason == 3u && snap.pc == 0x58012344u && snap.lr == 0x58012301u);
	assert(snap.cfsr == 0x00008200u && snap.hfsr == 0x40000000u && snap.mmfar == 0u);
	assert(snap.bfar == 0x4A011000u && snap.uptime_ms == 123456u);
	assert(strcmp(snap.tail, "E: ***** BUS FAULT *****\n") == 0);
	assert(!tr_he_fault_take(&rec, &snap) && !snap.fault);

	/* The record is a fixed 0x18C-byte block in the shared page. */
	assert(sizeof(tr_he_fault_t) == 0x18Cu);

	printf("test_he_fault: ok\n");
	return 0;
}
