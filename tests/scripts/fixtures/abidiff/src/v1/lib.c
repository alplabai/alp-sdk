#include "lib.h"
int keep(int x){return x+1;}
int gone(int x){return x-1;}
int sig(int x){return x*2;}
int use_cfg(struct cfg *c){return c->a+c->b;}
int g_level=1; int g_mode=2;
