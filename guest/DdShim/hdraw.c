/*
 * HDRAW.dll: DirectDraw as HTC's games expect it (docs/research-hd2-port.md, "DirectDraw caps").
 *
 * HTC's games (Teeter) gate their start on the HD2 display driver's caps: a video-memory primary
 * with back buffer and flip (DDSCAPS_VIDEOMEMORY | BACKBUFFER | FLIP), blits from system memory
 * (DDBLTCAPS_READSYSMEM) and overlay flip or alpha (DDOVERLAYCAPS_FLIP | ALPHASRC). Microsoft's
 * emulator display driver offers none of these and the game says "Memory is not enough". What the
 * game then does (a plain primary, offscreen surfaces, blits to the primary) works on that driver
 * with DirectDraw's software blitter, so only the caps answer is the problem.
 *
 * DirectDrawCreate here creates the real object and hands out a wrapper whose methods forward to it;
 * GetCaps adds the caps above to the driver caps. Surfaces and everything else are the real ones.
 * The HTC app's import of DDRAW.dll is renamed to HDRAW.dll when it is installed (same length).
 *
 * Built by tools/make-hdraw.py (NDK clang freestanding ARMv4T, make-mapilib.py's PE builder).
 */
typedef __WCHAR_TYPE__ WCHAR;
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;

#define IMPORT(dll, name) \
    __attribute__((section(".iat"), used)) void *iat_##dll##_##name = 0; \
    __attribute__((naked)) void name(void) { \
        __asm__ volatile("ldr r12, 1f\n\tldr r12, [r12]\n\tbx r12\n1:\t.word iat_" #dll "_" #name); }
#define END(dll) __attribute__((section(".iat"), used)) void *iat_##dll##__end = 0;

IMPORT(coredll, LoadLibraryW)
IMPORT(coredll, GetProcAddressW)
IMPORT(coredll, LocalAlloc)
IMPORT(coredll, LocalFree)
END(coredll)

#define DDSCAPS_BACKBUFFER 0x2
#define DDSCAPS_FLIP 0x4
#define DDSCAPS_VIDEOMEMORY 0x100
#define DDBLTCAPS_READSYSMEM 0x1
#define DDOVERLAYCAPS_FLIP 0x1
#define E_FAIL 0x80004005u

/* DDCAPS (CE): size, vidmem total/free/stride, ddsCaps, fourcc, palcaps, bltcaps, ckeycaps, alphacaps,
   rops[8], overlaycaps, ... */
enum { CAPS_DDSCAPS = 4, CAPS_BLT = 7, CAPS_OVERLAY = 18 };
/* IDirectDraw (CE) */
enum { M_QI, M_ADDREF, M_RELEASE, M_CREATECLIPPER, M_CREATEPALETTE, M_CREATESURFACE, M_ENUMDISPLAYMODES,
       M_ENUMSURFACES, M_FLIPTOGDISURFACE, M_GETCAPS, M_GETDISPLAYMODE, M_GETFOURCCCODES, M_GETGDISURFACE,
       M_GETMONITORFREQUENCY, M_GETSCANLINE, M_GETVERTICALBLANKSTATUS, M_RESTOREDISPLAYMODE,
       M_SETCOOPERATIVELEVEL, M_SETDISPLAYMODE, M_WAITFORVERTICALBLANK, M_GETAVAILABLEVIDMEM,
       M_GETSURFACEFROMDC, M_RESTOREALLSURFACES, M_TESTCOOPERATIVELEVEL, M_GETDEVICEIDENTIFIER, M_COUNT };

typedef DWORD (*METHOD)(void *self, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e);
typedef struct { METHOD *vtbl; } REAL;
typedef struct { const METHOD *vtbl; REAL *real; } WRAP;

static void *(*const pLoadLibraryW)(const WCHAR *) = (void *(*)(const WCHAR *))LoadLibraryW;
static void *(*const pGetProcAddressW)(void *, const WCHAR *) = (void *(*)(void *, const WCHAR *))GetProcAddressW;
static void *(*const pLocalAlloc)(DWORD, DWORD) = (void *(*)(DWORD, DWORD))LocalAlloc;
static void *(*const pLocalFree)(void *) = (void *(*)(void *))LocalFree;

/* Every method but Release/GetCaps passes its arguments on (up to five; extra ones are ignored). */
#define FORWARD(n) \
    static DWORD fwd##n(WRAP *w, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e) \
    { return w->real->vtbl[n](w->real, a, b, c, d, e); }
FORWARD(0) FORWARD(1) FORWARD(3) FORWARD(4) FORWARD(5) FORWARD(6) FORWARD(7) FORWARD(8) FORWARD(10) FORWARD(11)
FORWARD(12) FORWARD(13) FORWARD(14) FORWARD(15) FORWARD(16) FORWARD(17) FORWARD(18) FORWARD(19) FORWARD(20)
FORWARD(21) FORWARD(22) FORWARD(23) FORWARD(24)

static DWORD release(WRAP *w, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
{
    DWORD left = w->real->vtbl[M_RELEASE](w->real, 0, 0, 0, 0, 0);
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (left == 0) pLocalFree(w);
    return left;
}

static DWORD getcaps(WRAP *w, DWORD drv, DWORD hel, DWORD c, DWORD d, DWORD e)
{
    DWORD hr = w->real->vtbl[M_GETCAPS](w->real, drv, hel, c, d, e);
    if (hr == 0 && drv) {
        DWORD *caps = (DWORD *)drv;
        caps[CAPS_DDSCAPS] |= DDSCAPS_VIDEOMEMORY | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP;
        caps[CAPS_BLT] |= DDBLTCAPS_READSYSMEM;
        caps[CAPS_OVERLAY] |= DDOVERLAYCAPS_FLIP;
    }
    return hr;
}

static const METHOD vtbl[M_COUNT] = {
    (METHOD)fwd0, (METHOD)fwd1, (METHOD)release, (METHOD)fwd3, (METHOD)fwd4, (METHOD)fwd5, (METHOD)fwd6,
    (METHOD)fwd7, (METHOD)fwd8, (METHOD)getcaps, (METHOD)fwd10, (METHOD)fwd11, (METHOD)fwd12, (METHOD)fwd13,
    (METHOD)fwd14, (METHOD)fwd15, (METHOD)fwd16, (METHOD)fwd17, (METHOD)fwd18, (METHOD)fwd19, (METHOD)fwd20,
    (METHOD)fwd21, (METHOD)fwd22, (METHOD)fwd23, (METHOD)fwd24,
};

BOOL DllMain(HANDLE inst, DWORD reason, void *reserved) { (void)inst; (void)reason; (void)reserved; return 1; }

DWORD DirectDrawCreate(void *guid, WRAP **out, void *outer)
{
    DWORD (*create)(void *, REAL **, void *);
    REAL *real = 0;
    WRAP *w;
    DWORD hr;
    if (!out) return E_FAIL;
    *out = 0;
    create = (DWORD (*)(void *, REAL **, void *))pGetProcAddressW(pLoadLibraryW(L"ddraw.dll"), L"DirectDrawCreate");
    if (!create) return E_FAIL;
    hr = create(guid, &real, outer);
    if (hr || !real) return hr ? hr : E_FAIL;
    w = pLocalAlloc(0x40 /* LPTR */, sizeof *w);
    if (!w) { real->vtbl[M_RELEASE](real, 0, 0, 0, 0, 0); return 0x8007000Eu; /* E_OUTOFMEMORY */ }
    w->vtbl = vtbl;
    w->real = real;
    *out = w;
    return 0;
}
