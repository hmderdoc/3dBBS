# 3dBBS

A BBS terminal for the Nintendo 3DS (homebrew) that renders **stereoscopic 3D
scenes, streamed audio, and sixel graphics driven by the BBS** over a plain
telnet connection — while remaining a faithful ANSI-BBS terminal compatible
with SyncTerm/CTerm conventions.

The client identifies as **CTerm 1.332**, so Synchronet's `*` terminal
autodetect and existing SyncTerm-aware BBS code work unmodified. On top of
that it adds an `APC 3DS:` escape-sequence namespace: a BBS can upload meshes
(cached and deduplicated), place and animate them, and move a camera — all
rendered with real stereo depth on the top screen, composited under the
terminal text.

## Documentation

- **[docs/PROTOCOL.md](docs/PROTOCOL.md)** — the complete wire-protocol
  reference for BBS-side authors: detection, terminal capabilities, the file
  cache, audio, sixel, and the 3D scene protocol. **Start here if you are
  writing BBS code (or are an agent doing so).**
- **[docs/3D-AUTHORING.md](docs/3D-AUTHORING.md)** — the depth model
  (how z maps to what the eye sees), screen-to-world math, authoring
  patterns, and notes on extending a frame.js-style layer library to real 3D.
- **[DESIGN.md](DESIGN.md)** — internal architecture and decision history.

## Building

Requires devkitPro with the 3DS toolchain (`dkp-pacman -S 3ds-dev`):

```
make            # produces 3dBBS.3dsx (Homebrew Launcher / 3dslink)
```

Deploy over Wi-Fi with the Homebrew Launcher netloader:

```
3dslink -a <3ds-ip> 3dBBS.3dsx
```

## Testing

- `tests/host/run.sh` — host-native test suite for the terminal core
  (parser, wrap/region semantics, query replies, sixel decode, fixtures).
  Runs on the dev machine in about a second; run it after any core change.
- `tests/stress_server.py` — a fake "heavy BBS" (dense ANSI + streamed APC
  audio + a spinning 3D mesh demo). Doubles as **executable protocol
  documentation**: it emits exactly the byte sequences described in
  docs/PROTOCOL.md.
- `tests/proxy_server.py` — logging relay between the 3DS and a real BBS;
  captures APC traffic, sixel payloads, CSI usage census, and the client's
  UDP telemetry beacon on one timeline.

## Controls

| Input | Action |
|---|---|
| D-pad up/down (disconnected) | cycle phonebook (`sdmc:/3dBBS/phonebook.txt`) |
| Y (disconnected) | set username/password for the selected board (system keyboard, password masked) |
| X (disconnected) | toggle telnet ↔ rlogin for the selected board |
| Tap status bar | connect / disconnect |
| SELECT | display mode: keyboard → mirror → tall |
| Touch keyboard | input (shift/ctrl sticky); taps on mirrored terminal send mouse clicks |
| D-pad (connected) | arrow keys; A=Enter B=Backspace X=Space Y=Esc |
| Hold START ~1.5s | quit |

## Connecting and autologin

Phonebook entries live in `sdmc:/3dBBS/phonebook.txt`:

```
name|host|port|proto|user|pass      # proto: telnet or rlogin
```

Trailing fields are optional (3 fields = telnet, no credentials). Set
credentials on the device with **Y**, or edit the file directly.

**rlogin (port 513) autologins**: the stored username and password ride the
RFC 1282 handshake using SyncTerm's field convention, which Synchronet reads
for automatic login. Telnet connections do not autologin — the credentials
are stored but not sent (no prompt-matching yet).

> **Credentials are stored in plain text** on the SD card, exactly as
> SyncTerm's `syncterm.lst` does. Anyone with physical access to the card can
> read them. Leave the password empty for boards where that matters.

SSH is **not** supported: devkitPro ships `3ds-mbedtls` but no libssh2 for
3DS, so it would require porting libssh2 to ARM11 first.

## Status / dev notes

Working: terminal core (truecolor, iCE, DECSTBM, dynamic geometry + NAWS),
SyncTerm-compatible identification and query surface, APC audio engine with
JIT streaming, sixel with correct scroll/overwrite lifetime, 3D scene
protocol v1, text depth layers (protocol 0.3 — terminal text at real stereo
depths, PROTOCOL.md §7), three-thread architecture (net+render / APC worker /
SD flush).

Dev-only scaffolding still baked in (strip before any public release): perf
overlay, UDP telemetry beacon, L-button test probe, `LocalTest`/`FL-Proxy`
phonebook entries with hardcoded dev-machine IPs.

Licensing: vendored Synchronet sources (`vendor/synchronet/`, fonts + CTerm
spec) are GPL; this project is consequently GPL. A LICENSE file is still TODO
before publishing.
