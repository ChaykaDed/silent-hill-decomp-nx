#include "PsyX_GTE.h"
#include "PsyX/PsyX_globals.h"
#include "PsyX/PsyX_public.h"

#include "psx/libgte.h"
#include "psx/gtereg.h"

#include <math.h>



GTERegisters gteRegs;

#define GTE_SF(op)			((op >> 19) & 1)
#define GTE_MX(op)			((op >> 17) & 3)
#define GTE_V(op)			((op >> 15) & 3)
#define GTE_CV(op)			((op >> 13) & 3)
#define GTE_LM(op)			((op >> 10) & 1)
#define GTE_FUNCT(op)		(op & 63)

#define gteop(code)			(code & 0x1ffffff)

#define VX(n)				(n < 3 ? gteRegs.CP2D.p[ n << 1 ].sw.l : C2_IR1)
#define VY(n)				(n < 3 ? gteRegs.CP2D.p[ n << 1 ].sw.h : C2_IR2)
#define VZ(n)				(n < 3 ? gteRegs.CP2D.p[ (n << 1) + 1 ].sw.l : C2_IR3)
#define MX11(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) ].sw.l : -C2_R << 4)
#define MX12(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) ].sw.h : C2_R << 4)
#define MX13(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 1 ].sw.l : C2_IR0)
#define MX21(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 1 ].sw.h : C2_R13)
#define MX22(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 2 ].sw.l : C2_R13)
#define MX23(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 2 ].sw.h : C2_R13)
#define MX31(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 3 ].sw.l : C2_R22)
#define MX32(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 3 ].sw.h : C2_R22)
#define MX33(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 4 ].sw.l : C2_R22)
#define CV1(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 5 ].sd : 0)
#define CV2(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 6 ].sd : 0)
#define CV3(n)				(n < 3 ? gteRegs.CP2C.p[ (n << 3) + 7 ].sd : 0)

#ifndef max
#   define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef min
#   define min(a, b) ((a) < (b) ? (a) : (b))
#endif


static int m_sf;
static long long m_mac0;
static long long m_mac3;

unsigned int gte_leadingzerocount(unsigned int lzcs) 
{
#if 0 // OLD AND SLOW WAY
	unsigned int lzcr = 0;

	if ((lzcs & 0x80000000) == 0)
		lzcs = ~lzcs;

	while ((lzcs & 0x80000000) != 0) {
		lzcr++;
		lzcs <<= 1;
	}

	return lzcr;
#endif

	if (!lzcs)
		return 32;

	// perform fast bit scan

	unsigned int lzcr = lzcs;
	static char debruijn32[32] = {
        0, 31, 9, 30, 3, 8, 13, 29, 2, 5, 7, 21, 12, 24, 28, 19,
        1, 10, 4, 14, 6, 22, 25, 20, 11, 15, 23, 26, 16, 27, 17, 18
    };

	lzcr |= lzcr >> 1;
	lzcr |= lzcr >> 2;
	lzcr |= lzcr >> 4;
	lzcr |= lzcr >> 8;
	lzcr |= lzcr >> 16;
	lzcr++;

    return debruijn32[lzcr * 0x076be629 >> 27];
}

int LIM(int value, int max, int min, unsigned int flag) {
	if (value > max) {
		C2_FLAG |= flag;
		return max;
	}
	else if (value < min) {
		C2_FLAG |= flag;
		return min;
	}

	return value;
}

#define _oB_ (gteRegs.GPR.r[_Rs_] + _Imm_)

inline long long gte_shift(long long a, int sf) {
	if (sf > 0)
		return a >> 12;
	else if (sf < 0)
		return a << 12;

	return a;
}

int BOUNDS(/*int44*/long long value, int max_flag, int min_flag) {
	if (value/*.positive_overflow()*/ > (long long)0x7ffffffffff)
		C2_FLAG |= max_flag;

	if (value/*.negative_overflow()*/ < (long long)-0x8000000000)
		C2_FLAG |= min_flag;

	return int(gte_shift(value/*.value()*/, m_sf));
}

unsigned int gte_divide(unsigned short numerator, unsigned short denominator)
{
	if (numerator < (denominator * 2))
	{
		static unsigned char table[] =
		{
			0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1, 0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
			0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5, 0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
			0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb, 0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
			0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4, 0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
			0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
			0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d, 0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
			0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
			0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d, 0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
			0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e, 0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
			0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
			0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
			0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
			0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
			0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
			0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
			0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
			0x00
		};

		int shift = gte_leadingzerocount(denominator) - 16;

		int r1 = (denominator << shift) & 0x7fff;
		int r2 = table[((r1 + 0x40) >> 7)] + 0x101;
		int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1ffff;
		unsigned int reciprocal = ((r2 * r3) + 0x80) >> 8;

		return (unsigned int)((((unsigned long long)reciprocal * (numerator << shift)) + 0x8000) >> 16);
	}

	return 0xffffffff;
}

/* Setting bits 12 & 19-22 in FLAG does not set bit 31 */

int A1(/*int44*/long long a) { return BOUNDS(a, (1 << 31) | (1 << 30), (1 << 31) | (1 << 27)); }
int A2(/*int44*/long long a) { return BOUNDS(a, (1 << 31) | (1 << 29), (1 << 31) | (1 << 26)); }
int A3(/*int44*/long long a) { m_mac3 = a; return BOUNDS(a, (1 << 31) | (1 << 28), (1 << 31) | (1 << 25)); }
int Lm_B1(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 24)); }
int Lm_B2(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 31) | (1 << 23)); }
int Lm_B3(int a, int lm) { return LIM(a, 0x7fff, -0x8000 * !lm, (1 << 22)); }

int Lm_B3_sf(long long value, int sf, int lm) {
	int value_sf = int(gte_shift(value, sf));
	int value_12 = int(gte_shift(value, 1));
	int max = 0x7fff;
	int min = 0;
	if (lm == 0)
		min = -0x8000;

	if (value_12 < -0x8000 || value_12 > 0x7fff)
		C2_FLAG |= (1 << 22);

	if (value_sf > max)
		return max;
	else if (value_sf < min)
		return min;

	return value_sf;
}

int Lm_C1(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 21)); }
int Lm_C2(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 20)); }
int Lm_C3(int a) { return LIM(a, 0x00ff, 0x0000, (1 << 19)); }
int Lm_D(long long a, int sf) { return LIM(int(gte_shift(a, sf)), 0xffff, 0x0000, (1 << 31) | (1 << 18)); }

unsigned int Lm_E(unsigned int result) {
	if (result == 0xffffffff) {
		C2_FLAG |= (1 << 31) | (1 << 17);
		return 0x1ffff;
	}

	if (result > 0x1ffff)
		return 0x1ffff;

	return result;
}

long long F(long long a) {
	m_mac0 = a;

	if (a > 0x7fffffffLL)
		C2_FLAG |= (1 << 31) | (1 << 16);

	if (a < -0x80000000LL)
		C2_FLAG |= (1 << 31) | (1 << 15);

	return a;
}

int Lm_G1(long long a) {
	if (a > 0x3ff) {
		C2_FLAG |= (1 << 31) | (1 << 14);
		return 0x3ff;
	}
	if (a < -0x400) {
		C2_FLAG |= (1 << 31) | (1 << 14);
		return -0x400;
	}

	return int(a);
}

int Lm_G2(long long a) {
	if (a > 0x3ff) {
		C2_FLAG |= (1 << 31) | (1 << 13);
		return 0x3ff;
	}

	if (a < -0x400) {
		C2_FLAG |= (1 << 31) | (1 << 13);
		return -0x400;
	}

	return int(a);
}

int Lm_G1_ia(long long a) {
	if (a > 0x3ffffff)
		return 0x3ffffff;

	if (a < -0x4000000)
		return -0x4000000;

	return int(a);
}

int Lm_G2_ia(long long a) {
	if (a > 0x3ffffff)
		return 0x3ffffff;

	if (a < -0x4000000)
		return -0x4000000;

	return int(a);
}

int Lm_H(long long value, int sf) {
	long long value_sf = gte_shift(value, sf);
	int value_12 = int(gte_shift(value, 1));
	int max = 0x1000;
	int min = 0x0000;

	if (value_sf < min || value_sf > max)
		C2_FLAG |= (1 << 12);

	if (value_12 > max)
		return max;

	if (value_12 < min)
		return min;

	return value_12;
}



/* PGXP precise screen-coord FIFO, mirrors the GTE SXY0/SXY1/SXY2 FIFO so the
 * store macros can resolve a destination address to the precise float coord
 * the GTE just produced. Updated only when g_PsxUsePgxp. */
static float s_pgxpFifoX[3], s_pgxpFifoY[3], s_pgxpFifoW[3];

/* Parallel FIFO for the per-pixel flashlight: the GTE RTPS view-space (camera-
 * space) position MAC1/MAC2/MAC3 of each projected vertex. Mirrors the PGXP
 * SXY FIFO so the store hook can resolve a destination address to the view
 * position the GTE just produced. Updated only when g_PsyX_UsePerPixelFlashlight. */
static float s_vsFifoX[3], s_vsFifoY[3], s_vsFifoZ[3];

extern "C" int g_PgxpUseUnquantizedDepth; /* defined in PsyX_GPU.cpp */
extern "C" void VShadow_Store(void* addr, float x, float y, float z); /* PsyX_GPU.cpp */

/* Called from the gte_stsxy* store macros (only when g_PsxUsePgxp): the macro
 * just wrote the integer screen coord for FIFO slot `slot` (SXY0=0, SXY1=1,
 * SXY2=2) to `addr`, so record the shadow keyed by that address, validated by
 * the integer value the macro left there (DuckStation's SWC2 hook). */
extern "C" void Shadow_Store(void* addr, float x, float y, float w, unsigned value);
extern "C" void PGXP_StoreAddr(void* addr, int slot)
{
	if ((unsigned)slot > 2u) return;
	/* The gte_stsxy* macros now call this when (g_PsxUsePgxp ||
	 * g_PsyX_UsePerPixelFlashlight), so each store is independently gated:
	 * PGXP unchanged when its flag is off, flashlight a no-op when its flag is
	 * off, both off => not called at all (byte-identical legacy path). */
	if (g_PsxUsePgxp)
		Shadow_Store(addr, s_pgxpFifoX[slot], s_pgxpFifoY[slot], s_pgxpFifoW[slot], *(unsigned*)addr);
	if (g_PsyX_UsePerPixelFlashlight)
		VShadow_Store(addr, s_vsFifoX[slot], s_vsFifoY[slot], s_vsFifoZ[slot]);
}

int GTE_RotTransPers(int idx, int lm)
{
	int h_over_sz3;

	C2_MAC1 = A1(/*int44*/(long long)((long long)C2_TRX << 12) + (C2_R11 * VX(idx)) + (C2_R12 * VY(idx)) + (C2_R13 * VZ(idx)));
	C2_MAC2 = A2(/*int44*/(long long)((long long)C2_TRY << 12) + (C2_R21 * VX(idx)) + (C2_R22 * VY(idx)) + (C2_R23 * VZ(idx)));
	C2_MAC3 = A3(/*int44*/(long long)((long long)C2_TRZ << 12) + (C2_R31 * VX(idx)) + (C2_R32 * VY(idx)) + (C2_R33 * VZ(idx)));
	C2_IR1 = Lm_B1(C2_MAC1, lm);
	C2_IR2 = Lm_B2(C2_MAC2, lm);
	C2_IR3 = Lm_B3_sf(m_mac3, m_sf, lm);
	C2_SZ0 = C2_SZ1;
	C2_SZ1 = C2_SZ2;
	C2_SZ2 = C2_SZ3;
	C2_SZ3 = Lm_D(m_mac3, 1);
	h_over_sz3 = Lm_E(gte_divide(C2_H, C2_SZ3));
	C2_SXY0 = C2_SXY1;
	C2_SXY1 = C2_SXY2;
	C2_SX2 = Lm_G1(F((long long)C2_OFX + ((long long)C2_IR1 * h_over_sz3)) >> 16);
	C2_SY2 = Lm_G2(F((long long)C2_OFY + ((long long)C2_IR2 * h_over_sz3)) >> 16);

	/* PGXP: stash the full-precision projection keyed by the clamped integer screen
	 * coord the prim will store. Gated — zero cost / zero effect when PGXP is off. */
	if (g_PsxUsePgxp)
	{
		/* Project with a TRUE float divide, NOT the GTE's h_over_sz3. h_over_sz3 is the
		 * hardware UNR divide SATURATED by Lm_E to 0x1ffff (H/SZ3 capped at ~2.0) as soon
		 * as SZ3 < H/2, so geometry close to the camera got a WRONG precise coord AND was
		 * forced to affine (W=0) below. A poly straddling that threshold then renders
		 * half-perspective / half-affine and smears at the screen edge. The float divide
		 * has no cap: every in-front vertex (SZ3 > 0) projects correctly and stays on the
		 * perspective path, so the whole poly is consistent. (For SZ3 >= H/2 this is
		 * identical to the old path — only the previously-broken close case changes.)
		 * W = view-space SZ3 (only the per-vertex ratio matters; absolute scale cancels). */
		double fx, fy;
		float  pgxpW;
		if (C2_SZ3 > 0) {
			double ratio = (double)C2_H / (double)C2_SZ3;            /* H/SZ3, UNclamped */
			fx = (double)C2_OFX / 65536.0 + (double)C2_IR1 * ratio;
			fy = (double)C2_OFY / 65536.0 + (double)C2_IR2 * ratio;
			/* W for the shader's perspective divide. C2_SZ3 is the GTE's CLAMPED 16-bit
			 * depth register; two independent GTE calls for a shared edge quantize to
			 * SZ3 values 1-2 apart, so the per-vertex 1/W diverges and the perspective
			 * interpolation across the shared edge mismatches -> seam that gets more
			 * visible with distance. gte_shift(m_mac3,1) is the SAME shift Lm_D applies
			 * to make SZ3 but WITHOUT the [0,0xffff] clamp -> full precision, so coincident
			 * edges get matching W. Toggle (pgxpdepth 0/1) for A/B. */
			pgxpW = g_PgxpUseUnquantizedDepth ? (float)gte_shift(m_mac3, 1) : (float)C2_SZ3;
		} else {
			/* SZ3 == 0: at / behind the near plane, no valid projection -> affine (W=0). */
			fx = (double)C2_SX2; fy = (double)C2_SY2;
			pgxpW = 0.0f;
		}

		/* Mirror the GTE SXY FIFO with a precise FIFO so the gte_stsxy* store macros
		 * (which know the destination address but not the precise value) can record
		 * address->precise deterministically. Shift exactly as the C2_SXY shift above. */
		s_pgxpFifoX[0] = s_pgxpFifoX[1]; s_pgxpFifoX[1] = s_pgxpFifoX[2]; s_pgxpFifoX[2] = (float)fx;
		s_pgxpFifoY[0] = s_pgxpFifoY[1]; s_pgxpFifoY[1] = s_pgxpFifoY[2]; s_pgxpFifoY[2] = (float)fy;
		s_pgxpFifoW[0] = s_pgxpFifoW[1]; s_pgxpFifoW[1] = s_pgxpFifoW[2]; s_pgxpFifoW[2] = pgxpW;
	}

	/* Flashlight view-space FIFO: stash this vertex's camera-space position
	 * (RTPS MAC1/MAC2/MAC3, same units the light is pushed in). Mirrors the SXY
	 * FIFO shift above so gte_stsxy* can resolve address->view-pos. Off = no cost. */
	if (g_PsyX_UsePerPixelFlashlight)
	{
		s_vsFifoX[0] = s_vsFifoX[1]; s_vsFifoX[1] = s_vsFifoX[2]; s_vsFifoX[2] = (float)C2_MAC1;
		s_vsFifoY[0] = s_vsFifoY[1]; s_vsFifoY[1] = s_vsFifoY[2]; s_vsFifoY[2] = (float)C2_MAC2;
		s_vsFifoZ[0] = s_vsFifoZ[1]; s_vsFifoZ[1] = s_vsFifoZ[2]; s_vsFifoZ[2] = (float)C2_MAC3;
	}

	return h_over_sz3;
}

int GTE_operator(int op)
{
	int v;
	int lm;
	int cv;
	int mx;
	int h_over_sz3 = 0;

	lm = GTE_LM(gteop(op));
	m_sf = GTE_SF(gteop(op));

	C2_FLAG = 0;

	switch (GTE_FUNCT(gteop(op)))
	{
	case 0x00:
	case 0x01:
#ifdef GTE_LOG
		GTELOG("%08x RTPS", op);
#endif
		h_over_sz3 = GTE_RotTransPers(0, lm);

		C2_MAC0 = int(F((long long)C2_DQB + ((long long)C2_DQA * h_over_sz3)));
		C2_IR0 = Lm_H(m_mac0, 1);

		return 1;

	case 0x06:
#ifdef GTE_LOG
		GTELOG("%08x NCLIP", op);
#endif
		C2_MAC0 = int(F((long long)(C2_SX0 * C2_SY1) + (C2_SX1 * C2_SY2) + (C2_SX2 * C2_SY0) - (C2_SX0 * C2_SY2) - (C2_SX1 * C2_SY0) - (C2_SX2 * C2_SY1)));
		C2_FLAG = 0;
		return 1;

	case 0x0c:
#ifdef GTE_LOG
		GTELOG("%08x OP", op);
#endif

		C2_MAC1 = A1((long long)(C2_R22 * C2_IR3) - (C2_R33 * C2_IR2));
		C2_MAC2 = A2((long long)(C2_R33 * C2_IR1) - (C2_R11 * C2_IR3));
		C2_MAC3 = A3((long long)(C2_R11 * C2_IR2) - (C2_R22 * C2_IR1));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x10:
#ifdef GTE_LOG
		GTELOG("%08x DPCS", op);
#endif

		C2_MAC1 = A1((C2_R << 16) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_R << 16)), 0)));
		C2_MAC2 = A2((C2_G << 16) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_G << 16)), 0)));
		C2_MAC3 = A3((C2_B << 16) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_B << 16)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x11:
#ifdef GTE_LOG
		GTELOG("%08x INTPL", op);
#endif

		C2_MAC1 = A1((C2_IR1 << 12) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_IR1 << 12)), 0)));
		C2_MAC2 = A2((C2_IR2 << 12) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_IR2 << 12)), 0)));
		C2_MAC3 = A3((C2_IR3 << 12) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_IR3 << 12)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x12:
#ifdef GTE_LOG
		GTELOG("%08x MVMVA", op);
#endif

		mx = GTE_MX(gteop(op));
		v = GTE_V(gteop(op));
		cv = GTE_CV(gteop(op));

		switch (cv) {
		case 2:
			C2_MAC1 = A1((long long)(MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)));
			C2_MAC2 = A2((long long)(MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)));
			C2_MAC3 = A3((long long)(MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)));
			Lm_B1(A1(((long long)CV1(cv) << 12) + (MX11(mx) * VX(v))), 0);
			Lm_B2(A2(((long long)CV2(cv) << 12) + (MX21(mx) * VX(v))), 0);
			Lm_B3(A3(((long long)CV3(cv) << 12) + (MX31(mx) * VX(v))), 0);
			break;

		default:
			C2_MAC1 = A1(/*int44*/(long long)((long long)CV1(cv) << 12) + (MX11(mx) * VX(v)) + (MX12(mx) * VY(v)) + (MX13(mx) * VZ(v)));
			C2_MAC2 = A2(/*int44*/(long long)((long long)CV2(cv) << 12) + (MX21(mx) * VX(v)) + (MX22(mx) * VY(v)) + (MX23(mx) * VZ(v)));
			C2_MAC3 = A3(/*int44*/(long long)((long long)CV3(cv) << 12) + (MX31(mx) * VX(v)) + (MX32(mx) * VY(v)) + (MX33(mx) * VZ(v)));
			break;
		}

		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x13:
#ifdef GTE_LOG
		GTELOG("%08x NCDS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x14:
#ifdef GTE_LOG
		GTELOG("%08x CDP", op);
#endif

		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x16:
#ifdef GTE_LOG
		GTELOG("%08x NCDT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
			C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
			C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x1b:
#ifdef GTE_LOG
		GTELOG("%08x NCCS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1c:
#ifdef GTE_LOG
		GTELOG("%08x CC", op);
#endif

		C2_MAC1 = A1(/*int44*/(long long)(((long long)C2_RBK) << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)(((long long)C2_GBK) << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)(((long long)C2_BBK) << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1((C2_R << 4) * C2_IR1);
		C2_MAC2 = A2((C2_G << 4) * C2_IR2);
		C2_MAC3 = A3((C2_B << 4) * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x1e:
#ifdef GTE_LOG
		GTELOG("%08x NCS", op);
#endif

		C2_MAC1 = A1((long long)(C2_L11 * C2_VX0) + (C2_L12 * C2_VY0) + (C2_L13 * C2_VZ0));
		C2_MAC2 = A2((long long)(C2_L21 * C2_VX0) + (C2_L22 * C2_VY0) + (C2_L23 * C2_VZ0));
		C2_MAC3 = A3((long long)(C2_L31 * C2_VX0) + (C2_L32 * C2_VY0) + (C2_L33 * C2_VZ0));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
		C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
		C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x20:
#ifdef GTE_LOG
		GTELOG("%08x NCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x28:
#ifdef GTE_LOG
		GTELOG("%08x SQR", op);
#endif

		C2_MAC1 = A1(C2_IR1 * C2_IR1);
		C2_MAC2 = A2(C2_IR2 * C2_IR2);
		C2_MAC3 = A3(C2_IR3 * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		return 1;

	case 0x29:
#ifdef GTE_LOG
		GTELOG("%08x DPCL", op);
#endif

		C2_MAC1 = A1(((C2_R << 4) * C2_IR1) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - ((C2_R << 4) * C2_IR1)), 0)));
		C2_MAC2 = A2(((C2_G << 4) * C2_IR2) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - ((C2_G << 4) * C2_IR2)), 0)));
		C2_MAC3 = A3(((C2_B << 4) * C2_IR3) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - ((C2_B << 4) * C2_IR3)), 0)));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x2a:
#ifdef GTE_LOG
		GTELOG("%08x DPCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((C2_R0 << 16) + (C2_IR0 * Lm_B1(A1(((long long)C2_RFC << 12) - (C2_R0 << 16)), 0)));
			C2_MAC2 = A2((C2_G0 << 16) + (C2_IR0 * Lm_B2(A2(((long long)C2_GFC << 12) - (C2_G0 << 16)), 0)));
			C2_MAC3 = A3((C2_B0 << 16) + (C2_IR0 * Lm_B3(A3(((long long)C2_BFC << 12) - (C2_B0 << 16)), 0)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;

	case 0x2d:
#ifdef GTE_LOG
		GTELOG("%08x AVSZ3", op);
#endif

		C2_MAC0 = int(F((long long)(C2_ZSF3 * C2_SZ1) + (C2_ZSF3 * C2_SZ2) + (C2_ZSF3 * C2_SZ3)));
		C2_OTZ = Lm_D(m_mac0, 1);
		return 1;

	case 0x2e:
#ifdef GTE_LOG
		GTELOG("%08x AVSZ4", op);
#endif

		C2_MAC0 = int(F((long long)(C2_ZSF4 * C2_SZ0) + (C2_ZSF4 * C2_SZ1) + (C2_ZSF4 * C2_SZ2) + (C2_ZSF4 * C2_SZ3)));
		C2_OTZ = Lm_D(m_mac0, 1);
		return 1;

	case 0x30:
#ifdef GTE_LOG
		GTELOG("%08x RTPT", op);
#endif

		for (v = 0; v < 3; v++)
			h_over_sz3 = GTE_RotTransPers(v, lm);

		C2_MAC0 = int(F((long long)C2_DQB + ((long long)C2_DQA * h_over_sz3)));
		C2_IR0 = Lm_H(m_mac0, 1);
		return 1;

	case 0x3d:
#ifdef GTE_LOG
		GTELOG("%08x GPF", op);
#endif

		C2_MAC1 = A1(C2_IR0 * C2_IR1);
		C2_MAC2 = A2(C2_IR0 * C2_IR2);
		C2_MAC3 = A3(C2_IR0 * C2_IR3);
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3e:
#ifdef GTE_LOG
		GTELOG("%08x GPL", op);
#endif

		C2_MAC1 = A1(gte_shift(C2_MAC1, -m_sf) + (C2_IR0 * C2_IR1));
		C2_MAC2 = A2(gte_shift(C2_MAC2, -m_sf) + (C2_IR0 * C2_IR2));
		C2_MAC3 = A3(gte_shift(C2_MAC3, -m_sf) + (C2_IR0 * C2_IR3));
		C2_IR1 = Lm_B1(C2_MAC1, lm);
		C2_IR2 = Lm_B2(C2_MAC2, lm);
		C2_IR3 = Lm_B3(C2_MAC3, lm);
		C2_RGB0 = C2_RGB1;
		C2_RGB1 = C2_RGB2;
		C2_CD2 = C2_CODE;
		C2_R2 = Lm_C1(C2_MAC1 >> 4);
		C2_G2 = Lm_C2(C2_MAC2 >> 4);
		C2_B2 = Lm_C3(C2_MAC3 >> 4);
		return 1;

	case 0x3f:
#ifdef GTE_LOG
		GTELOG("%08x NCCT", op);
#endif

		for (v = 0; v < 3; v++) {
			C2_MAC1 = A1((long long)(C2_L11 * VX(v)) + (C2_L12 * VY(v)) + (C2_L13 * VZ(v)));
			C2_MAC2 = A2((long long)(C2_L21 * VX(v)) + (C2_L22 * VY(v)) + (C2_L23 * VZ(v)));
			C2_MAC3 = A3((long long)(C2_L31 * VX(v)) + (C2_L32 * VY(v)) + (C2_L33 * VZ(v)));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1(/*int44*/(long long)((long long)C2_RBK << 12) + (C2_LR1 * C2_IR1) + (C2_LR2 * C2_IR2) + (C2_LR3 * C2_IR3));
			C2_MAC2 = A2(/*int44*/(long long)((long long)C2_GBK << 12) + (C2_LG1 * C2_IR1) + (C2_LG2 * C2_IR2) + (C2_LG3 * C2_IR3));
			C2_MAC3 = A3(/*int44*/(long long)((long long)C2_BBK << 12) + (C2_LB1 * C2_IR1) + (C2_LB2 * C2_IR2) + (C2_LB3 * C2_IR3));
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_MAC1 = A1((C2_R << 4) * C2_IR1);
			C2_MAC2 = A2((C2_G << 4) * C2_IR2);
			C2_MAC3 = A3((C2_B << 4) * C2_IR3);
			C2_IR1 = Lm_B1(C2_MAC1, lm);
			C2_IR2 = Lm_B2(C2_MAC2, lm);
			C2_IR3 = Lm_B3(C2_MAC3, lm);
			C2_RGB0 = C2_RGB1;
			C2_RGB1 = C2_RGB2;
			C2_CD2 = C2_CODE;
			C2_R2 = Lm_C1(C2_MAC1 >> 4);
			C2_G2 = Lm_C2(C2_MAC2 >> 4);
			C2_B2 = Lm_C3(C2_MAC3 >> 4);
		}
		return 1;
	}

	return 0;
}