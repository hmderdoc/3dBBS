#ifndef TELNET_H
#define TELNET_H

#include <3ds/types.h>
#include <stdbool.h>

// Minimal telnet client (RFC 854 NVT + option negotiation + NAWS).
// Single connection; grows SSH/etc. behind the same interface later.

// Geometry reported via NAWS (RFC 1073) when the server asks. 80x25 is the
// floor per DESIGN.md; anything larger is fair game once the renderer is in.
void telnetSetSize(u16 cols, u16 rows);

// Call after a runtime resize: updates the stored size and, if the server
// negotiated NAWS, pushes the new geometry immediately.
void telnetNotifySize(u16 cols, u16 rows);

// Blocking connect (resolves via getaddrinfo), then switches the socket to
// non-blocking for pump/send. Returns false on failure.
bool telnetConnect(const char* host, u16 port);
bool telnetIsConnected(void);
void telnetClose(void);

// Call once per frame: drains the socket into an internal 256KB ring buffer
// (cheap — keeps the TCP window open regardless of how busy rendering or
// parsing is, so high-bandwidth senders never stall on us).
void telnetDrain(void);

// Pull clean application data out of the ring (IAC negotiation handled
// in-stream). Returns bytes written (0 = ring empty), or -1 once the
// connection has dropped AND the ring is fully consumed. Call repeatedly
// within a frame-time budget.
int telnetRead(u8* out, int cap);

// Send application data (IAC bytes are escaped as required).
// Thread-safe: callable from the main loop and the APC worker.
void telnetSend(const u8* data, int len);

// Perf counters: current ring backlog and cumulative received bytes
void telnetStats(int* ringBytes, u32* totalRxBytes);

#endif
