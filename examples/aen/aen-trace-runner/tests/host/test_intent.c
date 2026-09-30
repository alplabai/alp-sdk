/* tests/host/test_intent.c */
#include <assert.h>
#include "../../src/game/intent.h"

int main(void)
{
	tr_intent_t i = tr_intent_none();

	assert(i.lane_delta == 0);
	assert(!i.jump);
	assert(!i.duck);
	assert(i.source == TR_INPUT_NONE);
	return 0;
}
