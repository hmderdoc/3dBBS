# 3dBBS — a Nintendo 3DS BBS terminal with a stereoscopic 3D side-channel

A homebrew 3DS app that is a real ANSI-BBS terminal (SyncTerm-lineage), plus a
custom escape-sequence protocol that lets a BBS render true stereoscopic 3D
scenes on the top screen. Installable on a stock (CFW) 3DS by scanning a QR code.

---

## 1. Feasibility summary

| Piece | Verdict | Notes |
|---|---|---|
| Telnet BBS client on 3DS | Solid | libctru `soc:u` gives BSD sockets; telnet is trivial NVT negotiation |
| ANSI-BBS terminal emulation | Solid | Port Synchronet's `cterm` or reimplement against the CTerm spec |
| 3D graphics layer | Solid | citro3d renders to left/right-eye framebuffers; stereoscopic slider works |
| Custom escape sequences | Solid | CTerm already defines DCS/OSC/APC extension mechanisms; we add our own APC namespace |
| Capability detection | Solid | CTerm-style Device Attributes reply (`CSI < 0 c` → `CSI = ... c`); we answer with our own ID string |
| QR-code install | Solid | Build a `.cia`, host it (GitHub Releases), FBI's Remote Install scans a QR of the URL |
| SSH | Later | SyncTerm uses cryptlib; on 3DS this means porting mbedtls/libssh2. Telnet first. |

## 2. The SyncTerm question: port it, or mine it?

SyncTerm lives in the Synchronet source tree (`sbbs/src/syncterm`) and depends on:

- **ciolib** (`src/conio`) — console I/O abstraction with SDL/X11/ncurses/Win32 drivers; contains **cterm.c**, the actual ANSI-BBS/CTerm terminal state machine
- **xpdev** — cross-platform threads/sockets/filesystem shims
- **cryptlib** — SSH/TLS
- SDL2 — the usual desktop video/audio driver

A wholesale port is the wrong shape: SyncTerm is multithreaded around xpdev
threads, and ciolib's drivers assume a desktop windowing system. The valuable,
portable core is **cterm.c + the ciolib character-buffer model + the CTerm
protocol spec**.

**Decision (settled after inspecting the source, 2026-07): reimplement the
terminal core ourselves against the CTerm spec; vendor Synchronet's font data.**
Measured coupling of the modularized cterm (`cterm.c` + `cterm_cterm.c` +
`cterm_ecma48.c` etc., ~6k lines): it renders through ciolib's vmem/bitmap_con
model (876-line API header, vstat locking/threading) and pulls xpdev
(threadwrap, xpsem, xpbeep, link_list, base64, crc16, xpprintf). The shim would
be bigger than the terminal logic and would import desktop threading
assumptions. What we vendor instead:
- `allfonts.c` — all 45 CTerm fonts as bitmap data (VGA 8×16 CP437 first)
- `cterm.txt` / `cterm.adoc` — the behavioral spec our parser implements
Source: github.com/SynchronetBBS/sbbs mirror of gitlab.synchro.net (GPL).

Licensing: Synchronet/SyncTerm source is GPL — vendoring cterm makes this
project GPL. Fine for homebrew; we ship source anyway.

## 3. Architecture

```
┌───────────────────────────── top screen (400×240, stereo) ─────────────────────────────┐
│  citro3d scene layer (3D protocol content, left/right eye render targets)              │
│  citro2d terminal layer (dynamic cols×rows CP437 grid, 80×25 minimum; visible cells    │
│                          drawn per-frame; composites under/over 3D per protocol flags) │
└────────────────────────────────────────────────────────────────────────────────────────┘
┌──────────────────────────── bottom screen (320×240, touch) ────────────────────────────┐
│  on-screen keyboard + status bar; alt mode: 40-col local view / scrollback / phonebook │
└────────────────────────────────────────────────────────────────────────────────────────┘

app/
  source/
    net/        telnet.c   (RFC 854/856/1073 NVT, binary mode, NAWS)  sock.c (soc:u)
    term/       cterm/     (vendored from sbbs/src/conio, GPL)
                shim/      (minimal ciolib vscreen shim → cell grid)
    gfx/        termgfx.c  (cell grid → citro2d texture atlas render)
                scene3d.c  (citro3d scene graph: meshes, camera, lights, anim)
    proto/      apc3ds.c   (our APC "3DS:" command parser + DA responses)
    ui/         kbd.c      (touch keyboard)  phonebook.c  (dialing directory)
    audio/      ansimusic.c (CSI M / CSI | PLAY-string synth via ndsp)
  romfs/        fonts (CP437 8×16 et al.), keyboard art, default phonebook
```

Single-threaded main loop (poll socket → feed parser → render at 60 Hz);
libctru threads only if the socket read ever needs to move off the main loop.

### Terminal rendering on a 400px screen

**Geometry is dynamic — 80×25 is the *minimum* supported size, not the format.**
The cell grid is heap-allocated at whatever cols×rows the phonebook entry (or
the BBS, via resize sequences) asks for; NAWS (RFC 1073) reports the active
geometry to the BBS, so a custom board can run 120×40 or whatever it likes.

Rendering approach: draw only the *visible* cells each frame straight from the
cell grid via a font texture atlas (citro2d sprite batch). No whole-grid
texture — that would cap size at the GPU's 1024px texture limit (~128 cols);
per-cell drawing is memory-bound instead, so grid size is effectively
unbounded within reason (sanity cap ~240×100).

Views onto the grid:
- **Fit mode**: GPU-scale the full grid to 400×240 (orientation view; legible
  up to ~100 cols, squint-legible beyond)
- **Zoom mode** (shoulder button / tap): 1:1 pixel view, drag-to-pan — the
  DS/3DS browser trick. Auto-follow the cursor while output streams.
- Font size also selectable (8×16, 8×8, 6×8) which changes how much fits
  legibly in fit mode.

## 4. The 3D extension protocol

Follow CTerm's own extension conventions exactly — they solved this problem
already; we just occupy a new namespace.

### 4.1 Capability detection (revised after measuring Synchronet's autodetect)

We identify **as CTerm**, not as a parallel identity — Synchronet's `*`
terminal autodetect (src/sbbs3/answer.cpp) probes with a *plain* `CSI 0 c`
and parses the CTerm banner from the reply, alongside cursor-position
reports for rows/cols and a UTF-8 probe. So:

- `CSI 0 c` and `CSI < 0 c` → `CSI = 67;84;101;114;109;1;332 c` (CTerm 1.332,
  the revision our behavior is implemented against)
- telnet TTYPE → `"syncterm"` (SyncTerm's ANSI-BBS default)
- `APC SyncTERM:VER ST` → `APC SyncTERM:VER;3dBBS <ver> ST` — this is how a
  BBS distinguishes 3dBBS from real SyncTerm
- `APC 3DS:Query ST` → `APC 3DS:Ver;maj;min ST` — same, in our namespace

This makes existing SyncTerm-aware BBS code (Synchronet feature gates on
`cterm_version`) work unmodified.

### 4.2 Command channel: `APC 3DS: ... ST` (v1 implemented 2026-07)

APC strings are invisible to terminals that don't parse them — old clients
skip them safely. Assets travel through the standard SyncTerm cache
(`SyncTERM:C;S`, RAM-first with MD5 dedup via `C;L`); the `3DS:` namespace
drives the scene:

```
APC SyncTERM:C;S;<name>;<b64 3DM1 blob> ST     upload a mesh (once, dedup'd)
APC 3DS:Mesh;Load;S=<slot>;<name> ST           decode cached blob -> mesh slot (0-15)
APC 3DS:Obj;Add=<id>;M=<slot>;P=x,y,z;R=x,y,z;S=<scale>;Spin=x,y,z ST
                                               place/update instance (id 0-31);
                                               Spin = deg/sec per-axis animator
APC 3DS:Obj;Del=<id> ST
APC 3DS:Cam;P=x,y,z;L=x,y,z;Fov=<deg> ST       camera position/look-at/fov
APC 3DS:Scene;Clear ST                         drop all instances+meshes, reset cam
APC 3DS:Query ST                               client replies APC 3DS:Ver;maj;min ST
```

**3DM1 mesh format** (little-endian): `"3DM1"` magic, `u16 nVerts`,
`u16 nIdx`, then nVerts × `{f32 x,y,z; u8 r,g,b,a}` (16 bytes each), then
nIdx × `u16` triangle indices. Vertex-colored, unlit — the low-poly look is
the aesthetic and the ARM11 budget.

**Compositing v1**: while any instance is live, the scene renders on the top
screen first (both eyes, real stereo via the 3D slider) and the terminal
draws over it with black-background cells transparent — text floats over the
3D world. Retained-mode: after upload, an animated scene costs a few dozen
bytes of camera/transform updates.

Sound and file caching use **SyncTerm's own protocol** (implemented 2026-07),
not a 3DS namespace: `SyncTERM:C;S`/`C;L` (store/list cache, MD5 dedup),
`SyncTERM:A;LoadBlob/Load/Synth/Copy/Queue/Flush/Volume/Update` (patch slots +
ndsp channels 2-15), `SyncTERM:Q;*` feature queries (we truthfully advertise
WAV/PCM decode only), and `CSI = 7 n` channel-state reports. The 3D verbs
above stay in the 3DS: namespace since they have no SyncTerm equivalent.

### 4.2b Standard-sequence capabilities (not APC — spec'd by CTerm/xterm already)

- **Sixel graphics** (`DCS q ... ST`, CTerm-compatible): decode to RGBA →
  GPU texture drawn at the cursor's pixel position. Images wider/taller than
  the 3DS's 1024px texture limit get tiled across multiple textures. Well
  within the hardware's ability.
- **ANSI music** (`CSI |` / `CSI M` / `CSI N`): PLAY-string synth via ndsp.
- **Screen resize, BBS-driven**: `CSI 8 ; rows ; cols t` (xterm window ops) —
  reallocs the grid, re-reports via NAWS. Complements the user-driven resize
  from the phonebook entry.

Asset format: start with a compact custom binary (interleaved pos/normal/uv,
u16 indices, RGB565/ETC1 textures) — glTF is overkill for phase 1 and parsing
it on a 268 MHz ARM11 is not. Base64 inside APC; payloads chunked (~4 KB per
APC) so the terminal stays responsive during upload, with the SD-card cache
making repeat visits instant.

Design principle: the BBS drives *retained-mode* scene state with tiny
commands; the client owns the render loop at 60 fps. A 2400-baud-spirit
protocol — after assets are cached, an animated 3D login screen costs a few
hundred bytes.

### 4.3 Server side

The BBS side is the operator's domain. The client's contract is the wire
protocol documented here: detection via the CTerm DA banner / `SyncTERM:VER` /
`3DS:Query`, and the escape sequences in 4.2. Anything that emits those
sequences works.

## 4.5 Display modes (SELECT cycles; implemented in Phase 1.5)

- **keyboard** (primary): terminal on top, touch QWERTY + status bar on bottom
- **mirror**: terminal on both screens; taps on the bottom terminal send real
  mouse reports (CSI ?9 X10, ?1000 normal, ?1006 SGR tracking — the modes
  Synchronet hotspots use), so BBS click handlers work from the touchscreen
- **tall**: one terminal spanning both screens at width-fit scale — 80 cols
  gives an 80×60 grid, auto-resized and NAWS-reported so the BBS gets the rows

Phonebook: `sdmc:/3dBBS/phonebook.txt` (`name|host|port` per line), created
with futureland.today + vert.synchro.net defaults; D-pad cycles entries while
disconnected.

## 5. Input

- Bottom-screen touch keyboard (custom, not swkbd, so keys send immediately —
  swkbd is modal line-entry, wrong for a terminal; keep swkbd as a paste/line mode)
- D-pad/circle pad → arrow keys; A=Enter, B=Backspace, X=Space, Y=Esc;
  shoulder buttons = zoom/pan mode toggle
- Physical keyboard nice-to-have later (none native; not a launch concern)

## 6. Distribution & QR install

1. Build both artifacts: `.3dsx` (Homebrew Launcher) and `.cia` (via makerom + bannertool-successors) with banner/icon/jingle.
2. Publish `.cia` to GitHub Releases at a stable "latest" URL.
3. Generate a QR code of that URL; put it in the README.
4. User flow on CFW 3DS: FBI → Remote Install → Scan QR Code → installed. 
5. Later: submit to Universal-Updater's UniStore for discoverability + updates.

## 7. Toolchain & dev loop (macOS host)

- devkitPro pacman → `dkp-pacman -S 3ds-dev` (devkitARM, libctru, citro3d, citro2d, 3dstools)
- Emulator for the inner loop: **Azahar** (the maintained Citra successor) — has
  socket support, so telnet-to-real-BBS testing works without hardware
- Hardware test over Wi-Fi: `3dslink` to Homebrew Launcher's NetLoader — no SD
  card shuffling
- Public BBSes for testing terminal fidelity + a local Synchronet in Docker for
  protocol work

## 8. Roadmap

- **Phase 0 — scaffold:** toolchain install, hello-triangle + hello-sockets .3dsx running in Azahar
- **Phase 1 — it's a terminal:** telnet NVT, cterm-based ANSI-BBS emulation, CP437 font render, touch keyboard, phonebook. *Success bar: log into a real BBS and it looks right.*
- **Phase 2 — it's SyncTerm-ish:** zoom/pan, loadable fonts (`DCS CTerm:Font`), ANSI music, scrollback, DA replies
- **Phase 3 — it's 3D:** APC 3DS: parser, citro3d scene layer, stereo depth, SD asset cache, `3ds.js` server lib + demo door
- **Phase 4 — ship:** .cia packaging, icon/banner, QR release pipeline (CI), UniStore listing

## Open questions (deferred, not blockers)

- SSH support (mbedtls exists in devkitPro portlibs; cryptlib does not)
- IBM-PC charset edge cases vs. UTF-8 boards (CP437 first; UTF-8 later)
- New3DS vs Old3DS perf budget for the 3D layer (target Old3DS; it's a low-poly aesthetic anyway)
