/* tests/host/test_fault_inject.c -- the bench fault injector (src/ipc/tr_fault_inject.h): nothing
 * unless the magic is written, then due once the uptime passes the delay and on every later check. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/ipc/tr_fault_inject.h"

int main(void)
{
	static tr_fault_inject_t f;
	static const uint8_t     fill[] = { 0x00, 0xFF, 0xA5, 0x5A };

	for (unsigned i = 0; i < sizeof(fill); i++) { /* cold SRAM never fires */
		memset((void *)&f, fill[i], sizeof(f));
		assert(!tr_fault_inject_due(&f, 0xFFFFFFFFu));
	}
	f.delay_ms = 5000u;
	f.magic    = TR_FAULT_INJECT_MAGIC;
	assert(!tr_fault_inject_due(&f, 0u));
	assert(!tr_fault_inject_due(&f, 4999u));
	assert(tr_fault_inject_due(&f, 5000u));
	assert(tr_fault_inject_due(&f, 100000u)); /* stays armed: the fault repeats every boot */
	f.delay_ms = 0u;
	assert(tr_fault_inject_due(&f, 0u));
	f.magic = 0u; /* disarmed by the bench */
	assert(!tr_fault_inject_due(&f, 1u));
	puts("test_fault_inject: OK");
	return 0;
}
