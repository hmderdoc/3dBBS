#!/usr/bin/env python3
"""Local BBS stress server: drives the 3dBBS client the way a heavy board
does — dense ANSI repaints + a continuous APC audio stream — with per-second
TX/RX logging so client behavior is measurable without a real BBS.

Run:  python3 tests/stress_server.py [--port 2323] [--no-audio] [--ansi-cells N]
Then connect the 3DS to this machine (LocalTest phonebook entry).
"""
import argparse, base64, io, math, random, socket, struct, sys, threading, time

IAC, SB, SE, WILL, WONT, DO, DONT = 255, 250, 240, 251, 252, 253, 254
OPT_ECHO, OPT_SGA, OPT_TTYPE, OPT_NAWS = 1, 3, 24, 31
ESC = b"\x1b"
APC, ST = b"\x1b_", b"\x1b\\"


def make_wav(freq, dur_s, rate=22050):
    n = int(rate * dur_s)
    frames = b"".join(
        struct.pack("<h", int(12000 * math.sin(2 * math.pi * freq * i / rate)))
        for i in range(n))
    b = io.BytesIO()
    b.write(b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE")
    b.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
    b.write(b"data" + struct.pack("<I", len(frames)) + frames)
    return b.getvalue()


def ansi_burst(cells):
    """Random colored cells sprayed over an 80x25 screen."""
    out = []
    for _ in range(cells):
        r, c = random.randint(1, 25), random.randint(1, 79)
        fg, bg = random.randint(30, 37), random.randint(40, 47)
        ch = chr(random.randint(0x21, 0x7E))
        if random.random() < 0.2:  # sprinkle truecolor
            out.append(f"\x1b[{r};{c}H\x1b[38;2;{random.randint(0,255)};"
                       f"{random.randint(0,255)};{random.randint(0,255)}m{ch}")
        else:
            out.append(f"\x1b[{r};{c}H\x1b[{fg};{bg}m{ch}")
    return "".join(out).encode()


class Client(threading.Thread):
    def __init__(self, sock, addr, args):
        super().__init__(daemon=True)
        self.sock, self.addr, self.args = sock, addr, args
        self.tx = 0
        self.running = True

    def send(self, data):
        self.sock.sendall(data)
        self.tx += len(data)

    def reader(self):
        buf = b""
        while self.running:
            try:
                d = self.sock.recv(4096)
            except OSError:
                break
            if not d:
                break
            buf += d
            # Surface drain notifications distinctly; dump the rest raw
            while b"\x1b[=7;2;0n" in buf:
                i = buf.index(b"\x1b[=7;2;0n")
                if buf[:i]:
                    print(f"[rx] {buf[:i]!r}")
                print(f"[drain] channel 2 idle notify @{time.time():.2f}")
                buf = buf[i + 9:]
            while len(buf) >= 200:
                print(f"[rx] {buf[:200]!r}")
                buf = buf[200:]
            if buf:
                print(f"[rx] {buf!r}")
                buf = b""
        self.running = False

    def send_3d_demo(self):
        """Upload a colored pyramid (3DM1) and spin it behind the terminal."""
        verts = [
            (-1, -1, -1, 255, 60, 60, 255), (1, -1, -1, 60, 255, 60, 255),
            (1, -1, 1, 60, 60, 255, 255), (-1, -1, 1, 255, 255, 60, 255),
            (0, 1.2, 0, 255, 255, 255, 255),
        ]
        idx = [0, 1, 2, 0, 2, 3, 0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4]
        mesh = b"3DM1" + struct.pack("<HH", len(verts), len(idx))
        for v in verts:
            mesh += struct.pack("<fffBBBB", *v)
        for i in idx:
            mesh += struct.pack("<H", i)
        b64 = base64.b64encode(mesh)
        self.send(APC + b"SyncTERM:C;S;pyr.3dm;" + b64 + ST)
        self.send(APC + b"3DS:Mesh;Load;S=0;pyr.3dm" + ST)
        self.send(APC + b"3DS:Obj;Add=0;M=0;P=0,0,0;S=1;Spin=0,45,0" + ST)
        self.send(APC + b"3DS:Cam;P=0,0.8,4;L=0,0,0;Fov=40" + ST)
        print("[3d] pyramid uploaded and spinning")

    def send_layer_demo(self):
        """Text depth layers (protocol 0.3): three lines at three depths."""
        self.send(b"\x1b[=1;80*z\x1b[=2;300*z")     # layer 1 = 0.8u, layer 2 = 3u deep
        self.send(b"\x1b[5;10H\x1b[=2z\x1b[36mdeep background text (3.0)")
        self.send(b"\x1b[7;14H\x1b[=1z\x1b[33mmid-depth text (0.8)")
        self.send(b"\x1b[9;18H\x1b[=0z\x1b[97mglass-level text (0.0)")
        self.send(b"\x1b[0m")
        print("[3d] text layers demo placed (slide the 3D depth slider)")

    def run(self):
        print(f"[+] client {self.addr}")
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=self.reader, daemon=True).start()

        # Telnet negotiation + identity probes (port scanners disconnect
        # mid-handshake; that's fine)
        try:
            self.send(bytes([IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA,
                             IAC, DO, OPT_NAWS, IAC, DO, OPT_TTYPE]))
            time.sleep(0.3)
            self.send(bytes([IAC, SB, OPT_TTYPE, 1, IAC, SE]))  # TTYPE SEND
            self.send(b"\x1b[0c\x1b[6n\x1b[255n")               # DA + CPR + size
            self.send(APC + b"SyncTERM:VER" + ST)
            self.send(b"\x1b[2J\x1b[Hstress server: heavy ANSI + APC audio\r\n")
        except OSError:
            print(f"[-] client {self.addr} left during handshake")
            self.running = False
            self.sock.close()
            return

        if self.args.mesh:
            try:
                self.send_3d_demo()
                self.send_layer_demo()
            except OSError:
                pass

        # Ode to Joy (public domain), C major. A familiar tune makes any
        # dropped/late note instantly audible. (freq, beats); beat = 0.4s.
        C, D, E, F, G = 261.63, 293.66, 329.63, 349.23, 392.0
        notes = [
            (E,1),(E,1),(F,1),(G,1),(G,1),(F,1),(E,1),(D,1),
            (C,1),(C,1),(D,1),(E,1),(E,1.5),(D,0.5),(D,2),
            (E,1),(E,1),(F,1),(G,1),(G,1),(F,1),(E,1),(D,1),
            (C,1),(C,1),(D,1),(E,1),(D,1.5),(C,0.5),(C,2),
        ]
        BEAT = 0.4
        note_i = 0
        next_note = time.time() + 0.5
        last_log = time.time()
        last_tx = 0

        while self.running:
            t = time.time()
            try:
                if self.args.ansi_cells:
                    self.send(ansi_burst(self.args.ansi_cells))
                if self.args.audio and t >= next_note:
                    late = t - next_note
                    freq, beats = notes[note_i % len(notes)]
                    dur = beats * BEAT
                    next_note += dur
                    wav = make_wav(freq, dur)
                    b64 = base64.b64encode(wav)
                    # Real streamer pattern (lameboy/telnetvision): store to a
                    # cycled cache slot, Load by name, Queue, arm drain notify
                    slot = note_i % 4
                    name = f"g{slot}".encode()
                    t0 = time.time()
                    self.send(APC + b"SyncTERM:C;S;" + name + b";" + b64 + ST)
                    self.send(APC + b"SyncTERM:A;Load;S=%d;" % slot + name + ST)
                    self.send(APC + b"SyncTERM:A;Queue;C=2;S=%d" % slot + ST)
                    self.send(APC + b"SyncTERM:A;Update;C=2" + ST)
                    stall = time.time() - t0
                    print(f"[note] #{note_i % len(notes)} {freq:.0f}Hz "
                          f"{beats}b slot=g{slot} late={late:.2f}s"
                          f"{f' SEND STALLED {stall:.2f}s' if stall > 0.1 else ''}")
                    note_i += 1
            except OSError:
                break

            if t - last_log >= 1.0:
                rate = (self.tx - last_tx) / (t - last_log)
                print(f"[tx] {rate/1024:.0f} KB/s total {self.tx/1024:.0f}KB")
                last_tx, last_log = self.tx, t
            time.sleep(self.args.interval)

        print(f"[-] client {self.addr} done")
        self.sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=2323)
    ap.add_argument("--ansi-cells", type=int, default=150,
                    help="cells repainted per tick (0 = no ANSI load)")
    ap.add_argument("--interval", type=float, default=0.05,
                    help="tick interval in seconds")
    ap.add_argument("--no-audio", dest="audio", action="store_false")
    ap.add_argument("--no-mesh", dest="mesh", action="store_false",
                    help="skip the 3D pyramid demo")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"listening on :{args.port} "
          f"(ansi {args.ansi_cells} cells/{args.interval}s, audio {args.audio})")
    while True:
        sock, addr = srv.accept()
        Client(sock, addr, args).start()


if __name__ == "__main__":
    main()
