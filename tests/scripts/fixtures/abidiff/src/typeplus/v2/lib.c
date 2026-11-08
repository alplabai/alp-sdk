#include "lib.h"
int take(struct cfg *c, int extra)
{
	return c->a + c->b + (int)c->c + extra;
}
