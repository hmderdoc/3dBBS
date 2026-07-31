#include <3ds.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "shot.h"

#ifndef RELEASE_BUILD

// "3DSHOT03", u16 width, u16 height, u8 bytesPerPixel, u8 viewCount,
// then viewCount raw framebuffers, swept from one extreme to the other.
#define SHOT_MAGIC "3DSHOT03"

static int fd = -1;

static bool sendAll(const void* data, u32 len)
{
	const u8* p = (const u8*)data;
	while (len) {
		// Even a blocking socket returns short writes on payloads this size.
		int n = send(fd, p, len > 8192 ? 8192 : (int)len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (u32)n;
	}
	return true;
}

bool shotOpen(const char* host, int port, u16 w, u16 h, u8 views)
{
	shotClose();
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;

	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(port);
	dest.sin_addr.s_addr = inet_addr(host);
	if (connect(fd, (struct sockaddr*)&dest, sizeof(dest)) != 0) {
		shotClose();
		return false;
	}

	u8 hdr[14];
	memcpy(hdr, SHOT_MAGIC, 8);
	hdr[8]  = (u8)(w & 0xFF);
	hdr[9]  = (u8)(w >> 8);
	hdr[10] = (u8)(h & 0xFF);
	hdr[11] = (u8)(h >> 8);
	hdr[12] = 3;        // bytes per pixel
	hdr[13] = views;
	if (!sendAll(hdr, sizeof(hdr))) {
		shotClose();
		return false;
	}
	return true;
}

bool shotFrame(const u8* fb, u32 bytes)
{
	if (fd < 0 || !fb)
		return false;
	return sendAll(fb, bytes);
}

void shotClose(void)
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

#else

bool shotOpen(const char* host, int port, u16 w, u16 h, u8 views)
{
	(void)host; (void)port; (void)w; (void)h; (void)views;
	return false;
}
bool shotFrame(const u8* fb, u32 bytes) { (void)fb; (void)bytes; return false; }
void shotClose(void) {}

#endif
