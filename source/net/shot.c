#include <3ds.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "shot.h"

#ifndef RELEASE_BUILD

// "3DSHOT02", then: u16 width, u16 height, u8 bytesPerPixel, u8 eyeCount,
// followed by eyeCount raw framebuffers back to back.
#define SHOT_MAGIC "3DSHOT02"

static bool sendAll(int fd, const void* data, u32 len)
{
	const u8* p = (const u8*)data;
	while (len) {
		// A blocking socket can still return a short write; ~576KB of
		// framebuffer will not go out in one call.
		int n = send(fd, p, len > 8192 ? 8192 : (int)len, 0);
		if (n <= 0)
			return false;
		p += n;
		len -= (u32)n;
	}
	return true;
}

void shotSend(const char* host, int port)
{
	u16 wl = 0, hl = 0, wr = 0, hr = 0;
	u8* left = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &wl, &hl);
	u8* right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &wr, &hr);
	if (!left)
		return;

	// If the right eye was never drawn this frame it aliases the left, and
	// sending it would produce a "stereo" pair with no depth. Send one eye
	// and let the host say so rather than shipping a silent dud.
	u8 eyes = (right && right != left && wr == wl && hr == hl) ? 2 : 1;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(port);
	dest.sin_addr.s_addr = inet_addr(host);

	if (connect(fd, (struct sockaddr*)&dest, sizeof(dest)) == 0) {
		u8 hdr[12];
		memcpy(hdr, SHOT_MAGIC, 8);
		hdr[8]  = (u8)(wl & 0xFF);
		hdr[9]  = (u8)(wl >> 8);
		hdr[10] = (u8)(hl & 0xFF);
		hdr[11] = (u8)(hl >> 8);
		u8 tail[2] = { 3, eyes };   // bytes per pixel, eye count

		u32 bytes = (u32)wl * hl * 3;
		if (sendAll(fd, hdr, sizeof(hdr)) && sendAll(fd, tail, sizeof(tail)) &&
		    sendAll(fd, left, bytes) && eyes == 2)
			sendAll(fd, right, bytes);
	}
	close(fd);
}

#else

void shotSend(const char* host, int port) { (void)host; (void)port; }

#endif
