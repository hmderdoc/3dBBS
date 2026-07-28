#ifndef SOCK_H
#define SOCK_H

#include <stdbool.h>

// 3DS soc:u service init/teardown (required before any BSD socket use)
bool netInit(void);
void netExit(void);

#endif
