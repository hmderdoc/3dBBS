# 3D Authoring Guide

How depth actually works on the 3DS, how to place things so they land where
you want relative to your text UI, and how a frame.js-style layer library
maps onto (and could be extended by) this protocol. Companion to
[PROTOCOL.md](PROTOCOL.md) §6.

## 1. The depth model — no layers, no depth points

The 3DS top screen is a parallax-barrier display: two 400×240 images, one
per eye. Depth is nothing but **horizontal disparity** between them. The
client renders your scene twice through two cameras separated by a small
eye distance (scaled by the user's physical 3D slider — the *user* owns
intensity; slider at zero = flat but still composited).

The one anchor you design against is the **convergence plane**: at
**2.0 world units in front of the camera**, the two eye images coincide —
objects at that distance appear exactly at the physical screen glass.

- camera distance ≈ 2.0 → at the glass
- farther → recedes *into* the screen
- nearer → pops *out in front* of the glass

**Terminal text always sits exactly at the glass** (drawn identically to
both eyes). Two authoring rules follow:

1. Keep 3D objects **behind** the convergence plane wherever text overlaps
   them. Stereo says "object in front" + occlusion says "text in front" =
   visual conflict.
2. Pop-out effects are fine in text-free areas, but keep popped-out objects
   away from screen edges (the bezel cutting off a "nearer than the screen"
   object breaks the illusion — the classic window violation).

With the default camera (`P=0,0,4` looking at the origin, fov 40°): the
origin is 4 units away → comfortably behind the glass. The glass plane is at
z = 2. Comfortable content lives at camera distances ~2–10.

## 2. Placing 3D relative to screen cells

With the default camera, the visible extent at distance `D` in front of the
camera is:

```
worldH(D) = 2 * D * tan(fov/2)          // fov 40° -> worldH ≈ 0.728 * D
worldW(D) = worldH(D) * (400 / 240)     // top screen aspect
```

The 80×25 terminal grid renders 384px wide, centered in 400 (8px side bars),
and exactly 240px tall. To park an object behind cell `(col, row)` at camera
distance `D` (camera at `(0,0,4)`, so object z = `4 - D`):

```
sx = (8 + (col + 0.5) * 4.8) / 400      // horizontal screen fraction 0..1
sy = (row + 0.5) / 25                   // vertical screen fraction 0..1
x  = (sx - 0.5) * worldW(D)
y  = (0.5 - sy) * worldH(D)             // +Y is up
z  = 4 - D
```

Sanity check: `D = 2` puts it at the glass under that cell; `D = 6` puts it
well behind it. (For other grid sizes, replace 4.8 = 384/80 and 25.)

## 3. Byte budget intuition

Retained mode means the ongoing cost of a scene is near zero:

| Action | ~bytes | frequency |
|---|---|---|
| Mesh upload (`C;S`, 500-tri mesh) | ~15–20 KB b64 | once ever (MD5 dedup) |
| `Mesh;Load` + `Obj;Add` | 40–90 | per scene setup |
| Continuous spin (`Spin=`) | 0 | client-side animator |
| Move/retarget an object | ~50 | per update |
| Camera move | ~40 | per update |

A fully animated 3D login screen is a one-time upload plus a few hundred
bytes — 2400-baud spirit preserved.

## 4. The frame.js lens: from layer stacks to depth

Synchronet's `exec/load/frame.js` is the canonical BBS windowing library:
`Frame(x, y, width, height, attr, parent)` objects hold offscreen character
buffers, a **display stack** orders them (`frame.top()` / `frame.bottom()`),
and `cycle()` diffs the composite to the screen. Its "z-axis" is painter's
order only — pseudo-3D: which layer wins a cell, nothing more.

The honest mapping to this protocol:

**What stays exactly as it is.** Frames are text; text lives at the glass.
frame.js keeps doing what it does — layout, input, stacking, diffing — and
none of it needs to know 3D exists. That's a feature: your existing UIs run
unchanged.

**What becomes real depth.** The stack order that today only resolves
overlaps can *additionally* drive a scene behind the glass. The natural v1
extension is a decorator, not a rewrite — conceptually:

```
Frame3D(frame, depth)        // pairs a frame with a depth band behind it
  .backdrop(meshName, opts)  // 3DS:Obj at the frame's projected position,
                             // using §2 math at camera distance `depth`
  .moveTo/top/bottom(...)    // forwards to frame.js, then re-projects its
                             // backdrop objects to the new cell rect
```

Mapping rules that work with today's protocol:
- **stack position → camera distance**: bottom-most frame's backdrop deepest
  (say D=8), each layer above it a step nearer, floor at D≈2.5 so nothing
  crosses the glass under text.
- A frame's backdrop objects are placed via the §2 cell→world formula from
  the frame's `x,y,width,height` rect, and re-placed on `move()`/`top()` —
  ~50 bytes per object per move.
- Since black-background cells are transparent, a frame that wants its
  backdrop *visible through it* leaves black-bg holes; a frame that wants to
  fully occlude uses non-black backgrounds. That's your per-cell "alpha".

**What v1 cannot do (candidate protocol v2 asks).** Be aware of these before
designing around them:
1. **Text is not a 3D object** — a frame cannot itself tilt back or float at
   depth; only its backdrop can. (Would need a "panel" primitive: render a
   character rect onto a textured quad in-scene.)
2. **No textures** — meshes are vertex-colored only.
3. **No per-object depth override of the composite** — 3D is always behind
   all text; you can't sandwich an object between two frames.
4. **No lighting** — bake shading into vertex colors.
5. Convergence distance is fixed at 2.0 (a `Cam;Focal=` knob is a trivial
   client addition if scene-wide depth shifting turns out useful).

When your BBS-side experiments hit one of these walls, that's the signal for
which client feature lands next — the protocol namespace has room for all of
them.

## 5. Testing without a 3DS in the loop

`tests/stress_server.py --port 2323` speaks the whole protocol at a
connecting client (audio melody, ANSI storm, and the `send_3d_demo` pyramid
— a complete literal example of §2/§6 byte sequences to crib from).
`tests/proxy_server.py` relays a real BBS session while logging every APC,
sixel payload, and CSI census entry — point your board through it and you
can read back exactly what your library emitted.
