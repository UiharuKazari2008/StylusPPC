/*
 * WmpHook.dll: a Windows Media Player UI plug-in (IWMPPluginUI). The player loads it from
 * HKLM\Software\Microsoft\MediaPlayer\UIPlugins and passes its IWMPCore to SetCore. That is the
 * player already on screen. A WMPlayer.OCX created anywhere else is a second player, and the
 * desktop media keys AudioClass used to post never arrive here.
 *
 * The helper writes HKCU\Software\WMDroid\Media "cmd" (play, pause, next, prev, stop, "seek <ms>").
 * This writes title, artist, album, pos, dur (milliseconds), state (WMP playState) and seq.
 *
 * The home screen's music panel loads these plug-ins into the shell. A hidden window and a
 * polling thread there stop the Start menu opening, so the class object is handed out only
 * when this DLL is inside wmplayer.exe.
 */
typedef unsigned short WCHAR;
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef int HRESULT;
typedef void *HANDLE;
typedef void *HWND;

#define S_OK 0
#define E_NOINTERFACE 0x80004002
#define E_FAIL 0x80004005
#define CLASS_E_CLASSNOTAVAILABLE 0x80040111
#define VT_I4 3
#define VT_R4 4
#define VT_R8 5
#define VT_BSTR 8
#define VT_DISPATCH 9
#define VT_UNKNOWN 13
#define DISPATCH_METHOD 1
#define DISPATCH_PROPERTYGET 2
#define DISPATCH_PROPERTYPUT 4
#define DISPID_PROPERTYPUT (-3)
#define WM_APP 0x8000

typedef struct {
    DWORD data1;
    WORD data2, data3;
    unsigned char data4[8];
} GUID;

/* 16 bytes, matching VARIANT. The 8-byte payload is two words so this file never calls soft-float helpers. */
typedef struct {
    WORD vt, r1, r2, r3;
    unsigned lo, hi;
} VAR;
typedef char var_is_16_bytes[sizeof(VAR) == 16 ? 1 : -1];

typedef struct {
    VAR *args;
    int *named;
    unsigned nArgs, nNamed;
} PARAMS;

#define IMPORT(dll, name) \
    __attribute__((section(".iat"), used)) void *iat_##dll##_##name = 0; \
    __attribute__((naked)) void name(void) { \
        __asm__ volatile("ldr r12, 1f\n\tldr r12, [r12]\n\tbx r12\n1:\t.word iat_" #dll "_" #name); }
#define END(dll) __attribute__((section(".iat"), used)) void *iat_##dll##__end = 0;

IMPORT(coredll, RegisterClassW)
IMPORT(coredll, CreateWindowExW)
IMPORT(coredll, DestroyWindow)
IMPORT(coredll, DefWindowProcW)
IMPORT(coredll, GetModuleHandleW)
IMPORT(coredll, GetModuleFileNameW)
IMPORT(coredll, RegCreateKeyExW)
IMPORT(coredll, RegSetValueExW)
IMPORT(coredll, RegQueryValueExW)
IMPORT(coredll, RegDeleteValueW)
IMPORT(coredll, RegCloseKey)
IMPORT(coredll, Sleep)
IMPORT(coredll, SendMessageTimeoutW)
IMPORT(coredll, CreateThread)
END(coredll)
IMPORT(oleaut32, SysAllocString)
IMPORT(oleaut32, SysFreeString)
END(oleaut32)

static const GUID IID_NULL = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
static const GUID IID_IUnknown = {0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID IID_IDispatch = {0x00020400, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID IID_IClassFactory = {0x00000001, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
/* IWMPPluginUI {4C5E8F9F-AD3E-4bf9-9753-FCD30D6D38DD} */
static const GUID IID_Plugin = {0x4C5E8F9F, 0xAD3E, 0x4BF9, {0x97, 0x53, 0xFC, 0xD3, 0x0D, 0x6D, 0x38, 0xDD}};
/* {E7C4A11E-6B3D-4F2A-9C15-8D0E5F6A7B21} */
static const GUID CLSID_Hook = {0xE7C4A11E, 0x6B3D, 0x4F2A, {0x9C, 0x15, 0x8D, 0x0E, 0x5F, 0x6A, 0x7B, 0x21}};

static int guid_eq(const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static void var_clear(VAR *v)
{
    if (v->vt == VT_BSTR && v->lo) ((void (*)(WCHAR *))SysFreeString)((WCHAR *)v->lo);
    else if ((v->vt == VT_DISPATCH || v->vt == VT_UNKNOWN) && v->lo) {
        void *p = (void *)v->lo;
        void **vt = *(void ***)p;
        ((unsigned (*)(void *))vt[2])(p);
    }
    v->vt = 0;
    v->lo = v->hi = 0;
}

/* ARMv4 has no divide instruction. */
static unsigned udiv(unsigned n, unsigned d)
{
    unsigned q = 0, r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (n >> 31);
        n <<= 1;
        q <<= 1;
        if (r >= d) { r -= d; q |= 1; }
    }
    return q;
}

/* IEEE double bits to milliseconds. Twenty bits of significand is plenty for a track position. */
static int bits_to_ms(unsigned lo, unsigned hi)
{
    (void)lo;
    unsigned be = (hi >> 20) & 0x7FF;
    if ((hi >> 31) || be == 0 || be == 0x7FF) return 0;
    int exp = (int)be - 1023;
    unsigned prod = (0x100000u | (hi & 0xFFFFF)) * 1000u; /* sig * 1000, sig in [2^20, 2^21) */
    int sh = exp - 20;
    if (sh >= 12) return 0x7FFFFFFF;
    if (sh >= 0) return (int)(prod << sh);
    if (-sh >= 32) return 0;
    return (int)(prod >> -sh);
}

static int float_to_ms(unsigned f)
{
    unsigned be = (f >> 23) & 0xFF;
    if ((f >> 31) || be == 0 || be == 0xFF) return 0;
    int exp = (int)be - 127;
    unsigned prod = (((0x800000u | (f & 0x7FFFFF)) >> 3) * 1000u);
    int sh = exp - 20;
    if (sh >= 12) return 0x7FFFFFFF;
    if (sh >= 0) return (int)(prod << sh);
    if (-sh >= 32) return 0;
    return (int)(prod >> -sh);
}

/* Milliseconds to IEEE double bits, for controls.currentPosition. */
static void ms_to_double(int ms, unsigned *lo, unsigned *hi)
{
    *lo = *hi = 0;
    if (ms <= 0) return;
    unsigned n = (unsigned)ms;
    int up = 0;
    while (n < 0x40000000u && up < 30) { n <<= 1; up++; }
    unsigned q = udiv(n, 1000u);
    if (q == 0) return;
    int b = 0;
    while (q >= 0x200000u) { q >>= 1; b++; }
    while (q < 0x100000u) { q <<= 1; b--; }
    int exp = 20 + b - up;
    if (exp <= -1022 || exp >= 1023) return;
    *hi = ((unsigned)(exp + 1023) << 20) | (q & 0xFFFFF);
}

/* IDispatch is GetIDsOfNames (vtable 5) then Invoke (vtable 6). riid is IID_NULL. */
static int disp_invoke(void *obj, const WCHAR *name, int flags, VAR *arg, VAR *result)
{
    void **vt = *(void ***)obj;
    int id = 0;
    WCHAR *names[1];
    names[0] = (WCHAR *)name;
    HRESULT hr = ((HRESULT (*)(void *, const GUID *, WCHAR **, unsigned, DWORD, int *))vt[5])(
        obj, &IID_NULL, names, 1, 0, &id);
    if (hr < 0) return hr;
    PARAMS p;
    int named = DISPID_PROPERTYPUT;
    p.args = arg;
    p.named = flags == DISPATCH_PROPERTYPUT ? &named : 0;
    p.nArgs = arg ? 1 : 0;
    p.nNamed = flags == DISPATCH_PROPERTYPUT ? 1 : 0;
    if (result) { result->vt = 0; result->lo = result->hi = 0; }
    return ((HRESULT (*)(void *, int, const GUID *, DWORD, WORD, PARAMS *, VAR *, void *, unsigned *))vt[6])(
        obj, id, &IID_NULL, 0, (WORD)flags, &p, result, 0, 0);
}

static void *disp_get(void *obj, const WCHAR *name)
{
    VAR v;
    v.vt = 0;
    v.lo = v.hi = 0;
    if (disp_invoke(obj, name, DISPATCH_PROPERTYGET, 0, &v) < 0 || v.vt != VT_DISPATCH) {
        var_clear(&v);
        return 0;
    }
    return (void *)v.lo;
}

static int disp_int(void *obj, const WCHAR *name, int *out)
{
    VAR v;
    v.vt = 0;
    v.lo = v.hi = 0;
    if (disp_invoke(obj, name, DISPATCH_PROPERTYGET, 0, &v) < 0) return 0;
    if (v.vt == VT_I4) *out = (int)v.lo;
    else { var_clear(&v); return 0; }
    return 1;
}

static int disp_secs(void *obj, const WCHAR *name, int *ms)
{
    VAR v;
    v.vt = 0;
    v.lo = v.hi = 0;
    if (disp_invoke(obj, name, DISPATCH_PROPERTYGET, 0, &v) < 0) return 0;
    if (v.vt == VT_R8) *ms = bits_to_ms(v.lo, v.hi);
    else if (v.vt == VT_R4) *ms = float_to_ms(v.lo);
    else if (v.vt == VT_I4) *ms = (int)v.lo > 2000000 ? 0x7FFFFFFF : (int)v.lo * 1000;
    else { var_clear(&v); return 0; }
    return 1;
}

static void copy_bstr(WCHAR *dst, unsigned cap, WCHAR *src)
{
    unsigned i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void disp_text(void *obj, const WCHAR *name, WCHAR *dst, unsigned cap)
{
    VAR v;
    v.vt = 0;
    v.lo = v.hi = 0;
    dst[0] = 0;
    if (disp_invoke(obj, name, DISPATCH_PROPERTYGET, 0, &v) < 0 || v.vt != VT_BSTR) {
        var_clear(&v);
        return;
    }
    copy_bstr(dst, cap, (WCHAR *)v.lo);
    var_clear(&v);
}

static void disp_info(void *media, const WCHAR *which, WCHAR *dst, unsigned cap)
{
    VAR arg, v;
    arg.vt = VT_BSTR;
    arg.r1 = arg.r2 = arg.r3 = 0;
    arg.lo = (unsigned)((WCHAR *(*)(const WCHAR *))SysAllocString)(which);
    arg.hi = 0;
    v.vt = 0;
    v.lo = v.hi = 0;
    dst[0] = 0;
    if (arg.lo && disp_invoke(media, L"getItemInfo", DISPATCH_METHOD, &arg, &v) >= 0 && v.vt == VT_BSTR)
        copy_bstr(dst, cap, (WCHAR *)v.lo);
    var_clear(&v);
    var_clear(&arg);
}

static void *core;
static void *controls;
static HWND hwnd;
static HANDLE instance;
static HANDLE thread;
static int refs = 1;
static int last_hr;
/* WMP Mobile's pause method leaves the track playing. Stop instead, and the next play seeks back. */
static int resume_ms = -1;
static int resume_armed;

static void drop_controls(void)
{
    if (!controls) return;
    void **vt = *(void ***)controls;
    ((unsigned (*)(void *))vt[2])(controls);
    controls = 0;
}

static void *controls_of(void)
{
    if (controls) return controls;
    if (!core) return 0;
    controls = disp_get(core, L"controls");
    return controls;
}

static void seek_to(void *c, int ms)
{
    VAR arg;
    arg.vt = VT_R8;
    arg.r1 = arg.r2 = arg.r3 = 0;
    ms_to_double(ms, &arg.lo, &arg.hi);
    last_hr = disp_invoke(c, L"currentPosition", DISPATCH_PROPERTYPUT, &arg, 0);
}

static void run_cmd(const WCHAR *cmd)
{
    void *c = controls_of();
    if (!c || !cmd[0]) { last_hr = E_FAIL; return; }
    if (cmd[0] == 's' && cmd[1] == 'e') { /* seek <ms> */
        int ms = 0, i = 0;
        resume_ms = -1;
        resume_armed = 0;
        while (cmd[i] && cmd[i] != ' ') i++;
        while (cmd[i] == ' ') i++;
        while (cmd[i] >= '0' && cmd[i] <= '9') ms = ms * 10 + (cmd[i++] - '0');
        seek_to(c, ms);
        return;
    }
    if (cmd[0] == 'p' && cmd[1] == 'a') {
        int after = 0;
        last_hr = disp_invoke(c, L"pause", DISPATCH_METHOD, 0, 0);
        disp_int(core, L"playState", &after);
        /* 3 playing, 4/5 scanning, 6 buffering: pause did not take. */
        if (last_hr < 0 || after == 3 || after == 4 || after == 5 || after == 6) {
            int ms = 0;
            if (!disp_secs(c, L"currentPosition", &ms)) ms = 0;
            resume_ms = ms;
            resume_armed = 0;
            last_hr = disp_invoke(c, L"stop", DISPATCH_METHOD, 0, 0);
        }
        return;
    }
    if (cmd[0] == 'n' || cmd[0] == 's' || (cmd[0] == 'p' && cmd[1] == 'r')) {
        const WCHAR *method = cmd[0] == 'n' ? L"next" : cmd[0] == 's' ? L"stop" : L"previous";
        resume_ms = -1;
        resume_armed = 0;
        last_hr = disp_invoke(c, method, DISPATCH_METHOD, 0, 0);
        if (last_hr < 0) drop_controls();
        return;
    }
    last_hr = disp_invoke(c, L"play", DISPATCH_METHOD, 0, 0);
    if (resume_ms >= 0) resume_armed = 1;
    if (last_hr < 0) drop_controls();
}

static void set_sz(HANDLE key, const WCHAR *name, const WCHAR *text)
{
    unsigned n = 0;
    while (text[n]) n++;
    ((int (*)(HANDLE, const WCHAR *, DWORD, DWORD, const void *, DWORD))RegSetValueExW)(
        key, name, 0, 1, text, (n + 1) * 2);
}

static void set_dw(HANDLE key, const WCHAR *name, DWORD v)
{
    ((int (*)(HANDLE, const WCHAR *, DWORD, DWORD, const void *, DWORD))RegSetValueExW)(
        key, name, 0, 4, &v, 4);
}

static void publish(void)
{
    static const DWORD HKCU = 0x80000001;
    HANDLE key = 0;
    if (((int (*)(DWORD, const WCHAR *, DWORD, WCHAR *, DWORD, DWORD, void *, HANDLE *, DWORD *))RegCreateKeyExW)(
            HKCU, L"Software\\WMDroid\\Media", 0, 0, 0, 0xF003F, 0, &key, 0) != 0 || !key)
        return;
    WCHAR cmd[48];
    DWORD cb = sizeof cmd, type = 0;
    cmd[0] = 0;
    if (((int (*)(HANDLE, const WCHAR *, DWORD *, DWORD *, void *, DWORD *))RegQueryValueExW)(
            key, L"cmd", 0, &type, cmd, &cb) == 0 && cmd[0]) {
        ((int (*)(HANDLE, const WCHAR *))RegDeleteValueW)(key, L"cmd");
        run_cmd(cmd);
    }
    WCHAR title[128], artist[128], album[128];
    title[0] = artist[0] = album[0] = 0;
    int state = 0, pos = 0, dur = 0;
    if (core) {
        disp_int(core, L"playState", &state);
        void *media = disp_get(core, L"currentMedia");
        if (media) {
            disp_text(media, L"name", title, 128);
            disp_secs(media, L"duration", &dur);
            disp_info(media, L"Artist", artist, 128);
            if (!artist[0]) disp_info(media, L"Author", artist, 128);
            disp_info(media, L"WM/AlbumTitle", album, 128);
            if (!album[0]) disp_info(media, L"Album", album, 128);
            void **vt = *(void ***)media;
            ((unsigned (*)(void *))vt[2])(media);
        }
        void *c = controls_of();
        if (c) disp_secs(c, L"currentPosition", &pos);
        /* Play has reached the track: put it back where pause stopped it. */
        if (resume_armed && resume_ms >= 0 && c && (state == 3 || state == 6)) {
            int ms = resume_ms;
            resume_ms = -1;
            resume_armed = 0;
            seek_to(c, ms);
            pos = ms;
        } else if (resume_ms >= 0 && !resume_armed && (state == 0 || state == 1 || state == 8)) {
            state = 2; /* report paused, not stopped, while we are holding the position */
            pos = resume_ms;
        }
    }
    set_sz(key, L"title", title);
    set_sz(key, L"artist", artist);
    set_sz(key, L"album", album);
    set_dw(key, L"state", (DWORD)state);
    set_dw(key, L"pos", (DWORD)pos);
    set_dw(key, L"dur", (DWORD)dur);
    set_dw(key, L"err", (DWORD)last_hr);
    last_hr = 0;
    static DWORD seq;
    set_dw(key, L"seq", ++seq);
    ((int (*)(HANDLE))RegCloseKey)(key);
}

static int wndproc(HWND w, unsigned msg, unsigned wp, unsigned lp)
{
    if (msg == WM_APP) { publish(); return 1; }
    return ((int (*)(HWND, unsigned, unsigned, unsigned))DefWindowProcW)(w, msg, wp, lp);
}

/* Runs publish on the thread that created the window (the player's), so IWMPCore stays on its thread. */
static int worker(void *arg)
{
    (void)arg;
    for (;;) {
        ((void (*)(unsigned))Sleep)(500);
        if (!core) continue;
        if (hwnd) {
            DWORD result = 0;
            /* Runs publish on the player's thread. A thread that is not pumping times out and we try once from here. */
            int ok = ((int (*)(HWND, unsigned, unsigned, unsigned, unsigned, unsigned, DWORD *))SendMessageTimeoutW)(
                hwnd, WM_APP, 0, 0, 2, 300, &result);
            if (!ok && core) publish();
        } else if (core) publish();
    }
    return 0;
}

/* The shell hosts the same plug-in for the home screen's music panel. Only wmplayer.exe may run it. */
static int in_player(void)
{
    WCHAR path[260];
    unsigned n = ((unsigned (*)(HANDLE, WCHAR *, unsigned))GetModuleFileNameW)(0, path, 260);
    if (!n || n >= 260) return 0;
    path[n] = 0;
    const WCHAR *name = path;
    for (unsigned i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') name = path + i + 1;
    const WCHAR *want = L"wmplayer.exe";
    for (unsigned i = 0;; i++) {
        WCHAR a = name[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (WCHAR)(a + 32);
        if (a != b) return 0;
        if (!a) return 1;
    }
}

static void ensure_window(void)
{
    if (!in_player()) return;
    if (!hwnd) {
        struct {
            unsigned style;
            void *proc;
            int cbCls, cbWnd;
            HANDLE inst, icon, cursor, brush;
            const WCHAR *menu, *cls;
        } wc;
        wc.style = 0;
        wc.proc = (void *)wndproc;
        wc.cbCls = 0;
        wc.cbWnd = 0;
        wc.inst = instance ? instance : ((HANDLE (*)(const WCHAR *))GetModuleHandleW)(0);
        wc.icon = 0;
        wc.cursor = 0;
        wc.brush = 0;
        wc.menu = 0;
        wc.cls = L"PSNowPlay";
        ((WORD (*)(const void *))RegisterClassW)(&wc);
        /* WS_POPUP, not shown. */
        hwnd = ((HWND (*)(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int, HWND, void *, HANDLE, void *))CreateWindowExW)(
            0, L"PSNowPlay", L"", 0x80000000u, 0, 0, 1, 1, 0, 0, wc.inst, 0);
    }
    if (!thread)
        thread = ((HANDLE (*)(void *, unsigned, void *, void *, DWORD, DWORD *))CreateThread)(0, 0, (void *)worker, 0, 0, 0);
}

/* ---- IWMPPluginUI. The object is one pointer to this table (&plugin). ---- */

static HRESULT plug_qi(void *self, const GUID *iid, void **out)
{
    if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_Plugin)) {
        *out = self;
        refs++;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}
static unsigned plug_add(void *self) { (void)self; return ++refs; }
static unsigned plug_rel(void *self) { (void)self; return --refs; }

static HRESULT plug_setcore(void *self, void *p)
{
    (void)self;
    drop_controls();
    void *old = core;
    core = 0;
    if (old) {
        void **vt = *(void ***)old;
        ((unsigned (*)(void *))vt[2])(old);
    }
    if (p) {
        void **vt = *(void ***)p;
        if (((HRESULT (*)(void *, const GUID *, void **))vt[0])(p, &IID_IDispatch, &core) < 0) core = 0;
        ensure_window();
        publish();
    }
    return S_OK;
}
static HRESULT plug_create(void *self, HWND parent, HWND *out)
{
    (void)self;
    (void)parent;
    ensure_window();
    if (out) *out = hwnd;
    return S_OK;
}
static HRESULT plug_destroy(void *self)
{
    (void)self;
    if (hwnd) {
        ((int (*)(HWND))DestroyWindow)(hwnd);
        hwnd = 0;
    }
    return S_OK;
}
static HRESULT plug_page(void *self, HWND parent) { (void)self; (void)parent; return E_FAIL; }
static HRESULT plug_getprop(void *self, const WCHAR *name, VAR *out)
{
    (void)self;
    (void)name;
    if (out) { out->vt = 0; out->lo = out->hi = 0; }
    return E_FAIL;
}
static HRESULT plug_setprop(void *self, const WCHAR *name, const VAR *in)
{
    (void)self;
    (void)name;
    (void)in;
    return S_OK;
}
static HRESULT plug_accel(void *self, void *msg) { (void)self; (void)msg; return S_OK; }

static void *plug_vt[] = {
    plug_qi, plug_add, plug_rel, plug_setcore, plug_create, plug_destroy, plug_page, plug_getprop, plug_setprop, plug_accel,
};
static void *plugin = plug_vt;

static HRESULT fact_qi(void *self, const GUID *iid, void **out)
{
    if (guid_eq(iid, &IID_IUnknown) || guid_eq(iid, &IID_IClassFactory)) { *out = self; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static unsigned fact_add(void *self) { (void)self; return 1; }
static unsigned fact_rel(void *self) { (void)self; return 1; }
static HRESULT fact_create(void *self, void *outer, const GUID *iid, void **out)
{
    (void)self;
    if (outer) return E_NOINTERFACE;
    return plug_qi(&plugin, iid, out);
}
static HRESULT fact_lock(void *self, int lock) { (void)self; (void)lock; return S_OK; }

static void *fact_vt[] = { fact_qi, fact_add, fact_rel, fact_create, fact_lock };
static void *factory = fact_vt;

HRESULT DllGetClassObject(const GUID *clsid, const GUID *iid, void **out)
{
    /* Anywhere but the player (the shell's music panel) must not get a window or a thread. */
    if (!in_player() || !guid_eq(clsid, &CLSID_Hook)) return CLASS_E_CLASSNOTAVAILABLE;
    return fact_qi(&factory, iid, out);
}

HRESULT DllCanUnloadNow(void) { return 1; } /* S_FALSE: stay loaded while the player is open */

int DllMain(HANDLE inst, unsigned reason, void *reserved)
{
    (void)reserved;
    if (reason == 1) instance = inst;
    return 1;
}
