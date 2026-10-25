/* Forced into private Wine server copies only, after libc declarations. */
#include "config.h"
#include <stdlib.h>
#include <unistd.h>
_Noreturn void uwph_host_exit(int);
int uwph_host_atexit(void (*)(void));
void uwph_host_ready(void);
#define exit uwph_host_exit
#define _exit uwph_host_exit
#define atexit uwph_host_atexit
