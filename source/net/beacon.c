#include <3ds.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "beacon.h"

static int fd = -1;
static struct sockaddr_in dest;

void beaconInit(const char* host, int port)
{
#ifdef RELEASE_BUILD
	// Release builds carry no telemetry: fd stays -1, Send/Exit no-op.
	(void)host; (void)port;
	return;
#endif
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(port);
	dest.sin_addr.s_addr = inet_addr(host);
}

void beaconSend(const char* line)
{
	if (fd >= 0)
		sendto(fd, line, strlen(line), 0, (struct sockaddr*)&dest,
		       sizeof(dest));
}

void beaconExit(void)
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}
