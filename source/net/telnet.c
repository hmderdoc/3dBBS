#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include "telnet.h"

// Telnet protocol bytes
#define T_IAC  255
#define T_DONT 254
#define T_DO   253
#define T_WONT 252
#define T_WILL 251
#define T_SB   250
#define T_SE   240

// Options
#define OPT_ECHO  1
#define OPT_SGA   3
#define OPT_TTYPE 24
#define OPT_NAWS  31

// TTYPE subnegotiation
#define TTYPE_IS   0
#define TTYPE_SEND 1

// Terminal name reported via telnet TTYPE — matches SyncTerm's ANSI-BBS
// default (bbslist.c get_emulation_str) so BBS-side detection treats us
// the same
#define TERM_NAME "syncterm"

// Terminal type sent in the rlogin handshake. fTelnet uses this string to
// tell Synchronet the client is CP437 + truecolor capable, which is exactly
// what this client is; boards keying colour depth off it will enable 24-bit.
#define RLOGIN_TERM_NAME "ansi-bbs-cp437-truecolor"

typedef enum {
	ST_DATA, ST_IAC, ST_WILL, ST_WONT, ST_DO, ST_DONT, ST_SB, ST_SB_IAC
} TelnetState;

static int sockfd = -1;
static TelnetState state = ST_DATA;
static u16 termCols = 80, termRows = 25;
static bool nawsOn = false;
static bool eof = false;

static u8 sbOpt;
static u8 sbBuf[64];
static int sbLen;
static ConnProto proto = PROTO_TELNET;
static bool rloginAcked;   // consumed the server's leading NUL yet?

// Raw socket bytes land here before IAC processing
#define RING_SIZE 262144
static u8 ring[RING_SIZE];
static int ringHead, ringTail; // consume at head, produce at tail
static int ringCount;
static u32 totalRx;            // cumulative bytes drained (perf overlay)

void telnetSetSize(u16 cols, u16 rows)
{
	termCols = cols;
	termRows = rows;
}

// Both the main loop and the APC worker thread send; serialize so replies
// never interleave mid-sequence
static LightLock sendLock = 1; // LightLock unlocked state

static void rawSend(const u8* buf, int len)
{
	LightLock_Lock(&sendLock);
	int off = 0;
	while (off < len && sockfd >= 0) {
		int n = send(sockfd, buf + off, len - off, 0);
		if (n < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				continue;
			break;
		}
		off += n;
	}
	LightLock_Unlock(&sendLock);
}

static void sendCmd(u8 cmd, u8 opt)
{
	u8 buf[3] = { T_IAC, cmd, opt };
	rawSend(buf, 3);
}

static void sendNaws(void)
{
	if (proto == PROTO_RLOGIN) {
		// RFC 1282 window-change: magic cookie FF FF 's' 's' then
		// rows, cols, xpixels, ypixels as network-order u16s
		u8 buf[12] = {
			0xFF, 0xFF, 's', 's',
			(u8)(termRows >> 8), (u8)(termRows & 0xFF),
			(u8)(termCols >> 8), (u8)(termCols & 0xFF),
			0, 0, 0, 0
		};
		rawSend(buf, sizeof(buf));
		return;
	}
	u8 buf[9] = {
		T_IAC, T_SB, OPT_NAWS,
		(u8)(termCols >> 8), (u8)(termCols & 0xFF),
		(u8)(termRows >> 8), (u8)(termRows & 0xFF),
		T_IAC, T_SE
	};
	rawSend(buf, sizeof(buf));
}

void telnetNotifySize(u16 cols, u16 rows)
{
	telnetSetSize(cols, rows);
	if (sockfd >= 0 && nawsOn)
		sendNaws();
}

bool telnetConnect(const char* host, u16 port)
{
	return telnetConnectAs(host, port, PROTO_TELNET, NULL, NULL);
}

bool telnetConnectAs(const char* host, u16 port, ConnProto proto_,
                     const char* user, const char* pass)
{
	telnetClose();
	state = ST_DATA;
	nawsOn = false;
	eof = false;
	proto = proto_;
	rloginAcked = false;
	ringHead = ringTail = ringCount = 0;

	char portStr[8];
	snprintf(portStr, sizeof(portStr), "%u", port);

	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* res = NULL;
	if (getaddrinfo(host, portStr, &hints, &res) != 0 || !res)
		return false;

	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd < 0) {
		freeaddrinfo(res);
		return false;
	}

	// Non-blocking connect with a 5s timeout. No poll()/select()/SO_ERROR —
	// none of those report connect-completion reliably on 3DS soc (measured:
	// both waits failed sockets the far end had already accepted). Instead,
	// re-call connect() until it reports EISCONN, the one signal this stack
	// demonstrably gets right.
	struct sockaddr_storage addr;
	socklen_t alen = res->ai_addrlen;
	memcpy(&addr, res->ai_addr, alen < sizeof(addr) ? alen : sizeof(addr));
	fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);
	int rc = connect(sockfd, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (rc < 0) {
		if (errno != EINPROGRESS && errno != EWOULDBLOCK)
			goto fail;
		for (int waited = 0; ; waited += 100) {
			if (waited >= 5000)
				goto fail;
			svcSleepThread(100 * 1000000LL); // 100ms
			rc = connect(sockfd, (struct sockaddr*)&addr, alen);
			if (rc == 0 || errno == EISCONN)
				break;
			if (errno == EALREADY || errno == EINPROGRESS || errno == EWOULDBLOCK)
				continue;
			goto fail;
		}
	}

	// Big receive buffer absorbs audio-blob bursts while frames render;
	// no Nagle so DSR/query replies go out immediately
	int rcvbuf = 128 * 1024;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	int nodelay = 1;
	setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

	if (proto == PROTO_RLOGIN) {
		// RFC 1282 handshake, SyncTerm field convention (rlogin.c):
		//   NUL, password NUL, username NUL, "termtype/speed" NUL.
		// Synchronet reads these for autologin.
		char hs[192];
		int n = 0;
		hs[n++] = 0;
		const char* p = pass ? pass : "";
		const char* u = user ? user : "";
		int l = strlen(p);
		if (l > 63) l = 63;
		memcpy(hs + n, p, l); n += l; hs[n++] = 0;
		l = strlen(u);
		if (l > 63) l = 63;
		memcpy(hs + n, u, l); n += l; hs[n++] = 0;
		n += snprintf(hs + n, sizeof(hs) - n, "%s/115200",
		              RLOGIN_TERM_NAME) + 1;
		rawSend((const u8*)hs, n);
		nawsOn = true; // rlogin window changes need no negotiation
	}

	return true;

fail:
	close(sockfd);
	sockfd = -1;
	return false;
}

bool telnetIsConnected(void)
{
	return sockfd >= 0;
}

void telnetClose(void)
{
	if (sockfd >= 0) {
		close(sockfd);
		sockfd = -1;
	}
}

static void handleWill(u8 opt)
{
	// Accept the server driving echo and suppress-go-ahead; refuse the rest
	if (opt == OPT_ECHO || opt == OPT_SGA)
		sendCmd(T_DO, opt);
	else
		sendCmd(T_DONT, opt);
}

static void handleDo(u8 opt)
{
	if (opt == OPT_NAWS) {
		nawsOn = true;
		sendCmd(T_WILL, opt);
		sendNaws();
	} else if (opt == OPT_SGA || opt == OPT_TTYPE) {
		sendCmd(T_WILL, opt);
	} else {
		sendCmd(T_WONT, opt);
	}
}

static void handleSb(void)
{
	if (sbOpt == OPT_TTYPE && sbLen >= 1 && sbBuf[0] == TTYPE_SEND) {
		u8 buf[4 + sizeof(TERM_NAME) + 2] = { T_IAC, T_SB, OPT_TTYPE, TTYPE_IS };
		memcpy(buf + 4, TERM_NAME, sizeof(TERM_NAME) - 1);
		buf[4 + sizeof(TERM_NAME) - 1] = T_IAC;
		buf[4 + sizeof(TERM_NAME)] = T_SE;
		rawSend(buf, 4 + sizeof(TERM_NAME) + 1);
	}
}

void telnetDrain(void)
{
	if (sockfd < 0)
		return;

	while (ringCount < RING_SIZE) {
		// Largest contiguous chunk at the tail
		int chunk = RING_SIZE - ringTail;
		int space = RING_SIZE - ringCount;
		if (chunk > space)
			chunk = space;

		int n = recv(sockfd, ring + ringTail, chunk, 0);
		if (n == 0) {
			eof = true;
			telnetClose();
			return;
		}
		if (n < 0) {
			if (errno != EWOULDBLOCK && errno != EAGAIN) {
				eof = true;
				telnetClose();
			}
			return;
		}
		ringTail = (ringTail + n) % RING_SIZE;
		ringCount += n;
		totalRx += n;
	}
}

void telnetStats(int* ringBytes, u32* totalRxBytes)
{
	*ringBytes = ringCount;
	*totalRxBytes = totalRx;
}

int telnetRead(u8* out, int cap)
{
	int outLen = 0;

	while (ringCount > 0 && outLen < cap) {
		u8 c = ring[ringHead];
		ringHead = (ringHead + 1) % RING_SIZE;
		ringCount--;

		if (proto == PROTO_RLOGIN) {
			// Byte-transparent: no IAC. The server's initial NUL ack is
			// swallowed; OOB control bytes ride the urgent channel we
			// don't subscribe to, so nothing else needs filtering.
			if (!rloginAcked) {
				rloginAcked = true;
				if (c == 0)
					continue;
			}
			out[outLen++] = c;
		} else {
			switch (state) {
			case ST_DATA:
				if (c == T_IAC) state = ST_IAC;
				else out[outLen++] = c;
				break;
			case ST_IAC:
				switch (c) {
				case T_IAC:  out[outLen++] = c; state = ST_DATA; break;
				case T_WILL: state = ST_WILL; break;
				case T_WONT: state = ST_WONT; break;
				case T_DO:   state = ST_DO;   break;
				case T_DONT: state = ST_DONT; break;
				case T_SB:   state = ST_SB; sbOpt = 0xFF; sbLen = 0; break;
				default:     state = ST_DATA; break; // NOP/GA/etc.
				}
				break;
			case ST_WILL: handleWill(c); state = ST_DATA; break;
			case ST_WONT: state = ST_DATA; break;
			case ST_DO:   handleDo(c);   state = ST_DATA; break;
			case ST_DONT: state = ST_DATA; break;
			case ST_SB:
				if (c == T_IAC)
					state = ST_SB_IAC;
				else if (sbOpt == 0xFF)
					sbOpt = c;
				else if (sbLen < (int)sizeof(sbBuf))
					sbBuf[sbLen++] = c;
				break;
			case ST_SB_IAC:
				if (c == T_SE) {
					handleSb();
					state = ST_DATA;
				} else if (c == T_IAC) {
					if (sbLen < (int)sizeof(sbBuf))
						sbBuf[sbLen++] = c;
					state = ST_SB;
				} else {
					state = ST_DATA;
				}
				break;
			}
		}
	}

	if (outLen == 0 && ringCount == 0 && eof)
		return -1;
	return outLen;
}

void telnetSend(const u8* data, int len)
{
	if (sockfd < 0)
		return;
	if (proto == PROTO_RLOGIN) {
		rawSend(data, len); // transparent: 0xFF is data, not IAC
		return;
	}
	// Escape IAC bytes in outgoing data
	u8 buf[256];
	int o = 0;
	for (int i = 0; i < len; i++) {
		if (o >= (int)sizeof(buf) - 2) {
			rawSend(buf, o);
			o = 0;
		}
		buf[o++] = data[i];
		if (data[i] == T_IAC)
			buf[o++] = T_IAC;
	}
	if (o > 0)
		rawSend(buf, o);
}
