/*
 * ddcaps.exe: what the image's DirectDraw driver offers (research for HTC games, docs/research-hd2-port.md).
 * Writes \Storage Card\ddcaps.txt: driver and HEL DDCAPS, then whether a flipping primary
 * (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP, 1 back buffer) can be created, as Teeter needs.
 *
 * Built by tools/make-autorun.py --src guest/Tests/ddcaps.c --out <exe> (coredll imports only;
 * ddraw.dll is loaded at run time).
 */
typedef __WCHAR_TYPE__ WCHAR;
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;

#define IMPORT(ret, name, args) __attribute__((section(".iat"), used)) ret (*name) args = 0;
IMPORT(HANDLE, LoadLibraryW, (const WCHAR *name))
IMPORT(void *, GetProcAddressW, (HANDLE module, const WCHAR *name))
IMPORT(HANDLE, CreateFileW, (const WCHAR *path, DWORD access, DWORD share, void *security, DWORD disposition,
                             DWORD flags, HANDLE templ))
IMPORT(BOOL, WriteFile, (HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped))
IMPORT(BOOL, CloseHandle, (HANDLE h))
IMPORT(HANDLE, GetForegroundWindow, (void))

void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }

static HANDLE out;

static void put(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(out, s, n, &w, 0);
}

static void hex(const char *label, DWORD v)
{
    char b[48];
    int i = 0, k;
    while (label[i]) { b[i] = label[i]; i++; }
    b[i++] = '=';
    for (k = 7; k >= 0; k--) b[i++] = "0123456789abcdef"[(v >> (k * 4)) & 15];
    b[i++] = ' ';
    b[i] = 0;
    put(b);
}

typedef struct { DWORD vtbl[64]; } VT;
typedef struct { VT *v; } IFACE;
typedef DWORD (*FN)(void *, void *, void *, void *);   /* COM method: this + up to 3 args */

/* IDirectDraw (CE): QueryInterface, AddRef, Release, CreateClipper, CreatePalette, CreateSurface,
   EnumDisplayModes, EnumSurfaces, FlipToGDISurface, GetCaps, ... SetCooperativeLevel ... */
enum { DD_RELEASE = 2, DD_CREATESURFACE = 5, DD_GETCAPS = 9, DD_SETCOOPERATIVELEVEL = 17 };

static void dump(const char *name, DWORD *caps)
{
    static const char *const labels[] = { "size", "vidtotal", "vidfree", "vidstride", "ddscaps", "fourcc", "palcaps",
                                          "bltcaps", "ckeycaps", "alphacaps" };
    int i;
    put(name); put(": ");
    for (i = 0; i < 10; i++) hex(labels[i], caps[i]);
    hex("overlaycaps", caps[18]);
    hex("misccaps", caps[28]);
    put("\r\n");
}

int start(void)
{
    static const WCHAR path[] = L"\\Storage Card\\ddcaps.txt";
    DWORD drv[32], hel[32], sd[27];
    IFACE *dd = 0, *surf = 0;
    HANDLE lib;
    DWORD (*create)(void *, IFACE **, void *);
    DWORD hr;

    out = CreateFileW(path, 0x40000000, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    lib = LoadLibraryW(L"ddraw.dll");
    create = (DWORD (*)(void *, IFACE **, void *))GetProcAddressW(lib, L"DirectDrawCreate");
    if (!create) { put("no DirectDrawCreate\r\n"); CloseHandle(out); return 1; }
    hr = create(0, &dd, 0);
    hex("DirectDrawCreate", hr); put("\r\n");
    if (hr || !dd) { CloseHandle(out); return 1; }
    memset(drv, 0, sizeof drv); memset(hel, 0, sizeof hel);
    drv[0] = hel[0] = 0x70;
    hr = ((FN)dd->v->vtbl[DD_GETCAPS])(dd, drv, hel, 0);
    hex("GetCaps", hr); put("\r\n");
    dump("driver", drv);
    dump("hel", hel);
    /* exclusive full screen (DDSCL_FULLSCREEN 1), then a flipping primary with one back buffer */
    hr = ((FN)dd->v->vtbl[DD_SETCOOPERATIVELEVEL])(dd, GetForegroundWindow(), (void *)1, 0);
    hex("SetCooperativeLevel(FULLSCREEN)", hr); put("\r\n");
    memset(sd, 0, sizeof sd);
    sd[0] = 0x6C;                       /* DDSURFACEDESC size */
    sd[1] = 0x1 | 0x20;                 /* DDSD_CAPS | DDSD_BACKBUFFERCOUNT */
    sd[6] = 1;                          /* dwBackBufferCount */
    sd[25] = 0x40 | 0x4;                /* ddsCaps: PRIMARYSURFACE | FLIP */
    hr = ((FN)dd->v->vtbl[DD_CREATESURFACE])(dd, sd, &surf, 0);
    hex("CreateSurface(primary+flip)", hr); put("\r\n");
    if (surf) ((FN)surf->v->vtbl[DD_RELEASE])(surf, 0, 0, 0);
    sd[1] = 0x1;
    sd[6] = 0;
    sd[25] = 0x40;
    surf = 0;
    hr = ((FN)dd->v->vtbl[DD_CREATESURFACE])(dd, sd, &surf, 0);
    hex("CreateSurface(primary)", hr); put("\r\n");
    if (surf) ((FN)surf->v->vtbl[DD_RELEASE])(surf, 0, 0, 0);
    ((FN)dd->v->vtbl[DD_RELEASE])(dd, 0, 0, 0);
    CloseHandle(out);
    return 0;
}
