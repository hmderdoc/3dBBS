#ifdef ENABLE_SSH

#include <3ds.h>
#include <string.h>
#include <libssh2.h>
#include "ssh.h"

// Terminal type for the pty request — same string the rlogin handshake
// sends, so Synchronet keys CP437 + truecolor off it identically.
#define SSH_TERM_NAME "ansi-bbs-cp437-truecolor"

// One session at a time, matching the connection layer. libssh2 sessions
// are not thread-safe and telnetSend runs on the APC worker as well as the
// main loop, so every libssh2 call below is serialized behind this lock.
static LightLock sshLock = 1;
static LIBSSH2_SESSION* session;
static LIBSSH2_CHANNEL* channel;
static bool libInited;

// The session runs non-blocking from the very start: libssh2's blocking
// mode waits with select(), which this soc stack does not implement
// reliably (measured on connect-completion). EAGAIN-polling with a sleep
// is the one waiting primitive that demonstrably works here.
#define HANDSHAKE_TIMEOUT_MS 20000

#define WAIT_EAGAIN(expr, rcvar)                                  \
	do {                                                          \
		int waited_ = 0;                                          \
		while ((rcvar = (expr)) == LIBSSH2_ERROR_EAGAIN) {        \
			if (waited_ >= HANDSHAKE_TIMEOUT_MS)                  \
				break;                                            \
			svcSleepThread(10 * 1000000LL);                       \
			waited_ += 10;                                        \
		}                                                         \
	} while (0)

bool sshStart(int fd, const char* user, const char* pass,
              u16 cols, u16 rows)
{
	int rc;

	if (!libInited) {
		if (libssh2_init(0) != 0)
			return false;
		libInited = true;
	}

	sshClose();

	LightLock_Lock(&sshLock);
	session = libssh2_session_init();
	if (!session) {
		LightLock_Unlock(&sshLock);
		return false;
	}
	libssh2_session_set_blocking(session, 0);

	WAIT_EAGAIN(libssh2_session_handshake(session, fd), rc);
	if (rc)
		goto fail;

	// v1: no host-key verification (see ssh.h). Fingerprint is available
	// here via libssh2_hostkey_hash when TOFU storage lands.

	WAIT_EAGAIN(libssh2_userauth_password(session,
	                                      user ? user : "",
	                                      pass ? pass : ""), rc);
	if (rc)
		goto fail;

	do {
		channel = libssh2_channel_open_session(session);
		if (channel)
			break;
		rc = libssh2_session_last_errno(session);
		svcSleepThread(10 * 1000000LL);
	} while (rc == LIBSSH2_ERROR_EAGAIN);
	if (!channel)
		goto fail;

	WAIT_EAGAIN(libssh2_channel_request_pty_ex(channel,
	                SSH_TERM_NAME, sizeof(SSH_TERM_NAME) - 1,
	                NULL, 0, cols, rows, 0, 0), rc);
	if (rc)
		goto failchan;

	WAIT_EAGAIN(libssh2_channel_shell(channel), rc);
	if (rc)
		goto failchan;

	LightLock_Unlock(&sshLock);
	return true;

failchan:
	libssh2_channel_free(channel);
	channel = NULL;
fail:
	libssh2_session_free(session);
	session = NULL;
	LightLock_Unlock(&sshLock);
	return false;
}

int sshRecv(u8* out, int cap)
{
	if (!channel)
		return -1;
	LightLock_Lock(&sshLock);
	ssize_t n = libssh2_channel_read(channel, (char*)out, cap);
	bool closed = (n == 0 || n == LIBSSH2_ERROR_CHANNEL_CLOSED ||
	               n == LIBSSH2_ERROR_SOCKET_RECV) &&
	              libssh2_channel_eof(channel);
	LightLock_Unlock(&sshLock);
	if (n > 0)
		return (int)n;
	if (closed || (n < 0 && n != LIBSSH2_ERROR_EAGAIN))
		return -1;
	return 0;
}

void sshSend(const u8* data, int len)
{
	if (!channel)
		return;
	int off = 0;
	while (off < len) {
		LightLock_Lock(&sshLock);
		ssize_t n = libssh2_channel_write(channel, (const char*)data + off,
		                                  len - off);
		LightLock_Unlock(&sshLock);
		if (n == LIBSSH2_ERROR_EAGAIN) {
			svcSleepThread(2 * 1000000LL);
			continue;
		}
		if (n < 0)
			return;
		off += (int)n;
	}
}

void sshResize(u16 cols, u16 rows)
{
	if (!channel)
		return;
	LightLock_Lock(&sshLock);
	// Fire-and-forget; EAGAIN here just means the next resize wins
	libssh2_channel_request_pty_size(channel, cols, rows);
	LightLock_Unlock(&sshLock);
}

void sshClose(void)
{
	LightLock_Lock(&sshLock);
	if (channel) {
		libssh2_channel_free(channel);
		channel = NULL;
	}
	if (session) {
		libssh2_session_disconnect(session, "bye");
		libssh2_session_free(session);
		session = NULL;
	}
	LightLock_Unlock(&sshLock);
}

#endif /* ENABLE_SSH */
