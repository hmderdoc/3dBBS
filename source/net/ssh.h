#ifndef SSH_H
#define SSH_H

#include <3ds/types.h>
#include <stdbool.h>

// SSH2 transport over an already-connected TCP socket (libssh2, mbedtls
// backend). Compiled only when ENABLE_SSH is defined — the connection
// layer falls back to a clean failure without it.
//
// Host keys are accepted without verification in v1 (TOFU storage is a
// TODO); the transport still gives privacy on the wire and, more to the
// point here, boards that only listen on SSH become reachable.

// Blocking handshake + password auth + shell channel on fd (which must
// already be O_NONBLOCK; all waiting is EAGAIN-polling, never select()).
// Returns false on any failure — caller still owns/closes fd.
bool sshStart(int fd, const char* user, const char* pass,
              u16 cols, u16 rows);

// Read decrypted channel bytes. >0 = bytes, 0 = nothing pending,
// -1 = channel/session closed.
int sshRecv(u8* out, int cap);

// Write through the channel (loops internally on EAGAIN).
void sshSend(const u8* data, int len);

// Propagate a terminal resize (SSH window-change request).
void sshResize(u16 cols, u16 rows);

// Tear down channel + session (does not close the socket fd).
void sshClose(void);

#endif
