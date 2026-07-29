#!/usr/bin/env python3
"""3dBBS WAN accelerator relay: 3DS -> this machine -> a distant BBS.

Why this exists: the 3DS's TCP stack has a fixed 8KB receive window, which
caps sustained downloads at 8192/RTT — ~45KB/s to a 180ms-away board (see
DESIGN.md section 7.5). A relay on your LAN removes that cap: the 3DS<->relay
leg is ~2ms RTT (no longer window-bound) and the relay<->BBS leg uses this
machine's real TCP stack. rlogin handshakes (autologin) pass through
untouched. Run it, then point a phonebook entry at this machine's LAN IP.

Run:  python3 tools/relay.py [--host futureland.today] [--bbs-port 1513]
                             [--port 2324] [--debug]

--debug additionally logs APC/sixel/CSI wire traffic and listens for the
client's UDP telemetry beacon (dev builds only) — the mode the test rig uses.
"""
import argparse, re, socket, threading, time

CSI_RE = re.compile(rb"\x1b\[[0-9;?<=>! ]*([A-Za-z@`{|}~])")

MARKS_S2C = [b"SyncTERM:C;S;", b"SyncTERM:A;Load", b"SyncTERM:A;LoadBlob",
             b"SyncTERM:A;Queue", b"SyncTERM:A;Update", b"SyncTERM:A;Flush",
             b"SyncTERM:A;Volume", b"\x1bP"]
MARKS_C2S = [b"\x1b[=7;"]


def scan(tag, buf, marks, state):
    for m in marks:
        start = 0
        while True:
            i = buf.find(m, start)
            if i < 0:
                break
            name = m.decode(errors="replace")
            now = time.time()
            last = state.get(name)
            dt = f" dt={now-last:.2f}s" if last else ""
            state[name] = now
            detail = ""
            if m == MARKS_C2S[0]:
                detail = " " + buf[i:i + 16].decode(errors="replace")
            print(f"[{tag}] {name}{detail}{dt}")
            start = i + len(m)


def pump(tag, src, dst, marks, counter, capture_dcs=False, census=None,
         dump_all=False):
    state = {}
    carry = b""
    dcs_buf = None
    dcs_n = 0
    while True:
        try:
            d = src.recv(16384)
        except OSError:
            d = b""
        if not d:
            break
        counter[0] += len(d)
        if dump_all:
            # Client->server is low volume; log it verbatim so terminal
            # capability replies can be compared byte-for-byte
            print(f"[c2s] {d!r}")
        scan(tag, carry + d, marks, state)
        if census is not None:
            for m in CSI_RE.finditer(d):
                k = m.group(1).decode()
                census[k] = census.get(k, 0) + 1
        carry = d[-64:]  # marker split across reads

        if capture_dcs:
            work = d
            while work:
                if dcs_buf is None:
                    i = work.find(b"\x1bP")
                    if i < 0:
                        break
                    pre = (carry + work[:i])[-48:]
                    print(f"[pre-DCS] {pre!r}")  # cursor moves before the image
                    dcs_buf = bytearray()
                    work = work[i + 2:]
                else:
                    j = work.find(b"\x1b\\")
                    if j < 0:
                        dcs_buf += work[:512 * 1024 - len(dcs_buf)]
                        break
                    dcs_buf += work[:j]
                    fn = f"/tmp/sixel_{dcs_n}.bin"
                    with open(fn, "wb") as f:
                        f.write(dcs_buf)
                    print(f"[capture] DCS {len(dcs_buf)} bytes -> {fn}")
                    dcs_n += 1
                    dcs_buf = None
                    work = work[j + 2:]

        try:
            dst.sendall(d)
        except OSError:
            break
    for s in (src, dst):
        try:
            s.close()
        except OSError:
            pass


def telemetry_listener(port=2325):
    """Client-side stats beacon (UDP), printed inline with the wire log."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port))
    while True:
        d, addr = s.recvfrom(512)
        print(f"[3ds] {d.decode(errors='replace')}")


def main():
    ap = argparse.ArgumentParser()
    # 1513 = Futureland's rlogin port: the client's handshake (and with it
    # autologin) relays through untouched, so diagnostic sessions don't need
    # a manual login. Use --bbs-port 23 for a telnet session instead.
    ap.add_argument("--host", default="futureland.today")
    ap.add_argument("--bbs-port", type=int, default=1513)
    ap.add_argument("--port", type=int, default=2324)
    ap.add_argument("--debug", action="store_true",
                    help="log APC/sixel/CSI wire traffic + telemetry beacon")
    args = ap.parse_args()

    if args.debug:
        threading.Thread(target=telemetry_listener, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"relaying :{args.port} -> {args.host}:{args.bbs_port}")

    while True:
        cli, addr = srv.accept()
        print(f"[+] 3DS {addr}, dialing {args.host}")
        try:
            bbs = socket.create_connection((args.host, args.bbs_port), 10)
        except OSError as e:
            print(f"[!] BBS connect failed: {e}")
            cli.close()
            continue
        for s in (cli, bbs):
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        down, up = [0], [0]
        census = {} if args.debug else None
        threading.Thread(target=pump,
                         args=("bbs->3ds", bbs, cli, MARKS_S2C if args.debug else [],
                               down, args.debug, census),
                         daemon=True).start()
        threading.Thread(target=pump,
                         args=("3ds->bbs", cli, bbs, MARKS_C2S if args.debug else [],
                               up, False, None, args.debug),
                         daemon=True).start()

        def rates(down=down, up=up, census=census):
            last_d = last_u = 0
            while True:
                time.sleep(2)
                d, u = down[0], up[0]
                if census:
                    items = sorted(census.items(), key=lambda kv: -kv[1])
                    print("[csi] " + " ".join(f"{k}x{n}" for k, n in items[:12]))
                    census.clear()
                if d == last_d and u == last_u and d:
                    continue
                print(f"[rate] down {(d-last_d)/2048:.0f}KB/s up {(u-last_u)/2048:.1f}KB/s")
                last_d, last_u = d, u
        threading.Thread(target=rates, daemon=True).start()


if __name__ == "__main__":
    main()
