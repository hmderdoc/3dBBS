#include <3ds.h>
#include <malloc.h>
#include "sock.h"

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

static u32* socBuffer;

bool netInit(void)
{
	socBuffer = memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!socBuffer)
		return false;
	if (R_FAILED(socInit(socBuffer, SOC_BUFFERSIZE))) {
		free(socBuffer);
		socBuffer = NULL;
		return false;
	}
	return true;
}

void netExit(void)
{
	if (socBuffer) {
		socExit();
		free(socBuffer);
		socBuffer = NULL;
	}
}
