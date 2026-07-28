#ifndef B64_H
#define B64_H

#include <3ds/types.h>

// Decode base64 (skips whitespace and '='). Returns bytes written.
int b64decode(const char* in, int inLen, u8* out, int outCap);

#endif
