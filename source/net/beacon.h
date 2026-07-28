#ifndef BEACON_H
#define BEACON_H

// Dev telemetry: fire-and-forget UDP stats to the development Mac so client
// internals are observable without squinting at the overlay. Harmless when
// no collector is listening; strip for release builds.

void beaconInit(const char* host, int port);
void beaconSend(const char* line);
void beaconExit(void);

#endif
