# 3dBBS Wire Protocol — BBS Author's Reference

This document is written for someone (or some agent) writing **BBS-side
code** — Synchronet JS, a door in any language — that talks to the 3dBBS
client. It assumes no knowledge of the client's internals. Everything here is
implemented and tested against the current client; the byte sequences in
`tests/stress_server.py` are the executable version of this document.

Notation: `ESC` = 0x1B. `CSI x` = `ESC [ x`. `APC ... ST` = `ESC _ ... ESC \`.
`DCS ... ST` = `ESC P ... ESC \`. All APC/DCS payloads are plain text (base64
for binary), so they survive telnet transparently.

**Hard limit:** a single APC/DCS string may be at most **131072 bytes**
(including base64 inflation). Keep individual uploads under ~96KB of binary.

---

## 1. Detection

The client identifies as **CTerm 1.332** — the same family as SyncTERM — so
generic SyncTERM detection works, with two extra probes to distinguish 3dBBS
specifically.

| Probe (BBS sends) | Client replies |
|---|---|
| `CSI 0 c` (plain DA — what Synchronet's `*` autodetect sends) | `CSI = 67;84;101;114;109;1;332 c` ("CTerm" + revision 1.332) |
| `CSI < 0 c` | `CSI < 0;2;4;7 c` (capabilities: 2=bright bg, 4=pixel ops/sixel, 7=mouse) |
| telnet TTYPE | `syncterm` |
| `APC SyncTERM:VER ST` | `APC SyncTERM:VER;3dBBS 0.3 ST` |
| `APC 3DS:Query ST` | `APC 3DS:Ver;0;3 ST` |

Protocol 0.3 adds **text depth layers** (§7); gate them on minor >= 3.

On Synchronet with a `*` terminal type, `console.cterm_version` will be
`1332` after logon — gate SyncTERM-level features on that. To detect 3dBBS
specifically (e.g. before sending 3D), use either APC probe: real SyncTERM
answers `SyncTERM:VER` with its own version string and ignores `3DS:Query`
entirely (timeout = not 3dBBS).

## 2. Terminal capabilities

- **Geometry**: 80×25 default and minimum; up to 240×100. Reported via telnet
  NAWS. The BBS may resize with `CSI 8 ; rows ; cols t` (both 0 = restore
  80×25); a resize clears the screen and re-reports NAWS.
- **Colors**: classic 16 (bold=bright fg; `CSI ? 33 h` enables iCE
  bright backgrounds), xterm-256 (`SGR 38/48;5;n`), and 24-bit truecolor
  (`SGR 38/48;2;r;g;b`). aixterm 90–97/100–107 brights work.
- **Charset**: CP437 only. Bytes 0x80–0xFF are CP437 glyphs. No UTF-8.
- **Mouse**: `CSI ? 9 h` (X10), `CSI ? 1000 h` (press+release), `CSI ? 1006 h`
  (SGR encoding). Touchscreen taps arrive as left-button press+release at the
  tapped cell. Synchronet hotspots work as-is.
- **Scroll regions**: DECSTBM (`CSI top ; bot r`) fully honored, including
  IND/RI at margins and IL/DL bounded by the region.
- **Wrap**: CTerm semantics — immediate wrap on writing the last column;
  `CSI ? 7 l/h` disables/enables.

### Query surface (all answered; safe to probe)

| Query | Reply |
|---|---|
| `CSI 6 n` | `CSI row;col R` (cursor, 1-based) |
| `CSI 255 n` | `CSI rows;cols R` (terminal size, BANSI) |
| `CSI = 1 n` … `= 6 n` | CTerm state reports (font state, set modes, cell size `=3;16;8`, LCF, hyperlinks=0) |
| `CSI = 7 [;ch] n` | audio channel state (see §4) |
| `CSI ? 62 n` / `? 63 n` | macro space / checksum stubs |
| `CSI [?=] Ps $ p` | DECRQM mode reports (real state for ?7/?9/?25/?33/?1000/?1006) |
| `CSI ? 2;1 S` | `CSI ? 2;0;Wpx;Hpx S` (grid pixel size) |
| `OSC 4;n;? / 10;? / 11;? ST` | palette / default fg / default bg as `rgb:RR/GG/BB` |

## 3. File cache (`SyncTERM:C;`)

Per-host cache; assets upload once, ever.

```
APC SyncTERM:C;S;<name>;<base64 bytes> ST      store a file
APC SyncTERM:C;L[;<glob>] ST                   list: client replies one APC:
                                               "SyncTERM:C;L\n<name>\t<md5hex>\n..."
```

- Names: `[A-Za-z0-9._-]`, max 48 chars. No paths.
- **Dedup pattern**: `C;L` before uploading; if the name is listed with the
  right MD5, skip the `C;S`. The client may occasionally forget an entry
  (RAM cache eviction) — always treat a missing listing as "upload again",
  never as an error.
- Stores are served from RAM instantly (safe to `C;S` → use immediately in
  the same burst). Persistence to SD happens at disconnect.

## 4. Audio (`SyncTERM:A;` — cterm.adoc dialect)

PCM patches in 256 slots; playback channels **2–15** (0/1 reserved). All
errors are silent, per spec; confirm with state queries.

| Verb | Effect |
|---|---|
| `A;LoadBlob;S=<slot>;<b64 wav>` | decode WAV directly into a slot |
| `A;Load;S=<slot>;<name>` | decode a `C;S`-cached WAV into a slot |
| `A;Synth;S=<slot>;W=<SIN\|SQ\|SAW\|SILENCE>;F=<hz>;T=<dur>` | synthesize (dur: ms default, `f`=frames, `p`=periods) |
| `A;Copy;S=<src>;D=<dst>` | duplicate a slot |
| `A;Queue;C=<ch>;S=<slot>[;L][;V=<dB>][;I=<dur>][;O=<dur>]` | play: FIFO append; slot is emptied. `L`=loop, `V`=per-clip gain, `I/O`=fade in/out |
| `A;Flush;C=<ch>` | stop + drop the channel queue |
| `A;Volume;C=<ch>;V=<dB>` (or `VL=`/`VR=`) | channel gain (base −12 dB) |
| `A;Update;C=<ch>` | one-shot drain notify: client sends `CSI = 7;<ch>;0 n` when the queue empties. **If the channel is already idle, the notify fires immediately** — safe to arm late |
| `A;Wait;C=<ch>` | accepted, no-op (would stall the UI) |

**Formats**: WAV only — PCM 8/16-bit, mono or stereo (downmixed to mono),
any sample rate. Feature queries reply truthfully:
`APC SyncTERM:Q;libsndfile ST` → `CSI = 7;100;1 n`;
`Q;libsndfileFormat;0x010000;0x0002` (WAV/PCM16) and `;0x0005` (U8) → `1`,
everything else → `0`. **Do not send FLAC/MP3/OGG.**

**Streaming (JIT) notes** — validated against fl_records/lameboy/telnetvision:
- The store→load→queue-per-chunk pattern is the fast path (RAM-served).
- The declared WAV sample rate is honored per chunk: applied directly when
  the channel is idle, **resampled** when queued behind playing audio — so
  rate-based drift correction works.
- The channel FIFO is unbounded; queue as far ahead as you like. Approximations:
  crossfade (`X`) plays as a plain cut-in, `Volume` ramps (`T=`) are instant.

**State query**: `CSI = 7 n` → `CSI = 7[;ch;1]... n` (running channels);
`CSI = 7;<ch> n` → exactly one `ch;state` pair.

## 5. Sixel graphics (`DCS ... q ... ST`)

Standard sixel dialect: raster attributes (`"`), palette define/select (`#`,
RGB `Pu=2` and DEC-HLS `Pu=1`), RLE (`!`), `$`/`-`, transparent background
(`P2=1`). Anchored at the cursor cell when the DCS arrives.

Limits: max 1024×1024 px per image; 16 images live at once (oldest evicted);
colors quantized to RGB555.

**Lifetime semantics (match real CTerm — important for authors):**
- A new image **replaces** any image it overlaps (stream frames freely).
- **Text or erase written over an image destroys it** (v1: the whole image,
  not just the overlapped strip). Repaint-over-old-art works as on hardware.
- Images **scroll with content**, including DECSTBM region scrolls, and are
  clipped/destroyed as they exit the scrolled band.
- `CSI 2 J`, `ESC c`, and resize clear all images.

## 6. 3D scene (`APC 3DS:`)

Retained-mode: upload geometry once (through the §3 cache), then drive the
scene with tiny commands. While any object is live, the scene renders on the
top screen in **true stereoscopic 3D** and the terminal draws over it with
**black-background cells transparent** — text floats at the screen plane over
the 3D world. See docs/3D-AUTHORING.md for the depth model and math.

```
APC SyncTERM:C;S;<name>;<b64 3DM1> ST                       upload mesh
APC 3DS:Mesh;Load;S=<slot>;<name> ST                        decode into mesh slot (0-15)
APC 3DS:Obj;Add=<id>;M=<slot>;P=x,y,z;R=x,y,z;S=<scale>;Spin=x,y,z ST
APC 3DS:Obj;Del=<id> ST
APC 3DS:Cam;P=x,y,z;L=x,y,z;Fov=<deg> ST
APC 3DS:Scene;Clear ST
```

- `Obj;Add` creates **or updates** instance `<id>` (0–31). `P` position,
  `R` base rotation (degrees), `S` uniform scale, `Spin` degrees/second
  per axis (client-side animator — a spinning logo costs zero ongoing bytes).
  Omitted args default to 0 (scale 1).
- `Cam` defaults: `P=0,0,4`, `L=0,0,0`, `Fov=40` (degrees, vertical).
  Coordinates are right-handed, +Y up, +Z toward the default camera.
- `Scene;Clear` drops all instances and meshes and resets the camera.
  Also happens automatically on disconnect.

### 3DM1 mesh format (little-endian)

```
offset size  field
0      4     magic "3DM1"
4      2     u16 nVerts
6      2     u16 nIdx            (must be a multiple of 3; triangle list)
8      16*n  vertices: f32 x, f32 y, f32 z, u8 r, u8 g, u8 b, u8 a
...    2*m   u16 indices
```

Vertex-colored, unlit (alpha currently ignored). Indices out of range reject
the whole mesh. Keep meshes low-poly: this is a 268MHz handheld and the
aesthetic — hundreds of triangles per scene, not tens of thousands.

JS one-liner shape (Synchronet): pack with a byte array and `base64_encode`,
`C;S` it, `Mesh;Load` it, `Obj;Add` it. The pyramid in
`tests/stress_server.py::send_3d_demo` is a complete worked example.

## 7. Text depth layers (`CSI = ... z`) — protocol 0.3

Terminal text no longer has to sit at the glass. Every cell carries a
**layer** (0–15) stamped when it is written; each layer has a BBS-set depth,
and the renderer draws layers deep-to-near with true stereo disparity —
text itself separates in 3D. Layer state is orthogonal to SGR (SGR 0 does
not touch it); `ESC c` resets everything to the classic single-plane look.

```
CSI = Ps z            select the active text layer (Ps = 0..15, clamped)
CSI = Ps ; Pd * z     set layer Ps depth: Pd centi-world-units BEHIND the
                      glass (0 = at the glass; 150 = 1.5 units; clamp 0..1800)
```

- Writes, erases and fills stamp the active layer; scroll/insert/delete ops
  move tags with their cells. One grid, one cursor — whichever layer wrote a
  cell last owns it, exactly like color.
- Depth units match the 3D scene: a text layer at `Pd=150` has the same
  disparity as a scene vertex at camera distance 3.5 (glass = 2.0). Text can
  visually sit ON a scene object.
- The user's 3D slider scales everything; slider at zero renders the classic
  flat screen. Old clients ignore both sequences harmlessly.
- Layer 0 at depth 0 is the default: a BBS that never emits `= z` sequences
  gets today's behavior byte-for-byte.
- Suggested authoring: keep interactive/focused UI at 0–0.4, mid content
  around 0.5–1.5, ambient/background text 2–6. Cells are ~4.8px wide; a
  depth step under ~0.3 units reads as subtle relief, 1+ as clear separation.

## 8. Recommended session flow

```
1. (Synchronet does DA autodetect for terminal '*' users automatically)
2. gate:      console.cterm_version >= 1332   -> SyncTERM-level features OK
3. probe:     APC 3DS:Query ST                -> reply => 3dBBS: 3D/audio/sixel all safe
              (minor >= 3 => text depth layers too)
4. assets:    C;L to dedup, C;S what's missing
5. drive:     audio / sixel / 3D / text layers as above; degrade to ANSI when
              the probe times out
```
