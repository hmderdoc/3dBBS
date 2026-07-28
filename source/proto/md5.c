// MD5 per RFC 1321; compact single-block implementation.
#include <string.h>
#include "md5.h"

static const u32 K[64] = {
	0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
	0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
	0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
	0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
	0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
	0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
	0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
	0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
	0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
	0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
	0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
};
static const u8 S[64] = {
	7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
	5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
	4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
	6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21,
};

static u32 rotl(u32 x, int c) { return (x << c) | (x >> (32 - c)); }

static void block(Md5Ctx* c, const u8* p)
{
	u32 m[16];
	for (int i = 0; i < 16; i++)
		m[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((u32)p[i*4+3] << 24);

	u32 a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
	for (int i = 0; i < 64; i++) {
		u32 f;
		int g;
		if (i < 16)      { f = (b & cc) | (~b & d);        g = i; }
		else if (i < 32) { f = (d & b) | (~d & cc);        g = (5*i + 1) & 15; }
		else if (i < 48) { f = b ^ cc ^ d;                 g = (3*i + 5) & 15; }
		else             { f = cc ^ (b | ~d);              g = (7*i) & 15; }
		u32 tmp = d;
		d = cc;
		cc = b;
		b = b + rotl(a + f + K[i] + m[g], S[i]);
		a = tmp;
	}
	c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
}

void md5Init(Md5Ctx* c)
{
	c->state[0] = 0x67452301; c->state[1] = 0xefcdab89;
	c->state[2] = 0x98badcfe; c->state[3] = 0x10325476;
	c->count = 0;
}

void md5Update(Md5Ctx* c, const u8* data, size_t len)
{
	size_t have = c->count & 63;
	c->count += len;
	if (have) {
		size_t need = 64 - have;
		if (len < need) {
			memcpy(c->buf + have, data, len);
			return;
		}
		memcpy(c->buf + have, data, need);
		block(c, c->buf);
		data += need;
		len -= need;
	}
	while (len >= 64) {
		block(c, data);
		data += 64;
		len -= 64;
	}
	memcpy(c->buf, data, len);
}

void md5Final(Md5Ctx* c, u8 digest[16])
{
	u64 bits = c->count * 8;
	u8 pad = 0x80;
	md5Update(c, &pad, 1);
	static const u8 zero[64] = {0};
	while ((c->count & 63) != 56)
		md5Update(c, zero, 1);
	u8 lenb[8];
	for (int i = 0; i < 8; i++)
		lenb[i] = (bits >> (8 * i)) & 0xFF;
	md5Update(c, lenb, 8);
	for (int i = 0; i < 4; i++) {
		digest[i*4]   = c->state[i] & 0xFF;
		digest[i*4+1] = (c->state[i] >> 8) & 0xFF;
		digest[i*4+2] = (c->state[i] >> 16) & 0xFF;
		digest[i*4+3] = (c->state[i] >> 24) & 0xFF;
	}
}
