/*
 * SkyToday.dll: a classic Today screen item (WM 6.1, not Titanium) for today's weather.
 * The shell loads it from HKLM\Software\Microsoft\Today\Items\Weather and calls
 * InitializeCustomItem at ordinal 240 (todaycmn.h).
 *
 * Windows Mobile fetches the forecast (guest/Helper/Weather.cs) and writes
 * \Storage Card\.wmdroid\weather.txt. This item only draws that file.
 * Built by tools/make-weather.py.
 */

typedef unsigned int DWORD;
typedef unsigned int UINT;
typedef unsigned short WCHAR;
typedef int BOOL;
typedef int LONG;
typedef void *HANDLE;
typedef void *HWND;
typedef void *HDC;
typedef void *HINSTANCE;

#define IMPORT(dll, name) \
    __attribute__((section(".iat"), used)) void *iat_##dll##_##name = 0; \
    __attribute__((naked)) void name(void) { \
        __asm__ volatile("ldr r12, 1f\n\tldr r12, [r12]\n\tbx r12\n1:\t.word iat_" #dll "_" #name); }
#define END(dll) __attribute__((section(".iat"), used)) void *iat_##dll##__end = 0;

IMPORT(coredll, RegisterClassW)
IMPORT(coredll, CreateWindowExW)
IMPORT(coredll, DefWindowProcW)
IMPORT(coredll, BeginPaint)
IMPORT(coredll, EndPaint)
IMPORT(coredll, GetClientRect)
IMPORT(coredll, DrawTextW)
IMPORT(coredll, SetTextColor)
IMPORT(coredll, SetBkMode)
IMPORT(coredll, InvalidateRect)
IMPORT(coredll, GetDC)
IMPORT(coredll, ReleaseDC)
IMPORT(coredll, GetDeviceCaps)
IMPORT(coredll, GetParent)
IMPORT(coredll, SendMessageW)
IMPORT(coredll, GetSystemMetrics)
IMPORT(coredll, CreateFileW)
IMPORT(coredll, ReadFile)
IMPORT(coredll, CloseHandle)
IMPORT(coredll, GetFileAttributesW)
END(coredll)

#define WM_PAINT 0x000F
#define WM_ERASEBKGND 0x0014
#define WM_USER 0x0400
#define WM_TODAYCUSTOM_CLEARCACHE (WM_USER + 242)
#define WM_TODAYCUSTOM_QUERYREFRESHCACHE (WM_USER + 243)
#define WM_TODAYCUSTOM_RECEIVEDSELECTION (WM_USER + 244)
#define WM_TODAYCUSTOM_LOSTSELECTION (WM_USER + 245)
#define TODAYM_GETCOLOR (WM_USER + 100)
#define TODAYM_DRAWWATERMARK (WM_USER + 101)
#define TODAYCOLOR_TEXT 0x10000004
#define TODAYCOLOR_HIGHLIGHTEDTEXT 0x10000023
#define WS_VISIBLE 0x10000000
#define WS_CHILD 0x40000000
#define GENERIC_READ 0x80000000
#define OPEN_EXISTING 3
#define FILE_SHARE_READ 1
#define FILE_ATTRIBUTE_NORMAL 0x80
#define TRANSPARENT 1
#define DT_NOPREFIX 0x0800
#define DT_END_ELLIPSIS 0x8000
#define DT_VCENTER 0x0004
#define DT_SINGLELINE 0x0020
#define LOGPIXELSY 90

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { HDC hdc; int erase; RECT rc; int restore; int incUpdate; unsigned char rgb[32]; } PAINTSTRUCT;
typedef struct { HDC hdc; RECT rc; HWND hwnd; } TODAYDRAWWATERMARKINFO;
typedef long (*WNDPROC)(HWND, UINT, UINT, LONG);

typedef struct {
    WCHAR name[32];
    DWORD kind, order, cyp, enabled, options, flags;
    WCHAR dll[260];
    HINSTANCE inst;
    HWND hwnd;
    DWORD sizeOnDraw;
    unsigned char *cache;
    DWORD cacheBytes;
} TODAYLISTITEM;

static unsigned short (*const pRegisterClassW)(const void *) = (void *)RegisterClassW;
static HWND (*const pCreateWindowExW)(DWORD, const WCHAR *, const WCHAR *, DWORD, int, int, int, int, HWND, void *, HINSTANCE, void *) = (void *)CreateWindowExW;
static long (*const pDefWindowProcW)(HWND, UINT, UINT, LONG) = (void *)DefWindowProcW;
static HDC (*const pBeginPaint)(HWND, PAINTSTRUCT *) = (void *)BeginPaint;
static int (*const pEndPaint)(HWND, const PAINTSTRUCT *) = (void *)EndPaint;
static int (*const pGetClientRect)(HWND, RECT *) = (void *)GetClientRect;
static int (*const pDrawTextW)(HDC, const WCHAR *, int, RECT *, UINT) = (void *)DrawTextW;
static DWORD (*const pSetTextColor)(HDC, DWORD) = (void *)SetTextColor;
static int (*const pSetBkMode)(HDC, int) = (void *)SetBkMode;
static int (*const pInvalidateRect)(HWND, const RECT *, int) = (void *)InvalidateRect;
static HDC (*const pGetDC)(HWND) = (void *)GetDC;
static int (*const pReleaseDC)(HWND, HDC) = (void *)ReleaseDC;
static int (*const pGetDeviceCaps)(HDC, int) = (void *)GetDeviceCaps;
static HWND (*const pGetParent)(HWND) = (void *)GetParent;
static long (*const pSendMessageW)(HWND, UINT, UINT, LONG) = (void *)SendMessageW;
static int (*const pGetSystemMetrics)(int) = (void *)GetSystemMetrics;
static HANDLE (*const pCreateFileW)(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) = (void *)CreateFileW;
static int (*const pReadFile)(HANDLE, void *, DWORD, DWORD *, void *) = (void *)ReadFile;
static int (*const pCloseHandle)(HANDLE) = (void *)CloseHandle;
static DWORD (*const pGetFileAttributesW)(const WCHAR *) = (void *)GetFileAttributesW;

static const WCHAR itemClass[] = L"SkyToday2";
static HINSTANCE instance;
static int dpi = 96;
static int selected;
static int height;
static WCHAR line1[80];
static WCHAR line2[80];
static WCHAR box[40];

static void wcopy(WCHAR *d, const WCHAR *s, int cap)
{
    int i = 0;
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void wcat(WCHAR *d, const WCHAR *s, int cap)
{
    int n = 0, i = 0;
    while (d[n]) n++;
    while (s[i] && n + 1 < cap) d[n++] = s[i++];
    d[n] = 0;
}

static int wsame(const WCHAR *a, const WCHAR *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void choose_box(void)
{
    static const WCHAR main[] = L"\\Storage Card\\.wmdroid";
    static const WCHAR alt[] = L"\\Storage Card2\\.wmdroid";
    if (box[0] && pGetFileAttributesW(box) != 0xFFFFFFFFu) return;
    if (pGetFileAttributesW(main) != 0xFFFFFFFFu) wcopy(box, main, 40);
    else if (pGetFileAttributesW(alt) != 0xFFFFFFFFu) wcopy(box, alt, 40);
    else wcopy(box, main, 40);
}

static void utf8_to_wide(const char *s, WCHAR *d, int cap)
{
    int n = 0;
    if (!s) { if (cap > 0) d[0] = 0; return; }
    while (*s && n + 1 < cap) {
        unsigned int c = (unsigned char)*s++;
        unsigned int cp;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0 && ((unsigned char)*s & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (*s++ & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && ((unsigned char)s[0] & 0xC0) == 0x80 && ((unsigned char)s[1] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else {
            cp = '?';
            while (((unsigned char)*s & 0xC0) == 0x80) s++;
        }
        d[n++] = (WCHAR)(cp > 0xFFFF ? '?' : cp);
    }
    d[n] = 0;
}

static void add_ch(WCHAR *d, int cap, WCHAR c)
{
    int n = 0;
    while (d[n]) n++;
    if (n + 1 < cap) { d[n] = c; d[n + 1] = 0; }
}

static void add_utf8(WCHAR *d, int cap, const char *s)
{
    WCHAR tmp[80];
    utf8_to_wide(s, tmp, 80);
    wcat(d, tmp, cap);
}

static int starts_key(const char *line, const char *key)
{
    while (*key && *line == *key) { line++; key++; }
    return *key == 0 && *line == '=';
}

static const char *after_eq(const char *line)
{
    while (*line && *line != '=') line++;
    return *line == '=' ? line + 1 : line;
}

static void take(char *d, int cap, const char *s)
{
    int i = 0;
    while (s[i] && s[i] != '\r' && s[i] != '\n' && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* weather.txt is UTF-8 key=value lines written by guest/Helper/Weather.cs. */
static void read_weather(void)
{
    WCHAR path[64];
    char buf[2048];
    char place[64], temp[16], unit[8], high[16], low[16], text[48], status[64];
    DWORD n = 0;
    int i, ok = 0;
    HANDLE h;
    place[0] = temp[0] = unit[0] = high[0] = low[0] = text[0] = status[0] = 0;
    choose_box();
    wcopy(path, box, 64);
    wcat(path, L"\\weather.txt", 64);
    h = pCreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == (HANDLE)-1) {
        if (line1[0]) return;
        wcopy(line1, L"Weather", 80);
        wcopy(line2, L"Waiting", 80);
        return;
    }
    pReadFile(h, buf, sizeof buf - 1, &n, 0);
    pCloseHandle(h);
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    buf[n] = 0;
    i = (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) ? 3 : 0;
    while (i < (int)n) {
        int start = i;
        const char *v;
        while (i < (int)n && buf[i] != '\n' && buf[i] != '\r') i++;
        buf[i] = 0;
        v = after_eq(buf + start);
        if (starts_key(buf + start, "ok")) ok = v[0] == '1';
        else if (starts_key(buf + start, "place")) take(place, 64, v);
        else if (starts_key(buf + start, "temp")) take(temp, 16, v);
        else if (starts_key(buf + start, "unit")) take(unit, 8, v);
        else if (starts_key(buf + start, "high")) take(high, 16, v);
        else if (starts_key(buf + start, "low")) take(low, 16, v);
        else if (starts_key(buf + start, "text")) take(text, 48, v);
        else if (starts_key(buf + start, "status")) take(status, 64, v);
        if (i < (int)n) i++;
        if (i < (int)n && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
    if (!ok) {
        wcopy(line1, L"Weather", 80);
        if (status[0]) utf8_to_wide(status, line2, 80);
        else wcopy(line2, L"Waiting", 80);
    } else {
        line1[0] = 0;
        add_utf8(line1, 80, temp);
        add_ch(line1, 80, 0x00B0);
        add_utf8(line1, 80, unit);
        if (text[0]) { wcat(line1, L"  ", 80); add_utf8(line1, 80, text); }
        line2[0] = 0;
        if (place[0]) { add_utf8(line2, 80, place); wcat(line2, L"   ", 80); }
        wcat(line2, L"H ", 80);
        add_utf8(line2, 80, high);
        add_ch(line2, 80, 0x00B0);
        wcat(line2, L"  L ", 80);
        add_utf8(line2, 80, low);
        add_ch(line2, 80, 0x00B0);
    }
}

static int changed(const WCHAR *a, const WCHAR *b)
{
    return !wsame(a, b);
}

static int line_height(void)
{
    if (!height) {
        HDC hdc = pGetDC(0);
        int v = hdc ? pGetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) pReleaseDC(0, hdc);
        if (v < 96) v = 96;
        dpi = v;
        height = 22 * dpi / 96;
        if (height < 18) height = 18;
    }
    return height;
}

static long wndproc(HWND hwnd, UINT msg, UINT wParam, LONG lParam)
{
    switch (msg) {
    case WM_TODAYCUSTOM_QUERYREFRESHCACHE: {
        WCHAR was1[80], was2[80];
        wcopy(was1, line1, 80);
        wcopy(was2, line2, 80);
        read_weather();
        if (changed(was1, line1) || changed(was2, line2)) pInvalidateRect(hwnd, 0, 0);
        return 0;
    }
    case WM_TODAYCUSTOM_CLEARCACHE:
        return 0;
    case WM_TODAYCUSTOM_RECEIVEDSELECTION:
        selected = 1;
        pInvalidateRect(hwnd, 0, 1);
        return 0;
    case WM_TODAYCUSTOM_LOSTSELECTION:
        selected = 0;
        pInvalidateRect(hwnd, 0, 1);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        TODAYDRAWWATERMARKINFO dwi;
        HDC hdc = pBeginPaint(hwnd, &ps);
        RECT rc, top, bot;
        int mid, pad;
        DWORD color = pSendMessageW(pGetParent(hwnd), TODAYM_GETCOLOR,
            selected ? TODAYCOLOR_HIGHLIGHTEDTEXT : TODAYCOLOR_TEXT, 0);
        pGetClientRect(hwnd, &rc);
        dwi.hdc = hdc;
        dwi.hwnd = hwnd;
        dwi.rc = rc;
        pSendMessageW(pGetParent(hwnd), TODAYM_DRAWWATERMARK, 0, (LONG)&dwi);
        mid = (rc.top + rc.bottom) / 2;
        pad = 4 * dpi / 96;
        top = rc;
        bot = rc;
        top.bottom = mid;
        bot.top = mid;
        top.left += pad;
        bot.left += pad;
        pSetBkMode(hdc, TRANSPARENT);
        pSetTextColor(hdc, color);
        pDrawTextW(hdc, line1, -1, &top, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        pDrawTextW(hdc, line2, -1, &bot, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        pEndPaint(hwnd, &ps);
        return 0;
    }
    }
    return pDefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL DllMain(HANDLE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == 1) instance = inst;
    return 1;
}

HWND InitializeCustomItem(TODAYLISTITEM *item, HWND parent)
{
    struct {
        UINT style;
        WNDPROC proc;
        int cbCls, cbWnd;
        HINSTANCE inst;
        void *icon, *cursor, *brush;
        const WCHAR *menu, *cls;
    } wc;
    int i, h;
    if (!item || !item->enabled) return 0;
    for (i = 0; i < (int)(sizeof wc / sizeof(int)); i++) ((int *)&wc)[i] = 0;
    wc.proc = wndproc;
    wc.inst = instance;
    wc.cls = itemClass;
    pRegisterClassW(&wc);
    h = line_height() * 2;
    item->hwnd = pCreateWindowExW(0, itemClass, 0, WS_CHILD | WS_VISIBLE, 0, 0, pGetSystemMetrics(0), h, parent, 0, instance, 0);
    item->cyp = (DWORD)h;
    read_weather();
    return item->hwnd;
}
