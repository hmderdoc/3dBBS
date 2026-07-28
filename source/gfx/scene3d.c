#include <3ds.h>
#include <math.h>
#include <string.h>
#include "scene3d.h"
#include "vshader_shbin.h"

#define CLEAR_COLOR 0x101020FF
#define SCENE_CLEAR 0x000000FF  // protocol scenes clear to black (terminal bg)

typedef struct { float x, y, z, r, g, b; } vertex;

typedef struct {
	void* vbo;      // linear mem, vertex[]
	u16* ibo;       // linear mem, triangle indices
	int nIdx;
	bool used;
} SceneMesh;

typedef struct {
	bool used;
	int meshSlot;
	float pos[3];
	float rotDeg[3];   // base rotation
	float spinDps[3];  // animator, degrees/second
	float spinAcc[3];  // accumulated (main thread)
	float scale;
} SceneInst;

static DVLB_s* vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection, uLoc_modelView;

static C3D_AttrInfo attrInfo;

static vertex* cube_vbo;
static int cube_count;
static float cubeAngleX, cubeAngleY;

static SceneMesh meshes[SCENE_MAX_MESHES];
static SceneInst insts[SCENE_MAX_INSTS];
static float camPos[3] = { 0, 0, 4 };
static float camLook[3] = { 0, 0, 0 };
static float camFov = 40.0f;
static int liveInsts;
static LightLock sLock;

// Cube corners indexed by bit pattern: bit0=+x, bit1=+y, bit2=+z
static const u8 face_corners[6][4] = {
	{0,1,3,2}, {4,5,7,6}, {0,1,5,4}, {2,3,7,6}, {0,2,6,4}, {1,3,7,5},
};

static vertex cornerVertex(u8 c)
{
	vertex v;
	v.x = (c & 1) ? 1.0f : -1.0f;
	v.y = (c & 2) ? 1.0f : -1.0f;
	v.z = (c & 4) ? 1.0f : -1.0f;
	v.r = (c & 1) ? 1.0f : 0.1f;
	v.g = (c & 2) ? 1.0f : 0.1f;
	v.b = (c & 4) ? 1.0f : 0.1f;
	return v;
}

void scene3dInit(void)
{
	LightLock_Init(&sLock);

	vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);

	uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	uLoc_modelView  = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");

	AttrInfo_Init(&attrInfo);
	AttrInfo_AddLoader(&attrInfo, 0, GPU_FLOAT, 3); // v0=position
	AttrInfo_AddLoader(&attrInfo, 1, GPU_FLOAT, 3); // v1=color

	cube_count = 36;
	cube_vbo = linearAlloc(cube_count * sizeof(vertex));
	vertex* p = cube_vbo;
	for (int f = 0; f < 6; f++) {
		const u8* c = face_corners[f];
		*p++ = cornerVertex(c[0]); *p++ = cornerVertex(c[1]); *p++ = cornerVertex(c[2]);
		*p++ = cornerVertex(c[0]); *p++ = cornerVertex(c[2]); *p++ = cornerVertex(c[3]);
	}
}

void scene3dUpdate(void)
{
	cubeAngleX += C3D_AngleFromDegrees(1.0f);
	cubeAngleY += C3D_AngleFromDegrees(0.7f);

	LightLock_Lock(&sLock);
	for (int i = 0; i < SCENE_MAX_INSTS; i++) {
		if (!insts[i].used)
			continue;
		for (int a = 0; a < 3; a++)
			insts[i].spinAcc[a] += insts[i].spinDps[a] / 60.0f;
	}
	LightLock_Unlock(&sLock);
}

static void bindPipeline(void)
{
	C3D_BindProgram(&program);
	C3D_SetAttrInfo(&attrInfo);

	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
}

static void renderAttract(C3D_RenderTarget* target, float iod)
{
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, cube_vbo, sizeof(vertex), 2, 0x10);

	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
	C3D_FrameDrawOn(target);

	C3D_Mtx projection, modelView;
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(40.0f), C3D_AspectRatioTop,
	                    0.01f, 100.0f, iod, 2.0f, false);
	Mtx_Identity(&modelView);
	Mtx_Translate(&modelView, 0.0f, 0.0f, -4.0f, true);
	Mtx_RotateX(&modelView, cubeAngleX, true);
	Mtx_RotateY(&modelView, cubeAngleY, true);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_modelView, &modelView);
	C3D_DrawArrays(GPU_TRIANGLES, 0, cube_count);
}

static void renderScene(C3D_RenderTarget* target, float iod)
{
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, SCENE_CLEAR, 0);
	C3D_FrameDrawOn(target);

	C3D_Mtx projection, view;
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(camFov),
	                    C3D_AspectRatioTop, 0.01f, 1000.0f, iod, 2.0f, false);
	C3D_FVec eye = FVec3_New(camPos[0], camPos[1], camPos[2]);
	C3D_FVec at = FVec3_New(camLook[0], camLook[1], camLook[2]);
	C3D_FVec up = FVec3_New(0, 1, 0);
	Mtx_LookAt(&view, eye, at, up, false);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);

	for (int i = 0; i < SCENE_MAX_INSTS; i++) {
		SceneInst* in = &insts[i];
		if (!in->used)
			continue;
		SceneMesh* m = &meshes[in->meshSlot];
		if (!m->used)
			continue;

		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, m->vbo, sizeof(vertex), 2, 0x10);

		C3D_Mtx model, mv;
		Mtx_Identity(&model);
		Mtx_Translate(&model, in->pos[0], in->pos[1], in->pos[2], true);
		Mtx_RotateX(&model, C3D_AngleFromDegrees(in->rotDeg[0] + in->spinAcc[0]), true);
		Mtx_RotateY(&model, C3D_AngleFromDegrees(in->rotDeg[1] + in->spinAcc[1]), true);
		Mtx_RotateZ(&model, C3D_AngleFromDegrees(in->rotDeg[2] + in->spinAcc[2]), true);
		Mtx_Scale(&model, in->scale, in->scale, in->scale);
		Mtx_Multiply(&mv, &view, &model);

		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_modelView, &mv);
		C3D_DrawElements(GPU_TRIANGLES, m->nIdx, C3D_UNSIGNED_SHORT, m->ibo);
	}
}

void scene3dRenderTo(C3D_RenderTarget* target, float iod)
{
	bindPipeline();
	LightLock_Lock(&sLock);
	if (liveInsts > 0)
		renderScene(target, iod);
	else
		renderAttract(target, iod);
	LightLock_Unlock(&sLock);
}

bool scene3dActive(void)
{
	return liveInsts > 0;
}

// --- protocol mutators (worker thread) ---

bool scene3dMeshLoad(int slot, const u8* data, int len)
{
	if (slot < 0 || slot >= SCENE_MAX_MESHES || len < 8)
		return false;
	if (memcmp(data, "3DM1", 4) != 0)
		return false;
	u16 nVerts = data[4] | (data[5] << 8);
	u16 nIdx = data[6] | (data[7] << 8);
	if (nIdx % 3 || len < 8 + nVerts * 16 + nIdx * 2)
		return false;

	vertex* vbo = linearAlloc(nVerts * sizeof(vertex));
	u16* ibo = linearAlloc(nIdx * sizeof(u16));
	if (!vbo || !ibo) {
		if (vbo) linearFree(vbo);
		if (ibo) linearFree(ibo);
		return false;
	}

	const u8* vp = data + 8;
	for (int i = 0; i < nVerts; i++, vp += 16) {
		float f[3];
		memcpy(f, vp, 12);
		vbo[i].x = f[0];
		vbo[i].y = f[1];
		vbo[i].z = f[2];
		vbo[i].r = vp[12] / 255.0f;
		vbo[i].g = vp[13] / 255.0f;
		vbo[i].b = vp[14] / 255.0f;
	}
	const u8* ip = data + 8 + nVerts * 16;
	for (int i = 0; i < nIdx; i++)
		ibo[i] = ip[i * 2] | (ip[i * 2 + 1] << 8);
	for (int i = 0; i < nIdx; i++) {
		if (ibo[i] >= nVerts) { // reject out-of-range indices
			linearFree(vbo);
			linearFree(ibo);
			return false;
		}
	}

	LightLock_Lock(&sLock);
	if (meshes[slot].used) {
		linearFree(meshes[slot].vbo);
		linearFree(meshes[slot].ibo);
	}
	meshes[slot].vbo = vbo;
	meshes[slot].ibo = ibo;
	meshes[slot].nIdx = nIdx;
	meshes[slot].used = true;
	LightLock_Unlock(&sLock);
	return true;
}

void scene3dInstSet(int id, int meshSlot, const float pos[3],
                    const float rotDeg[3], float scale, const float spinDps[3])
{
	if (id < 0 || id >= SCENE_MAX_INSTS ||
	    meshSlot < 0 || meshSlot >= SCENE_MAX_MESHES)
		return;
	LightLock_Lock(&sLock);
	SceneInst* in = &insts[id];
	if (!in->used) {
		liveInsts++;
		memset(in->spinAcc, 0, sizeof(in->spinAcc));
	}
	in->used = true;
	in->meshSlot = meshSlot;
	memcpy(in->pos, pos, sizeof(in->pos));
	memcpy(in->rotDeg, rotDeg, sizeof(in->rotDeg));
	memcpy(in->spinDps, spinDps, sizeof(in->spinDps));
	in->scale = scale;
	LightLock_Unlock(&sLock);
}

void scene3dInstRemove(int id)
{
	if (id < 0 || id >= SCENE_MAX_INSTS)
		return;
	LightLock_Lock(&sLock);
	if (insts[id].used) {
		insts[id].used = false;
		liveInsts--;
	}
	LightLock_Unlock(&sLock);
}

void scene3dCamSet(const float pos[3], const float look[3], float fovDeg)
{
	LightLock_Lock(&sLock);
	memcpy(camPos, pos, sizeof(camPos));
	memcpy(camLook, look, sizeof(camLook));
	if (fovDeg >= 10.0f && fovDeg <= 120.0f)
		camFov = fovDeg;
	LightLock_Unlock(&sLock);
}

void scene3dSceneClear(void)
{
	LightLock_Lock(&sLock);
	for (int i = 0; i < SCENE_MAX_INSTS; i++)
		insts[i].used = false;
	liveInsts = 0;
	for (int i = 0; i < SCENE_MAX_MESHES; i++) {
		if (meshes[i].used) {
			linearFree(meshes[i].vbo);
			linearFree(meshes[i].ibo);
			meshes[i].used = false;
		}
	}
	camPos[0] = 0; camPos[1] = 0; camPos[2] = 4;
	camLook[0] = camLook[1] = camLook[2] = 0;
	camFov = 40.0f;
	LightLock_Unlock(&sLock);
}

void scene3dExit(void)
{
	scene3dSceneClear();
	linearFree(cube_vbo);
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}
