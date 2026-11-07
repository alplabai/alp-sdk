#include "lib.h"
int keep(int x){return x+1;}
long sig(long x,int y){return x*y;}
int use_cfg(struct cfg *c){return c->a+c->b+(int)c->c;}
long g_mode=2;
