#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "settings.h"

#define CFG_DIR  "sdmc:/3dBBS"
#define CFG_PATH CFG_DIR "/settings.txt"

static const char* defaultFile =
	"# 3dBBS settings\n"
	"#\n"
	"# lid_keepalive_min: minutes to keep a live connection alive after the\n"
	"#   lid is closed. The console stays awake for that long (it does NOT\n"
	"#   sleep), so it costs battery; after the window expires the system\n"
	"#   sleeps normally. 0 disables the hold.\n"
	"lid_keepalive_min=5\n"
	"#\n"
	"# led: drive the RGB notification LED from receive throughput — a hue\n"
	"#   sweep that speeds up and brightens with the data stream.\n"
	"#   1 = on, 0 = leave the LED to the system.\n"
	"led=1\n";

static Settings cfg = { 5, true };

static void apply(const char* key, const char* val)
{
	if (!strcmp(key, "lid_keepalive_min")) {
		int m = atoi(val);
		// An unbounded hold would flatten the battery in a bag; a full
		// hour is already far past any plausible "finish this download"
		cfg.lidKeepaliveMin = (m < 0) ? 0 : (m > 60 ? 60 : m);
	} else if (!strcmp(key, "led") || !strcmp(key, "wifi_led")) {
		// wifi_led is the name this setting shipped under for one build,
		// when it drove the Wi-Fi lamp instead of the RGB LED.
		cfg.led = (atoi(val) != 0);
	}
}

void settingsLoad(void)
{
	FILE* f = fopen(CFG_PATH, "r");
	if (!f) {
		mkdir(CFG_DIR, 0777);
		f = fopen(CFG_PATH, "w");
		if (f) {
			fputs(defaultFile, f);
			fclose(f);
		}
		return;   // freshly written file == the compiled-in defaults
	}

	char line[128];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || !line[0])
			continue;
		char* eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		apply(line, eq + 1);
	}
	fclose(f);
}

const Settings* settingsGet(void) { return &cfg; }
