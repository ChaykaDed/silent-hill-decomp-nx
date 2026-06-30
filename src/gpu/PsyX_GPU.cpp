#include "PsyX_GPU.h"

#include "PsyX/PsyX_public.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_render.h"

#include "../PsyX_main.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "psx/gtereg.h"

#define GET_TPAGE_FORMAT(tpage) ((TexFormat)((tpage >> 7) & 0x3))
#define GET_TPAGE_BLEND(tpage)  ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage) ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)        ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)        (clut >> 6)

OT_TAG prim_terminator = { (uintptr_t)-1, 0 }; // P_TAG with zero primLength

int g_currentOTBucketCount = 0;
float g_otBucketDepth = 0.0f;

/* ----------------------------------------------------------------------------
 * PGXP (perspective-correct rendering) — shadow-memory model, DuckStation-faithful.
 *
 * One shadow table parallels PSX memory: each entry is keyed by the NATIVE
 * ADDRESS of a vertex word (the packed s16 x | s16 y<<16 the GPU reads) and
 * holds the precise float screen X/Y + view W the GTE produced for that word,
 * the integer `value` it shadows (validation, never the key) and the frame
 * generation. Coverage is built by propagation along the data path, never by
 * heuristics:
 *   - GTE store (gte_stsxy*)               -> Shadow_Store(destAddr, fx,fy,w, value)
 *   - drawer copy (poly->xN = screenXy[f]) -> Shadow_Copy(&poly->xN, &screenXy[f])
 *   - GPU draw   (MakeVertex)              -> GetPreciseVertex(primFieldAddr, value, ...)
 * A vertex is either propagated (precise) or absent (clean affine, ppw=0). No
 * ring, no parked set, no nearest-match, no weld — those collide and oscillate.
 * Seams vanish for free: both bone-joint verts are tracked end-to-end and project
 * to the same precise value, so they coincide. All work is gated by g_PsxUsePgxp;
 * the off path is byte-identical to the legacy affine path.
 * -------------------------------------------------------------------------- */

/* Frame generation: bumped once per frame (PGXP_CoverageTick) so a shadow entry
 * left at a packet address reused by a later frame is rejected on lookup. */
static unsigned s_pgxpGen = 1;
extern "C" void PGXP_BumpGen(void) { s_pgxpGen++; }

/* Shadow entry: precise projection of the word at `key`. value = the packed
 * integer (s16 x | s16 y<<16) that lives at key; a draw that reused the address
 * with a different value falls to affine. */
struct ShadowEntry { uintptr_t key; unsigned gen; unsigned value; float x, y, w; };
/* Must hold every projected vertex word AND every copied prim-field word for one
 * frame (~230k verts -> up to ~1M words). 2^21 open-addressed, 16-probe. */
#define SHADOW_BITS 21
#define SHADOW_SIZE (1u << SHADOW_BITS)
#define SHADOW_MASK (SHADOW_SIZE - 1u)
static ShadowEntry s_shadow[SHADOW_SIZE];

static inline unsigned ShadowHash(uintptr_t k) {
	return (unsigned)((k >> 2) * 2654435761u) & SHADOW_MASK;
}

static void Shadow_Put(void* addr, float x, float y, float w, unsigned value) {
	uintptr_t k = (uintptr_t)addr;
	unsigned s = ShadowHash(k);
	for (int i = 0; i < 16; i++) {
		ShadowEntry* e = &s_shadow[(s + i) & SHADOW_MASK];
		if (e->key == k || e->key == 0 || e->gen != s_pgxpGen) {
			e->key = k; e->gen = s_pgxpGen; e->value = value;
			e->x = x; e->y = y; e->w = w; return;
		}
	}
	ShadowEntry* e = &s_shadow[s];   /* probe exhausted: overwrite base */
	e->key = k; e->gen = s_pgxpGen; e->value = value;
	e->x = x; e->y = y; e->w = w;
}

static const ShadowEntry* Shadow_Get(const void* addr) {
	uintptr_t k = (uintptr_t)addr;
	unsigned s = ShadowHash(k);
	for (int i = 0; i < 16; i++) {
		const ShadowEntry* e = &s_shadow[(s + i) & SHADOW_MASK];
		if (e->key == k) return (e->gen == s_pgxpGen) ? e : nullptr;
		if (e->key == 0) return nullptr;
	}
	return nullptr;
}

/* GTE store hook (DuckStation SWC2, done at source level): record the precise
 * projection of the word just written to `addr`. Called from the gte_stsxy*
 * macros via PGXP_StoreAddr, which reads the integer value back from addr. */
extern "C" void Shadow_Store(void* addr, float x, float y, float w, unsigned value) {
	Shadow_Put(addr, x, y, w, value);
}

/* ---- View-space shadow for the per-pixel flashlight -------------------------
 * A second table parallel to s_shadow, keyed the same way (vertex-word native
 * address) and gen-stamped with the same s_pgxpGen (bumped every frame in
 * PGXP_CoverageTick regardless of flags). It holds the GTE RTPS camera-space
 * position of each projected vertex and is propagated along the SAME copy path
 * as PGXP (Shadow_Copy below). Entirely gated by g_PsyX_UsePerPixelFlashlight;
 * the off path never reads or writes it. */
struct VsEntry { uintptr_t key; unsigned gen; float vx, vy, vz; };
static VsEntry s_vshadow[SHADOW_SIZE];

static void Vs_Put(void* addr, float vx, float vy, float vz) {
	uintptr_t k = (uintptr_t)addr;
	unsigned s = ShadowHash(k);
	for (int i = 0; i < 16; i++) {
		VsEntry* e = &s_vshadow[(s + i) & SHADOW_MASK];
		if (e->key == k || e->key == 0 || e->gen != s_pgxpGen) {
			e->key = k; e->gen = s_pgxpGen; e->vx = vx; e->vy = vy; e->vz = vz; return;
		}
	}
	VsEntry* e = &s_vshadow[s];
	e->key = k; e->gen = s_pgxpGen; e->vx = vx; e->vy = vy; e->vz = vz;
}

static const VsEntry* Vs_Get(const void* addr) {
	uintptr_t k = (uintptr_t)addr;
	unsigned s = ShadowHash(k);
	for (int i = 0; i < 16; i++) {
		const VsEntry* e = &s_vshadow[(s + i) & SHADOW_MASK];
		if (e->key == k) return (e->gen == s_pgxpGen) ? e : nullptr;
		if (e->key == 0) return nullptr;
	}
	return nullptr;
}

/* GTE store hook for the flashlight view-space FIFO (PsyX_GTE.cpp PGXP_StoreAddr,
 * fired when g_PsyX_UsePerPixelFlashlight). */
extern "C" void VShadow_Store(void* addr, float vx, float vy, float vz) {
	Vs_Put(addr, vx, vy, vz);
}

/* Drawer copy hook (DuckStation CPU MOVE/SW): the game just did *dst = *src (a
 * vertex word moving from a GTE scratch slot into a prim field). Propagate the
 * shadow along the same path so the GPU resolves the prim-field address. If src
 * isn't tracked, leave dst absent -> clean affine. The PGXP and flashlight
 * shadows are independently gated, so each is byte-identical when its flag is
 * off and both off => an immediate return (legacy path). */
extern "C" void Shadow_Copy(void* dst, const void* src) {
	if (g_PsxUsePgxp) {
		const ShadowEntry* e = Shadow_Get(src);
		if (e) Shadow_Put(dst, e->x, e->y, e->w, *(const unsigned*)dst);
	}
	if (g_PsyX_UsePerPixelFlashlight) {
		const VsEntry* ve = Vs_Get(src);
		if (ve) Vs_Put(dst, ve->vx, ve->vy, ve->vz);
	}
}

/* Max |precise screen coord| (PSX units) PGXP will use before clamping the POSITION
 * for GPU guard-band safety (W is kept either way, so the vertex stays perspective).
 * Higher = less texture compression at the extreme screen edge (more visible in 16:9
 * Hor+), at the cost of larger off-screen NDC. Live-tunable via console `pgxpedge`. */
extern "C" { float g_PgxpEdgeMax = 8192.0f; }

/* PGXP perspective-W precision. 1 (default) = use the unquantized GTE accumulator
 * (gte_shift(m_mac3,1)) for the shader's per-vertex W instead of the clamped 16-bit
 * SZ3 register; coincident edges then get a matching 1/W so seams stop widening with
 * distance. 0 = original quantized SZ3 (for A/B). Live-tunable via console `pgxpdepth`. */
extern "C" { int g_PgxpUseUnquantizedDepth = 1; }

/* GPU draw resolve (DuckStation GetPreciseVertex): shadow at the prim-field
 * address, validated by exact value. Miss / behind-near-plane (W=0) -> affine
 * (ppw=0). rawX/rawY = the integer in the field; ofsX/ofsY = draw-env offset
 * added to land in vertex.x/.y space. */
static inline bool GetPreciseVertex(const void* addr, unsigned value, int rawX, int rawY,
                                    float ofsX, float ofsY, float* ox, float* oy, float* ow) {
	const ShadowEntry* e = Shadow_Get(addr);
	if (e && e->value == value && e->w > 0.0f) {
		/* Keep EVERY valid (W>0) vertex on the perspective path. The warp at the screen
		 * edge is a MIXED polygon: some verts perspective (PGXP), some affine -- the
		 * interpolation across the poly is then inconsistent and smears right where the
		 * affine verts are (just off the 4:3 / 16:9 edge). Rejecting off-screen verts to
		 * affine (what the old +-2px / magnitude-bound code did) CREATES that mix. Instead
		 * use the precise coord so the whole poly is consistently perspective-correct.
		 *
		 * Clamp only for guard-band safety: geometry very close to the camera and off to
		 * the side projects to tens of thousands of units, and such extreme positions
		 * stretch under rasterization. Clamp the POSITION but KEEP W>0 so the vertex stays
		 * perspective (no affine mix). PGXP_OFFSCREEN_MAX is well past the visible width so
		 * the on-screen + just-off-screen geometry (the part that matters) is exact.
		 *
		 * Only W=0 verts -- behind / at the near plane (no valid projection), set in
		 * PsyX_GTE.cpp -- fall through to affine below. */
		const float m = g_PgxpEdgeMax;
		float px = e->x < -m ? -m : (e->x > m ? m : e->x);
		float py = e->y < -m ? -m : (e->y > m ? m : e->y);
		*ox = px + ofsX; *oy = py + ofsY; *ow = e->w; return true;
	}
	*ox = (float)rawX + ofsX; *oy = (float)rawY + ofsY; *ow = 0.0f; return false;
}

/* Precise backface test for the lit-character drawer. The game's gte_nclip runs
 * on the rounded 16-bit screen coords; at distance the cross product of small
 * integers flips sign on near-edge-on faces -> faces get false-culled and the
 * model sheds chunks far away (waist/silhouette first). Recompute the cross
 * product from the PGXP precise float projection (keyed by the screenXy address,
 * the same address SH_PGXP_PROP copies from). Returns 1 = backface (cull), 0 =
 * frontface (keep); falls back to the GTE integer sign when PGXP is off or a
 * vertex isn't tracked. The offset cancels in the cross product, so pass 0. */
static inline bool Pgxp_FetchXY(const void* a, float* x, float* y) {
	float w;
	return GetPreciseVertex(a, *(const unsigned*)a, 0, 0, 0.0f, 0.0f, x, y, &w);
}

extern "C" int PsyX_PGXP_TriBackface(const void* a0, const void* a1, const void* a2, int intNcl)
{
	if (g_PsxUsePgxp) {
		float x0,y0, x1,y1, x2,y2;
		if (Pgxp_FetchXY(a0,&x0,&y0) && Pgxp_FetchXY(a1,&x1,&y1) && Pgxp_FetchXY(a2,&x2,&y2)) {
			float ncl = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
			return ncl <= 0.0f ? 1 : 0;
		}
	}
	return intNcl <= 0 ? 1 : 0;
}

/* Quad = two triangles; the game culls only if BOTH are backfacing (n012<=0 AND
 * n312>=0, opposite winding on the 2nd). Same precise/integer fallback. */
extern "C" int PsyX_PGXP_QuadBackface(const void* a0, const void* a1, const void* a2, const void* a3,
                                      int intN012, int intN312)
{
	if (g_PsxUsePgxp) {
		float x0,y0, x1,y1, x2,y2, x3,y3;
		if (Pgxp_FetchXY(a0,&x0,&y0) && Pgxp_FetchXY(a1,&x1,&y1) &&
		    Pgxp_FetchXY(a2,&x2,&y2) && Pgxp_FetchXY(a3,&x3,&y3)) {
			float n012 = (x1-x0)*(y2-y0) - (x2-x0)*(y1-y0);
			float n312 = (x1-x3)*(y2-y3) - (x2-x3)*(y1-y3);
			return (n012 <= 0.0f && n312 >= 0.0f) ? 1 : 0;
		}
	}
	return (intN012 <= 0 && intN312 >= 0) ? 1 : 0;
}

/* Coverage instrumentation: precise (det) vs affine (miss) per 3D vertex, dumped
 * ~once a second when PGXP is on. Also bumps the frame generation. */
static unsigned int s_pgxpDet = 0, s_pgxpMiss = 0, s_pgxpFrames = 0;
extern "C" void PGXP_CoverageTick(void)
{
	PGXP_BumpGen();
	if (!g_PsxUsePgxp) { s_pgxpDet = s_pgxpMiss = 0; return; }
	if (++s_pgxpFrames >= 60)
	{
		unsigned int tot = s_pgxpDet + s_pgxpMiss;
		if (tot)
			eprintinfo("[PGXP] cov %uf: det=%u(%.0f%%) miss=%u(%.0f%%)\n",
				s_pgxpFrames,
				s_pgxpDet,  100.0 * (double)s_pgxpDet  / (double)tot,
				s_pgxpMiss, 100.0 * (double)s_pgxpMiss / (double)tot);
		s_pgxpDet = s_pgxpMiss = s_pgxpFrames = 0;
	}
}

extern "C" void PGXP_FrameReset(void) { /* shadow is gen-stamped; no reset needed */ }

/* ---- Per-prim affine flag (billboards) -------------------------------------
 * Screen-space prims (billboards, 2D/HUD) build their corners directly, never
 * through the GTE, so they have no shadow and naturally miss to affine. We mark
 * them explicitly too: PsyX_SetNextPrimAffine sets a pending flag, addPrim
 * (PsyX_CaptureGteDepths) records the prim pointer here, and the draw side reads
 * it to force affine. gen-stamped so a reused packet address from last frame is
 * rejected. */
struct AffineEntry { uintptr_t key; unsigned gen; };
#define AFFINE_BITS 15
#define AFFINE_SIZE (1u << AFFINE_BITS)
#define AFFINE_MASK (AFFINE_SIZE - 1u)
static AffineEntry s_affine[AFFINE_SIZE];
static int g_primPgxpForceAffine = 0;

extern "C" void PsyX_SetNextPrimAffine(void)
{
	if (!g_PsxUsePgxp) return;
	g_primPgxpForceAffine = 1;
}

static void AffineStore(const void* prim) {
	uintptr_t key = (uintptr_t)prim;
	unsigned s = (unsigned)((key >> 2) * 2654435761u) & AFFINE_MASK;
	for (int i = 0; i < 16; i++) {
		AffineEntry* e = &s_affine[(s + i) & AFFINE_MASK];
		if (e->key == key || e->key == 0 || e->gen != s_pgxpGen) {
			e->key = key; e->gen = s_pgxpGen; return;
		}
	}
	s_affine[s].key = key; s_affine[s].gen = s_pgxpGen;
}

static bool AffineGet(const void* prim) {
	uintptr_t key = (uintptr_t)prim;
	unsigned s = (unsigned)((key >> 2) * 2654435761u) & AFFINE_MASK;
	for (int i = 0; i < 16; i++) {
		const AffineEntry* e = &s_affine[(s + i) & AFFINE_MASK];
		if (e->key == key) return e->gen == s_pgxpGen;
		if (e->key == 0) return false;
	}
	return false;
}

static bool s_curPgxpAffine = false;
static void PGXP_BeginPrim(const void* prim) { s_curPgxpAffine = AffineGet(prim); }

/* ---- Sub-pixel weld (close PGXP cross-bone joint seams) ---------------------
 * Even with complete coverage a thin residual seam survives: a joint shared by
 * two bone meshes is TWO distinct verts at the same 3D point, and independent
 * fixed-point matrix math per bone projects them up to ~1px apart. PSX integer
 * rounding hid this; PGXP's sub-pixel positions expose it as a seam that shifts
 * with the pose (flickers during animation). Snap a precise vert onto a near-
 * coincident earlier vert THIS FRAME — same screen position within g_pgxpWeldPx
 * AND near-identical depth W — so the shared point renders once. The depth gate
 * is the safety: only verts that are genuinely the same 3D point merge; two
 * different surfaces that merely overlap on screen never fuse. Gen-stamped per
 * frame like the shadow table. Unlike the old weld this runs over COMPLETE
 * coverage with a tight radius, so it only dedups coincident points — it is not
 * papering over missing precise data. */
/* OFF by default: a global distance weld can't tell a real shared joint from any
 * other nearby same-depth vert (a character's whole body is ~one depth), so it
 * flattens detail and spawns new seams — the historical weld failure. Kept behind
 * the console WELD cmd only as an experimental knob; the clean shadow model (WELD 0)
 * is the shipped behaviour. */
float g_pgxpWeldPx     = 0.0f;   /* console WELD:  0 = off (default) */
float g_pgxpWeldWRatio = 1.04f;  /* console WELDW: max depth (W) ratio to weld */
struct WeldEntry { unsigned gen; float x, y, w; };
#define WELD_BITS 17
#define WELD_SIZE (1u << WELD_BITS)
#define WELD_MASK (WELD_SIZE - 1u)
static WeldEntry s_weld[WELD_SIZE];
static inline unsigned WeldHash(int ix, int iy) {
	return ((unsigned)ix * 73856093u) ^ ((unsigned)iy * 19349663u);
}
static void WeldVertex(float* x, float* y, float* w)
{
	if (g_pgxpWeldPx <= 0.0f) return;
	const float r2 = g_pgxpWeldPx * g_pgxpWeldPx;
	int ix = (int)(*x < 0 ? *x - 0.5f : *x + 0.5f);
	int iy = (int)(*y < 0 ? *y - 0.5f : *y + 0.5f);
	int r = (int)(g_pgxpWeldPx + 0.999f);
	if (r < 1) r = 1; else if (r > 4) r = 4;
	for (int dy = -r; dy <= r; dy++)
	for (int dx = -r; dx <= r; dx++) {
		WeldEntry* e = &s_weld[WeldHash(ix + dx, iy + dy) & WELD_MASK];
		if (e->gen != s_pgxpGen) continue;
		float ex = e->x - *x, ey = e->y - *y;
		if (ex * ex + ey * ey > r2) continue;
		float lo = e->w < *w ? e->w : *w, hi = e->w < *w ? *w : e->w;
		if (lo > 0.0f && hi <= lo * g_pgxpWeldWRatio) { *x = e->x; *y = e->y; *w = e->w; return; }
	}
	WeldEntry* e = &s_weld[WeldHash(ix, iy) & WELD_MASK];
	e->gen = s_pgxpGen; e->x = *x; e->y = *y; e->w = *w;
}

/* Fill a GrVertex's precise PGXP fields (ppx/ppy/ppw) from the shadow at the
 * vertex's prim-field address. ppw>0 selects the shader's perspective path;
 * ppw=0 is affine. addr = the field pointer (MakeVertex has it); rawX/rawY = the
 * integer coord in that field. */
static inline void PgxpFillVertex(GrVertex* v, const void* addr, int rawX, int rawY, float ofsX, float ofsY)
{
	if (s_curPgxpAffine) {
		v->ppx = (float)v->x; v->ppy = (float)v->y; v->ppw = 0.0f; s_pgxpMiss++; return;
	}
	float ox, oy, ow;
	if (GetPreciseVertex(addr, *(const unsigned*)addr, rawX, rawY, ofsX, ofsY, &ox, &oy, &ow)) {
		WeldVertex(&ox, &oy, &ow);
		v->ppx = ox; v->ppy = oy; v->ppw = ow; s_pgxpDet++;
	} else {
		v->ppx = (float)v->x; v->ppy = (float)v->y; v->ppw = 0.0f; s_pgxpMiss++;
	}
}

/* Fill a GrVertex's view-space position (vsx/vsy/vsz) from the flashlight shadow
 * at the vertex's prim-field address (same address-keyed lookup as PGXP). A miss
 * leaves the memset-0 default, which the shader treats as "untracked" (vsz<=0,
 * not lit). Called only when g_PsyX_UsePerPixelFlashlight. */
static inline void VsFillVertex(GrVertex* v, const void* addr)
{
	const VsEntry* e = Vs_Get(addr);
	if (e) { v->vsx = e->vx; v->vsy = e->vy; v->vsz = e->vz; }
}

DISPENV currentDispEnv;
DISPENV activeDispEnv;
DRAWENV activeDrawEnv;

static const char* currentSplitDebugText = nullptr;
TextureID overrideTexture = 0;
int overrideTextureWidth = 0;
int overrideTextureHeight = 0;

int g_GPUDisabledState = 0;
int g_DrawPrimMode = 0;

// Per-primitive GTE SZ depth table.  Populated at addPrim time (GTE SZ registers
// valid immediately after RotTransPers calls), looked up during primitive parsing
// to give GL per-vertex perspective depth.  Cleared each GsDrawOt call.
// 4096 slots, linear probe ≤16; collision rate is negligible for typical scene sizes.
#define SZ_TABLE_BITS 12
#define SZ_TABLE_SIZE (1 << SZ_TABLE_BITS)
#define SZ_TABLE_MASK (SZ_TABLE_SIZE - 1)

struct SZEntry { uintptr_t key; uint16_t sz[4]; };
static SZEntry g_szTable[SZ_TABLE_SIZE];

// Global SZ scale: maximum SZ seen in the previous frame, used as the
// depth reference so all polygons share a consistent window_depth space
// regardless of which OT bucket they landed in.
static uint32_t g_szMaxThisFrame = 0;
static uint32_t g_szMaxPrevFrame = 0;

/* PGXP depth fix: the previous frame's max SZ, used to normalize per-vertex
 * SZ3 into NDC depth in the shader (same formula as ApplyGtePerVertexDepth but
 * per-vertex + unquantized, so coplanar faces no longer share a depth bucket).
 * Returns 1 as a safe floor before the first frame. */
extern "C" float PGXP_GetSzMax(void)
{
	return (g_szMaxPrevFrame < 1) ? 1.0f : (float)g_szMaxPrevFrame;
}

// World-geometry renderers (Gfx_MeshDraw) bulk-transform vertices before the
// polygon loop, so the GTE SZ FIFO is stale at each polygon's addPrim call.
// They call PsyX_SetNextPrimSz with the polygon's field_18C SZ values so the
// next PsyX_CaptureGteDepths invocation uses the correct per-vertex depths.
static uint16_t g_primSzNext[4];
static int g_primSzNextValid = 0;

extern "C" void PsyX_SetNextPrimSz(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3, int arg3)
{
	(void)arg3;
	uint16_t avg   = (uint16_t)(((unsigned)s0 + s1 + s2 + s3) >> 2);
	uint16_t avg_q = (uint16_t)((avg >> 6) << 6);
	// Calibrate with unquantised real max so character/item GL depths stay accurate.
	uint32_t mx = s0 > s1 ? s0 : s1;
	if (s2 > mx) mx = s2;
	if (s3 > mx) mx = s3;
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;
	g_primSzNext[0] = g_primSzNext[1] = g_primSzNext[2] = g_primSzNext[3] = avg_q;
	g_primSzNextValid = 1;
}

extern "C" void PsyX_CaptureGteDepths(void* prim)
{
	/* PGXP: if the next prim was flagged screen-space (billboards), record it so
	 * the draw side forces affine. Per-prim, then cleared. */
	if (g_primPgxpForceAffine) {
		AffineStore(prim);
		g_primPgxpForceAffine = 0;
	}

	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);

	uint16_t s0, s1, s2, s3;
	if (g_primSzNextValid) {
		s0 = g_primSzNext[0]; s1 = g_primSzNext[1];
		s2 = g_primSzNext[2]; s3 = g_primSzNext[3];
		g_primSzNextValid = 0;
	} else {
		s0 = (uint16_t)C2_SZ0; s1 = (uint16_t)C2_SZ1;
		s2 = (uint16_t)C2_SZ2; s3 = (uint16_t)C2_SZ3;
	}

	// Track per-frame SZ maximum for global depth calibration
	uint32_t mx = s0 > s1 ? s0 : s1;
	if (s2 > mx) mx = s2;
	if (s3 > mx) mx = s3;
	if (mx > g_szMaxThisFrame) g_szMaxThisFrame = mx;

	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (g_szTable[s].key == 0 || g_szTable[s].key == key) {
			g_szTable[s].key = key;
			g_szTable[s].sz[0] = s0; g_szTable[s].sz[1] = s1;
			g_szTable[s].sz[2] = s2; g_szTable[s].sz[3] = s3;
			return;
		}
	}
	// Probe exhausted — overwrite initial slot
	g_szTable[slot].key = key;
	g_szTable[slot].sz[0] = s0; g_szTable[slot].sz[1] = s1;
	g_szTable[slot].sz[2] = s2; g_szTable[slot].sz[3] = s3;
}

extern "C" void PsyX_ClearGteDepthTable(void)
{
	g_szMaxPrevFrame = g_szMaxThisFrame;
	g_szMaxThisFrame = 0;
	memset(g_szTable, 0, sizeof(g_szTable));
	g_primSzNextValid = 0;
	/* s_shadow / s_affine are gen-stamped, NOT cleared here: this runs at the start
	 * of GsDrawOt, after addPrim filled them but before DrawOTag reads them, so a
	 * memset would wipe the current frame's entries before use. */
	g_primPgxpForceAffine = 0;
	s_curPgxpAffine = false;
}

static bool PsyX_LookupGteDepths(const void* prim, uint16_t* sz)
{
	uintptr_t key = (uintptr_t)prim;
	int slot = (int)((key >> 2) & SZ_TABLE_MASK);
	for (int i = 0; i < 16; i++) {
		int s = (slot + i) & SZ_TABLE_MASK;
		if (g_szTable[s].key == key) {
			sz[0] = g_szTable[s].sz[0]; sz[1] = g_szTable[s].sz[1];
			sz[2] = g_szTable[s].sz[2]; sz[3] = g_szTable[s].sz[3];
			return true;
		}
		if (g_szTable[s].key == 0) break;
	}
	return false;
}

// Overrides flat bucket z with GTE SZ-based depth.
// Uses average SZ across polygon vertices: geometrically more accurate than max_SZ,
// which can be dominated by a single near vertex and sort adjacent coplanar surfaces
// into the wrong OT depth relationship.  Uniform depth per polygon (all vertices
// share one value) eliminates the per-vertex interpolation that caused diffuse
// Z-fighting along polygon edges.
static void ApplyGtePerVertexDepth(GrVertex* vertex, const P_TAG* polyTag, bool isQuad)
{
	if (g_szMaxPrevFrame < 1) return;

	uint16_t sz[4];
	if (!PsyX_LookupGteDepths(polyTag, sz))
		return;

	float sv0, sv1, sv2, sv3 = 0.0f;
	if (isQuad) {
		sv0 = (float)sz[0]; sv1 = (float)sz[1];
		sv2 = (float)sz[3]; sv3 = (float)sz[2];  // buffer[2]=V3, buffer[3]=V2
	} else {
		sv0 = (float)sz[1]; sv1 = (float)sz[2]; sv2 = (float)sz[3];
	}

	float sz_avg = isQuad ? (sv0 + sv1 + sv2 + sv3) * 0.25f
	                      : (sv0 + sv1 + sv2) * (1.0f / 3.0f);
	if (sz_avg < 1.0f) return;  // 2D/HUD prim — keep bucket depth

	float z_val = 1.0f - 2.0f * sz_avg * (1.0f / (float)g_szMaxPrevFrame);
	if (z_val < -1.0f) z_val = -1.0f;
	if (z_val >  1.0f) z_val =  1.0f;
	vertex[0].z = vertex[1].z = vertex[2].z = z_val;
	if (isQuad) vertex[3].z = z_val;
}

struct GPUDrawSplit
{
	DRAWENV			drawenv;
	DISPENV			dispenv;

	BlendMode		blendMode;

	TexFormat		texFormat;
	TextureID		textureId;

	int				drawPrimMode;

	u_short			startVertex;
	u_short			numVerts;

	const char*		debugText;
};

#define MAX_DRAW_SPLITS	 4096

GrVertex g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
GPUDrawSplit g_splits[MAX_DRAW_SPLITS];

int g_vertexIndex = 0;
int g_splitIndex = 0;

void ClearSplits()
{
	currentSplitDebugText = nullptr;
	g_vertexIndex = 0;
	g_splitIndex = 0;
	g_splits[0].texFormat = (TexFormat)0xFFFF;
}

template<class T>
void DrawEnvDimensions(T& width, T& height)
{
	if (activeDrawEnv.dfe)
	{
		width = activeDispEnv.disp.w;
		height = activeDispEnv.disp.h;
	}
	else
	{
		width = activeDrawEnv.clip.w;
		height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float& ofsX, float& ofsY)
{
	if (activeDrawEnv.dfe)
	{
		// also make offset in draw dimensions range to prevent flicker
		const int x = activeDispEnv.disp.x;
		const int y = activeDispEnv.disp.y;
		ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		ofsX = 0.0f;
		ofsY = 0.0f;
	}
}

inline void ScreenCoordsToEmulator(GrVertex* vertex, int count)
{
}

void LineSwapSourceVerts(VERTTYPE*& p0, VERTTYPE*& p1, unsigned char*& c0, unsigned char*& c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if ((p0[0] > p1[0]) ||
		(p0[1] > p1[1] && p0[0] == p1[0]))
	{
		VERTTYPE* tmp = p0;
		p0 = p1;
		p1 = tmp;

		unsigned char* tmpCol = c0;
		c0 = c1;
		c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, ushort gteidx)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	if (dx > abs((short)dy)) 
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else 
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexTriangle(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[0].z = vertex[1].z = vertex[2].z = g_otBucketDepth;

	if (g_PsxUsePgxp)
	{
		PgxpFillVertex(&vertex[0], p0, p0[0], p0[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[1], p1, p1[0], p1[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[2], p2, p2[0], p2[1], ofsX, ofsY);
		/* Per-poly consistency: if ANY vertex fell to affine (ppw<=0 — at/behind the
		 * near plane, where there's no valid perspective projection), drop the WHOLE
		 * poly to affine. A poly with some verts perspective and some affine shears at
		 * the screen edge (the grazing-angle case); consistent affine matches PSX. */
		if (vertex[0].ppw <= 0.0f || vertex[1].ppw <= 0.0f || vertex[2].ppw <= 0.0f)
			vertex[0].ppw = vertex[1].ppw = vertex[2].ppw = 0.0f;
	}

	if (g_PsyX_UsePerPixelFlashlight)
	{
		VsFillVertex(&vertex[0], p0);
		VsFillVertex(&vertex[1], p1);
		VsFillVertex(&vertex[2], p2);
	}

	ScreenCoordsToEmulator(vertex, 3);
}

void MakeVertexQuad(GrVertex* vertex, VERTTYPE* p0, VERTTYPE* p1, VERTTYPE* p2, VERTTYPE* p3, ushort gteidx)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	if (g_PsxUsePgxp)
	{
		PgxpFillVertex(&vertex[0], p0, p0[0], p0[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[1], p1, p1[0], p1[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[2], p2, p2[0], p2[1], ofsX, ofsY);
		PgxpFillVertex(&vertex[3], p3, p3[0], p3[1], ofsX, ofsY);
		/* Per-poly consistency (see MakeVertexTri): any affine vertex -> whole poly affine. */
		if (vertex[0].ppw <= 0.0f || vertex[1].ppw <= 0.0f ||
		    vertex[2].ppw <= 0.0f || vertex[3].ppw <= 0.0f)
			vertex[0].ppw = vertex[1].ppw = vertex[2].ppw = vertex[3].ppw = 0.0f;
	}

	if (g_PsyX_UsePerPixelFlashlight)
	{
		VsFillVertex(&vertex[0], p0);
		VsFillVertex(&vertex[1], p1);
		VsFillVertex(&vertex[2], p2);
		VsFillVertex(&vertex[3], p3);
	}

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeVertexRect(GrVertex* vertex, VERTTYPE* p0, short w, short h, ushort gteidx)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(ofsX, ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;

	vertex[0].z = vertex[1].z = vertex[2].z = vertex[3].z = g_otBucketDepth;

	ScreenCoordsToEmulator(vertex, 4);
}

void MakeTexcoordQuad(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, unsigned char* uv3, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const unsigned char bright = 2;
	// Strip ABR (bits 5-6) and TP (bits 7-8) from tpage - shader only needs X/Y page coords (bits 0-4)
	short pageCoord = page & 0x1F;

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = pageCoord;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordTriangle(GrVertex* vertex, unsigned char* uv0, unsigned char* uv1, unsigned char* uv2, short page, short clut, unsigned char dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const unsigned char bright = 2;
	// Strip ABR (bits 5-6) and TP (bits 7-8) from tpage - shader only needs X/Y page coords (bits 0-4)
	short pageCoord = page & 0x1F;

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordRect(GrVertex* vertex, unsigned char* uv, short page, short clut, short w, short h)
{
	assert(uv);

	// sim overflow
	if (int(uv[0]) + w > 255) w = 255 - uv[0];
	if (int(uv[1]) + h > 255) h = 255 - uv[1];

	const unsigned char bright = 2;
	const unsigned char dither = 0;
	// Strip ABR (bits 5-6) and TP (bits 7-8) from tpage - shader only needs X/Y page coords (bits 0-4)
	short pageCoord = page & 0x1F;

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = pageCoord;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = pageCoord;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = pageCoord;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = pageCoord;
	vertex[3].clut = clut;

	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}
}

void MakeTexcoordLineZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex* vertex, unsigned char dither)
{
	const unsigned char bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex* vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		vertex[n]._p0 = 0;
		--n;
	}
}

void MakeColourLine(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;
	vertex[1]._p0 = 0;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;
	vertex[2]._p0 = 0;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
	vertex[3]._p0 = 0;
}

void MakeColourTriangle(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;
	vertex[1]._p0 = 0;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
	vertex[2]._p0 = 0;
}

void MakeColourQuad(GrVertex* vertex, bool shadeTexOn, unsigned char* col0, unsigned char* col1, unsigned char* col2, unsigned char* col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;
	vertex[0]._p0 = 0;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;
	vertex[1]._p0 = 0;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
	vertex[2]._p0 = 0;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
	vertex[3]._p0 = 0;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	g_vertexBuffer[g_vertexIndex + 4] = g_vertexBuffer[g_vertexIndex + 3];

	g_vertexBuffer[g_vertexIndex + 5] = g_vertexBuffer[g_vertexIndex + 2];
	g_vertexBuffer[g_vertexIndex + 2] = g_vertexBuffer[g_vertexIndex + 3];
	g_vertexBuffer[g_vertexIndex + 3] = g_vertexBuffer[g_vertexIndex + 1];
}

//------------------------------------------------------------------------------------------------------------------------

static void AddSplit(bool semiTrans, bool textured)
{
	int tpage = activeDrawEnv.tpage;
	GPUDrawSplit& curSplit = g_splits[g_splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GET_TPAGE_FORMAT(tpage);
	TextureID textureId = textured ? g_vramTexture : g_whiteTexture;

	if (textured && overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = overrideTexture;
	}

	// FIXME: compare drawing environment too?
	if (curSplit.blendMode == blendMode &&
		curSplit.texFormat == texFormat &&
		curSplit.textureId == textureId &&
		curSplit.drawPrimMode == g_DrawPrimMode &&
		curSplit.drawenv.clip.x == activeDrawEnv.clip.x &&
		curSplit.drawenv.clip.y == activeDrawEnv.clip.y &&
		curSplit.drawenv.clip.w == activeDrawEnv.clip.w &&
		curSplit.drawenv.clip.h == activeDrawEnv.clip.h &&
		curSplit.drawenv.dfe == activeDrawEnv.dfe &&
		curSplit.debugText == currentSplitDebugText)
	{
		return;
	}

	curSplit.numVerts = g_vertexIndex - curSplit.startVertex;

	if (g_splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		eprinterr("MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors\n");
		return;
	}

	GPUDrawSplit& split = g_splits[++g_splitIndex];
	split.blendMode = blendMode;
	split.texFormat = texFormat;
	split.textureId = textureId;
	split.drawPrimMode = g_DrawPrimMode;
	split.drawenv = activeDrawEnv;
	split.dispenv = activeDispEnv;
	split.debugText = currentSplitDebugText;

	split.drawenv.tw.w = overrideTextureWidth;
	split.drawenv.tw.h = overrideTextureHeight;

	split.startVertex = g_vertexIndex;
	split.numVerts = 0;
}

/* Debug isolation of the additive (BM_ADD) layer, driven by the `add` console cmd.
 * 0 = drop every additive split (confirm whether a fire/lightning effect is additive
 * geometry), 1 = normal, 2 = draw additive WITH the depth test that GR_SetBlendMode
 * normally disables (test the "additive draws through the floor" hypothesis). */
int g_PsxDbgAddMode = 1;

void DrawSplit(const GPUDrawSplit& split)
{
	const bool isAdditive = (split.blendMode == BM_ADD || split.blendMode == BM_ADD_QUATER_SOURCE);

	if (g_PsxDbgAddMode == 0 && isAdditive)
		return;

	{
		/* [WORLDSPLIT] Identify which render path the 3D WORLD uses. The old
		 * cap of 40 only caught boot/title 2D prims (verts 6-18, dfe=1). World
		 * geometry chunks have many more verts; log the first 40 big splits so
		 * the gameplay world's dfe (-> enable=!dfe -> which GR_SetOffscreenState
		 * branch / ortho) is visible in one in-game capture. */
		static int bigSplitLog = 0;
		if (bigSplitLog < 40 && split.numVerts >= 60) {
			eprintf("[WORLDSPLIT] verts=%d dfe=%d fmt=%d blend=%d texId=%u clip=(%d,%d,%d,%d)\n",
				split.numVerts, split.drawenv.dfe, split.texFormat, split.blendMode,
				(unsigned)split.textureId, split.drawenv.clip.x, split.drawenv.clip.y,
				split.drawenv.clip.w, split.drawenv.clip.h);
			bigSplitLog++;
		}
	}
	if(split.debugText)
		GR_PushDebugLabel(split.debugText);

	GR_SetStencilMode(split.drawPrimMode);	// draw with mask 0x16

	GR_SetTexture(split.textureId, split.texFormat);

	if (split.texFormat == TF_32_BIT_RGBA)
		GR_SetOverrideTextureSize(split.drawenv.tw.w, split.drawenv.tw.h);

	const bool drawOnScreen = split.drawenv.dfe;
	GR_SetupClipMode(&split.drawenv.clip, drawOnScreen);
	GR_SetOffscreenState(&split.drawenv.clip, !drawOnScreen);

	GR_SetBlendMode(split.blendMode);

	if (g_PsxDbgAddMode == 2 && isAdditive)
		GR_EnableDepth(1);

	GR_DrawTriangles(split.startVertex, split.numVerts / 3);

	if (split.debugText)
		GR_PopDebugLabel();
}

extern int g_dbg_polygonSelected;

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
#ifdef _DEBUG
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex* vert = &g_vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			eprintf(" ==========================================\n");
			eprintf(" POLYGON: %d\n", g_dbg_polygonSelected);
                        eprintf("X: %d Y: %d\n", vert->x, vert->y);
                        eprintf("U: %d V: %d\n", vert->u, vert->v);
                        eprintf("TP: %d CLT: %d\n", vert->page, vert->clut);
			
			eprintf(" ==========================================\n");
		}

		PsyX_UpdateInput();
	}
#endif // _DEBUG

	// next code ideally should be called before EndScene
	GR_UpdateVertexBuffer(g_vertexBuffer, g_vertexIndex);

	for (int i = 1; i <= g_splitIndex; i++)
		DrawSplit(g_splits[i]);

	ClearSplits();
}

// forward declarations
int ParsePrimitive(P_TAG* polyTag);

void ParsePrimitivesLinkedList(u_long* p, int singlePrimitive)
{
	if (!p)
		return;

	// setup single primitive flag (needed for AddSplits)
	g_DrawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG* polyTag = reinterpret_cast<P_TAG*>(p);
		ParsePrimitive(polyTag);

		GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
		lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;
	}
	else
	{
		// Bucket-accurate depth: all primitives inside the same OT bucket share
		// one depth value — matching the PSX's painter's-algorithm intent.
		// g_otBucketDepth advances only at tagLength==0 bucket-boundary entries.
		int otBucketIdx = 0;
		const float otBucketStep = (g_currentOTBucketCount > 1)
			? (2.0f / (float)(g_currentOTBucketCount - 1)) : 0.0f;
		g_otBucketDepth = -1.0f;
		// walk OT_TAG linked list with safety guards
		uintptr_t basePacket = reinterpret_cast<uintptr_t>(p);
		for (int safety = 0; safety < 16384; safety++)
		{
			const int tagLength = getlen(basePacket);
			if (tagLength > 0 && tagLength <= 32)
			{
				uintptr_t currentPacket = basePacket;
				const uintptr_t endPacket = basePacket + (tagLength + P_LEN) * sizeof(u_int);
				int primLength = 0;
				while (currentPacket < endPacket)
				{
					primLength = ParsePrimitive(reinterpret_cast<P_TAG*>(currentPacket));
					if (primLength <= 0) break;
					currentPacket += (primLength + P_LEN) * sizeof(u_int);
				}

				if (currentPacket != endPacket)
				{
					eprinterr("did not output valid primitive or ptag length is not valid (diff=%d)\n", endPacket-currentPacket);
					/* One-shot dump: the corrupted prim's raw bytes
					 * fingerprint the writer. Same approach that pinned
					 * the knife OT corruption to func_800611C0 via the
					 * recognizable .NHS. tail bytes from POLY_FT4 vertex
					 * data. After the first dump, fall back to the
					 * existing rate-limited summary above. */
					static int s_badPrimDumped = 0;
					if (!s_badPrimDumped) {
						s_badPrimDumped = 1;
						const uint32_t* w = reinterpret_cast<const uint32_t*>(basePacket);
						eprintinfo("[OT-PRIM] FIRST corrupt prim at %p tagLen=%d code=0x%02x\n",
							(void*)basePacket, tagLength,
							reinterpret_cast<P_TAG*>(basePacket)->code);
						eprintinfo("[OT-PRIM]   raw 64 bytes: %08x %08x %08x %08x %08x %08x %08x %08x\n",
							(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
							(unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
						eprintinfo("[OT-PRIM]                 %08x %08x %08x %08x %08x %08x %08x %08x\n",
							(unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
							(unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
					}
				}
			}
			else if (tagLength == 0)
			{
				// OT bucket boundary — advance to the next bucket's depth.
				g_otBucketDepth = -1.0f + (float)otBucketIdx * otBucketStep;
				if (g_otBucketDepth > 1.0f) g_otBucketDepth = 1.0f;
				otBucketIdx++;
			}
			else if (tagLength > 32)
			{
				eprinterr("got invalid tag length %d, code %d\n", tagLength, reinterpret_cast<P_TAG*>(basePacket)->code);
				static int s_badTagDumped = 0;
				if (!s_badTagDumped) {
					s_badTagDumped = 1;
					const uint32_t* w = reinterpret_cast<const uint32_t*>(basePacket);
					eprintinfo("[OT-PRIM] FIRST bad-tag-len at %p tagLen=%d\n",
						(void*)basePacket, tagLength);
					eprintinfo("[OT-PRIM]   raw 64 bytes: %08x %08x %08x %08x %08x %08x %08x %08x\n",
						(unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
						(unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7]);
					eprintinfo("[OT-PRIM]                 %08x %08x %08x %08x %08x %08x %08x %08x\n",
						(unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
						(unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
				}
			}

			GPUDrawSplit& lastSplit = g_splits[g_splitIndex];
			lastSplit.numVerts = g_vertexIndex - lastSplit.startVertex;

			if (isendprim(basePacket))
				break;

			// Validate next pointer before following it.
			// Crash root-caused via WinDbg minidump on the muzzle-flash repro:
			// FAILURE_BUCKET_ID INVALID_POINTER_READ at this exact site,
			// stack ParsePrimitivesLinkedList+0xa5 -> DrawOTag -> GsDrawOt.
			// The next-pointer can land on:
			//   1. NULL / very low (uninitialized OT bucket)        — break
			//   2. (uintptr_t)-1 == 0xFFFF..FF (PSX legacy terminator
			//      written by some not-fully-ported code; differs from
			//      &prim_terminator that isendprim looks for)        — break
			//   3. Unmapped high address (Windows user mode tops at
			//      0x7FFF'FFFF'FFFF; anything past that is kernel)   — break
			//   4. Wild but technically-mapped — can't catch without
			//      VirtualQuery; rely on the 16384 safety counter.
			uintptr_t nextPtr = reinterpret_cast<uintptr_t>(nextPrim(basePacket));
			if (nextPtr < 0x10000 ||
			    nextPtr == static_cast<uintptr_t>(-1) ||
			    nextPtr >= 0x7FFFFFFFFFFFULL) {
				static int s_badNextLogged = 0;
				if (s_badNextLogged < 16) {
					eprintinfo("[OT] bad nextPtr=0x%llX at %p — chain walk halted\n",
						(unsigned long long)nextPtr, (void*)basePacket);
					s_badNextLogged++;
				}
				break;
			}
			basePacket = nextPtr;
		}
	}
}

inline int IsNull(POLY_FT3* poly)
{
	return  poly->x0 == -1 &&
		poly->y0 == -1 &&
		poly->x1 == -1 &&
		poly->y1 == -1 &&
		poly->x2 == -1 &&
		poly->y2 == -1;
}

static int ProcessFlatLines(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2* poly = (LINE_F2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = c0;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3* poly = (LINE_F3*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 5;
	}
	case 0xc:
	{
		int i;
		LINE_F4* poly = (LINE_F4*)polyTag;

		AddSplit(semiTrans, false);

		{
			VERTTYPE* p0 = &poly->x0;
			VERTTYPE* p1 = &poly->x1;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x1;
			VERTTYPE* p1 = &poly->x2;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		{
			VERTTYPE* p0 = &poly->x2;
			VERTTYPE* p1 = &poly->x3;
			unsigned char* c0 = &poly->r0;
			unsigned char* c1 = c0;

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			LineSwapSourceVerts(p0, p1, c0, c1);
			MakeLineArray(firstVertex, p0, p1, gteIndex);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}

		return 6;
	}
	}
	return 0;
}

static int ProcessGouraudLines(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2* poly = (LINE_G2*)polyTag;

		AddSplit(semiTrans, false);

		VERTTYPE* p0 = &poly->x0;
		VERTTYPE* p1 = &poly->x1;
		unsigned char* c0 = &poly->r0;
		unsigned char* c1 = &poly->r1;

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		LineSwapSourceVerts(p0, p1, c0, c1);
		MakeLineArray(firstVertex, p0, p1, gteIndex);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

static int ProcessFlatPoly(P_TAG* polyTag)
{
	/* PGXP hint: the prim's stamped GTE ring position (0xFFFF / ignored when
	 * PGXP off). 3D polygons only — sprites/tiles/lines stay 0xFFFF. */
	const u_short gteIndex = g_PsxUsePgxp ? polyTag->pgxp_index : (u_short)0xFFFF;
	if (g_PsxUsePgxp) PGXP_BeginPrim(polyTag);

	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3* poly = (POLY_F3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x4:
	{
		POLY_FT3* poly = (POLY_FT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
			AddSplit(semiTrans, true);

			GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
			ApplyGtePerVertexDepth(firstVertex, polyTag, false);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
			polygon_count++;
#endif
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4* poly = (POLY_F4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;
#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 5;
	}
	case 0xC:
	{
		POLY_FT4* poly = (POLY_FT4*)polyTag;
		/* PC-port guard: skip POLY_FT4 with obviously bogus tpage/clut/UV.
		 * Combat particle effects (muzzle flash, blood splat, sparks) build
		 * prims with weapon-specific CLUT/TPAGE bits that reference VRAM
		 * regions which may not be correctly populated in PsyCross's
		 * software VRAM. Without this guard those prims trip a shader
		 * read into uninitialized GPU memory and crash GsDrawOt.
		 *
		 * Validation:
		 *   - tpage low 5 bits give (TX, TY) page index. TX is 0..15,
		 *     TY is 0..1. Anything outside is a corrupt prim.
		 *   - clut Y is bits 6..14, must fit VRAM height (512). Y > 511
		 *     means the prim was built with a stale/uninitialized clut
		 *     field.
		 *   - All four UVs at (0,0) typically means an unrendered ghost
		 *     prim — kept anyway since some valid prims pin to (0,0).
		 *
		 * Drops 0..1% of prims in the wild. If everything's getting
		 * dropped, the guard's too tight or the upload path is broken
		 * upstream — check [PFT4DROP] log entries. */
		{
			short tpage = poly->tpage;
			short clut = poly->clut;
			int tx = tpage & 0xF;          /* page X (0..15) */
			int ty = (tpage >> 4) & 0x1;   /* page Y (0..1) */
			/* Real clut Y is 9 bits (0..511); bit 15 is reserved/0 on valid
			 * prims. Read unsigned and do NOT mask to 0x1FF so a set bit 15
			 * (uninitialized/garbage clut) pushes clutY past 511 and trips the
			 * guard. The old `& 0x1FF` capped clutY at 511, making `> 511` dead
			 * code that never dropped or logged a single prim. */
			int clutY = ((unsigned short)clut) >> 6;
			(void)tx; (void)ty;
			if (clutY > 511) {
				static int s_pft4DropCount = 0;
				if (s_pft4DropCount < 32) {
					eprintinfo("[PFT4DROP] tpage=0x%04hX clut=0x%04hX uvs=(%d,%d)(%d,%d)(%d,%d)(%d,%d) reason=clutY_oob (%d)\n",
						tpage, clut,
						poly->u0, poly->v0, poly->u1, poly->v1,
						poly->u2, poly->v2, poly->u3, poly->v3,
						clutY);
					s_pft4DropCount++;
				}
				return 9;  /* skip rendering, advance past prim */
			}
		}
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	}
	return 0;
}

static int ProcessGouraudPoly(P_TAG* polyTag)
{
	/* PGXP hint (3D polygons only). 0xFFFF / ignored when PGXP off. */
	const u_short gteIndex = g_PsxUsePgxp ? polyTag->pgxp_index : (u_short)0xFFFF;
	if (g_PsxUsePgxp) PGXP_BeginPrim(polyTag);

	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3* poly = (POLY_G3*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		// Per-vertex fog factor packed into pad1/pad2 (game writes fog amount there).
		// v0 shares v1's fog (code byte occupies v0's pad slot).
		firstVertex[0]._p0 = poly->pad1;
		firstVertex[1]._p0 = poly->pad1;
		firstVertex[2]._p0 = poly->pad2;

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 6;
	}
	case 0x4:
	{
		POLY_GT3* poly = (POLY_GT3*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, false);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		// Copy per-vertex fog factor from pad bytes
		firstVertex[0]._p0 = poly->p1;  // v0: shares v1's fog (code byte occupies v0's pad)
		firstVertex[1]._p0 = poly->p1;  // v1
		firstVertex[2]._p0 = poly->p2;  // v2

		g_vertexIndex += 3;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 9;
	}
	case 0x8:
	{
		POLY_G4* poly = (POLY_G4*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		// Per-vertex fog factor packed into pad1/pad2/pad3 (note: MakeColourQuad swaps v2/v3).
		firstVertex[0]._p0 = poly->pad1;
		firstVertex[1]._p0 = poly->pad1;
		firstVertex[2]._p0 = poly->pad3;
		firstVertex[3]._p0 = poly->pad2;

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 8;
	}
	case 0xC:
	{
		POLY_GT4* poly = (POLY_GT4*)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2, gteIndex);
		ApplyGtePerVertexDepth(firstVertex, polyTag, true);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		// Copy per-vertex fog factor from pad bytes (note: MakeColourQuad swaps v2/v3)
		firstVertex[0]._p0 = (unsigned char)poly->pad2;  // v0: own fog (game carries it in pad2; v0 color word's pad is the code byte)
		firstVertex[1]._p0 = poly->p1;  // v1
		firstVertex[2]._p0 = poly->p3;  // v3 (buffer[2] = poly vertex 3 due to swap)
		firstVertex[3]._p0 = poly->p2;  // v2 (buffer[3] = poly vertex 2 due to swap)

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 12;
	}
	}
	return 0;
}

static int ProcessTileAndSprt(P_TAG* polyTag)
{
	const u_short gteIndex = 0xFFFF;

	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE* poly = (TILE*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x64:
	{
		SPRT* poly = (SPRT*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 4;
	}
	case 0x68:
	{
		TILE_1* poly = (TILE_1*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x70:
	{
		TILE_8* poly = (TILE_8*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x74:
	{
		SPRT_8* poly = (SPRT_8*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	case 0x78:
	{
		TILE_16* poly = (TILE_16*)polyTag;

		AddSplit(semiTrans, false);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 2;
	}
	case 0x7C:
	{
		SPRT_16* poly = (SPRT_16*)polyTag;

		AddSplit(semiTrans, true);

		GrVertex* firstVertex = &g_vertexBuffer[g_vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16, gteIndex);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		g_vertexIndex += 6;

#if defined(DEBUG_POLY_COUNT)
		polygon_count++;
#endif
		return 3;
	}
	}
	return 0;
}

static int ProcessDrawEnv(P_TAG* polyTag)
{
	const u_int* codePtr = (u_int*)&polyTag->pad0;
	int processedLongs = 0;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u_int code = codePtr[i];
		const int primSubType = code >> 24 & 0x0F;

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			activeDrawEnv.dfe = 1; // Force dfe=1: PSX dfe only controls display-during-draw for interlace, not rendering target
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w -= activeDrawEnv.clip.x;
			activeDrawEnv.clip.h -= activeDrawEnv.clip.y;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			// TODO
			activeDrawEnv.ofs[0] = code & 2047;
			activeDrawEnv.ofs[1] = (code >> 11) & 2047;
			break;
		}
		case 0x6:
		{
			eprintf("Mask setting: %08x\n", code);
			//MaskSetOR = (*cb & 1) ? 0x8000 : 0x0000;
			//MaskEvalAND = (*cb & 2) ? 0x8000 : 0x0000;
			break;
		}
		case 0:
			// proceed to next primitive tag — but consume the rest of the
			// declared packet length so the caller's
			//   currentPacket += (primLength + P_LEN) * 4
			// advance lands at endPacket. Returning the partial count made
			// currentPacket short of endPacket; the leftover bytes (zero
			// padding) then got mis-parsed as a fresh prim with code=0x00 /
			// primLength=3, which overshot endPacket by 16 bytes (exactly
			// the diff=-16 in the OT-PRIM log) and started chasing wild
			// next-pointers — repro: handgun fire, stack
			// ParsePrimitivesLinkedList+0xae -> DrawOTag -> GsDrawOt.
			return polyTag->len;
		}
		++processedLongs;
	}

	return processedLongs;
}

static int ProcessPsyXPrims(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX* psytex = (DR_PSYX_TEX*)polyTag;
		overrideTexture = psytex->code[0] & 0xFFFFFF;
		overrideTextureWidth = psytex->code[1] & 0xFFF;
		overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom texture packet
		DR_PSYX_DBGMARKER* psydbg = (DR_PSYX_DBGMARKER*)polyTag;
		currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG* polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE* drmove = (DR_MOVE*)polyTag;

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drmove->code[2];
			*(uint*)&rect.w = *(uint*)&drmove->code[4];

			MoveImage(&rect, x, y);
			primLength = 5;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD* drload = (DR_LOAD*)polyTag;

			RECT16 rect;
			*(uint*)&rect.x = *(uint*)&drload->code[1];
			*(uint*)&rect.w = *(uint*)&drload->code[2];

			LoadImage(&rect, (u_long*)drload->p);
			//Emulator_UpdateVRAM();			// FIXME: should it be updated immediately?

			// FIXME: is there othercommands?
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
	//default:
	//	eprinterr("got %0x primitive\n", primType);
	}

	if(primLength == 0)
	{
		eprinterr("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}
