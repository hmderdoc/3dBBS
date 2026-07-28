#ifndef SCENE3D_H
#define SCENE3D_H

#include <3ds/types.h>
#include <citro3d.h>
#include <stdbool.h>

// Stereoscopic 3D scene (raw citro3d pipeline). Two modes:
//  - attract: the spinning cube (shown while disconnected)
//  - protocol: BBS-driven scene via APC "3DS:" verbs — mesh slots loaded
//    from the C;S cache, instances with transforms and spin animators, and
//    a camera. Renders BEFORE the terminal text, so it shows through
//    black-background cells, in stereoscopic depth.
//
// Mesh/instance mutators are called from the APC worker thread; rendering
// happens on main. Internal lock keeps them consistent.
//
// Coexists with citro2d: call C2D_Prepare() after scene3dRenderTo() before
// any 2D drawing in the same frame.

#define SCENE_MAX_MESHES 16
#define SCENE_MAX_INSTS  32

void scene3dInit(void);
void scene3dUpdate(void);   // advance animators one frame (main)
void scene3dRenderTo(C3D_RenderTarget* target, float iod);
void scene3dExit(void);

// --- protocol scene (worker thread unless noted) ---

// Parse a 3DM1 mesh blob into a slot. Format (little-endian):
//   u32 magic "3DM1" | u16 nVerts | u16 nIdx
//   nVerts * { f32 x,y,z; u8 r,g,b,a }
//   nIdx * u16 indices (triangles)
bool scene3dMeshLoad(int slot, const u8* data, int len);

void scene3dInstSet(int id, int meshSlot, const float pos[3],
                    const float rotDeg[3], float scale, const float spinDps[3]);
void scene3dInstRemove(int id);
void scene3dCamSet(const float pos[3], const float look[3], float fovDeg);
void scene3dSceneClear(void);   // also called on disconnect

bool scene3dActive(void);       // any instance live? (main: pick composite path)

#endif
