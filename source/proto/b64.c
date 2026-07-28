#include "b64.h"

static int b64val(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

int b64decode(const char* in, int inLen, u8* out, int outCap)
{
	int acc = 0, bits = 0, n = 0;
	for (int i = 0; i < inLen; i++) {
		int v = b64val(in[i]);
		if (v < 0)
			continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n < outCap)
				out[n++] = (acc >> bits) & 0xFF;
		}
	}
	return n;
}
