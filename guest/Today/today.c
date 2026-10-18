/*
 * InfraToday.dll: a classic Today screen item (not Titanium) showing how many people are in this
 * phone's InfraWAN room. The shell loads it from HKLM\Software\Microsoft\Today\Items\InfraWAN and
 * calls InitializeCustomItem at ordinal 240 (todaycmn.h).
 *
 * While the item is on screen it writes "lobby today" into the helper mailbox, the same way
 * InfraLobby does, and reads \Storage Card\.wmdroid\lobby.txt. A tap opens InfraLobby.
 * Built by tools/make-today.py.
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
IMPORT(coredll, CreateProcessW)
IMPORT(coredll, CloseHandle)
IMPORT(coredll, CreateFileW)
IMPORT(coredll, ReadFile)
IMPORT(coredll, WriteFile)
IMPORT(coredll, CreateDirectoryW)
IMPORT(coredll, MoveFileW)
IMPORT(coredll, GetFileAttributesW)
IMPORT(coredll, GetTickCount)
END(coredll)

#define WM_DESTROY 0x0002
#define WM_PAINT 0x000F
#define WM_ERASEBKGND 0x0014
#define WM_LBUTTONUP 0x0202
#define WM_USER 0x0400
#define WM_TODAYCUSTOM_CLEARCACHE (WM_USER + 242)
#define WM_TODAYCUSTOM_QUERYREFRESHCACHE (WM_USER + 243)
#define WM_TODAYCUSTOM_RECEIVEDSELECTION (WM_USER + 244)
#define WM_TODAYCUSTOM_LOSTSELECTION (WM_USER + 245)
#define WM_TODAYCUSTOM_ACTION (WM_USER + 247)
#define TODAYM_GETCOLOR (WM_USER + 100)
#define TODAYM_DRAWWATERMARK (WM_USER + 101)
#define TODAYCOLOR_TEXT 0x10000004
#define TODAYCOLOR_HIGHLIGHTEDTEXT 0x10000023
#define WS_VISIBLE 0x10000000
#define WS_CHILD 0x40000000
#define GENERIC_READ 0x80000000
#define GENERIC_WRITE 0x40000000
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define FILE_SHARE_READ 1
#define FILE_ATTRIBUTE_NORMAL 0x80
#define TRANSPARENT 1
#define DT_VCENTER 0x0004
#define DT_SINGLELINE 0x0020
#define LOGPIXELSY 90

typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { HDC hdc; int erase; RECT rc; int restore; int incUpdate; unsigned char rgb[32]; } PAINTSTRUCT;
typedef struct { HDC hdc; RECT rc; HWND hwnd; } TODAYDRAWWATERMARKINFO;
typedef long (*WNDPROC)(HWND, UINT, UINT, LONG);

/* todaycmn.h TODAYLISTITEM. cyp is the height the shell uses; fEnabled is the registry flag. */
typedef struct {
    WCHAR name[32];
    DWORD kind, order, cyp, enabled, options, flags;
    WCHAR dll[260];
    HINSTANCE inst;
    HWND hwnd;
    DWORD sizeOnDraw; /* todaycmn.h fSizeOnDraw, after hwndCustom */
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
static int (*const pCreateProcessW)(const WCHAR *, WCHAR *, void *, void *, int, DWORD, void *, const WCHAR *, void *, void *) = (void *)CreateProcessW;
static int (*const pCloseHandle)(HANDLE) = (void *)CloseHandle;
static HANDLE (*const pCreateFileW)(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE) = (void *)CreateFileW;
static int (*const pReadFile)(HANDLE, void *, DWORD, DWORD *, void *) = (void *)ReadFile;
static int (*const pWriteFile)(HANDLE, const void *, DWORD, DWORD *, void *) = (void *)WriteFile;
static int (*const pCreateDirectoryW)(const WCHAR *, void *) = (void *)CreateDirectoryW;
static int (*const pMoveFileW)(const WCHAR *, const WCHAR *) = (void *)MoveFileW;
static DWORD (*const pGetFileAttributesW)(const WCHAR *) = (void *)GetFileAttributesW;
static DWORD (*const pGetTickCount)(void) = (void *)GetTickCount;

static const WCHAR itemClass[] = L"InfraToday4";
static HINSTANCE instance;
static int dpi = 96;
static int selected;
static int height;
static WCHAR label[48] = L"InfraWAN";
static WCHAR box[40];
static DWORD seq;

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

/* "lobby today" keeps the phone in the room; "lobby today stop" leaves unless InfraLobby is open. */
static void tell(const char *msg)
{
    WCHAR dir[64], tmp[80], dest[80];
    DWORD tick, wrote = 0;
    int i, n = 0;
    HANDLE h;
    choose_box();
    wcopy(dir, box, 64);
    wcat(dir, L"\\out", 64);
    pCreateDirectoryW(box, 0);
    pCreateDirectoryW(dir, 0);
    tick = pGetTickCount() ^ (seq++ << 20);
    wcopy(tmp, dir, 80);
    wcat(tmp, L"\\t", 80);
    n = 0;
    while (tmp[n]) n++;
    for (i = 7; i >= 0; i--) {
        int dgt = (tick >> (i * 4)) & 15;
        tmp[n++] = (WCHAR)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10);
    }
    tmp[n] = 0;
    wcat(tmp, L".tmp", 80);
    h = pCreateFileW(tmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == (HANDLE)-1) return;
    while (msg[wrote]) wrote++;
    pWriteFile(h, msg, wrote, &wrote, 0);
    pCloseHandle(h);
    wcopy(dest, dir, 80);
    wcat(dest, L"\\t", 80);
    n = 0;
    while (dest[n]) n++;
    for (i = 7; i >= 0; i--) {
        int dgt = (tick >> (i * 4)) & 15;
        dest[n++] = (WCHAR)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10);
    }
    dest[n] = 0;
    wcat(dest, L".msg", 80);
    pMoveFileW(tmp, dest);
}

static void append_num(WCHAR *d, int cap, int v)
{
    WCHAR rev[8];
    int n = 0, i, len = 0;
    if (v < 0) v = 0;
    do { rev[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v && n < 8);
    while (d[len]) len++;
    for (i = n - 1; i >= 0 && len + 1 < cap; i--) d[len++] = rev[i];
    d[len] = 0;
}

static int starts(const char *s, const char *p)
{
    while (*p && *s == *p) { s++; p++; }
    return *p == 0;
}

/* lobby.txt is UTF-8 key=value lines (InfraredBridge.writeLobby). */
static void read_lobby(void)
{
    WCHAR path[64];
    char buf[2048], st[24];
    DWORD n = 0;
    int up = 0, peers = 0, i;
    const char *status = "";
    HANDLE h;
    choose_box();
    wcopy(path, box, 64);
    wcat(path, L"\\lobby.txt", 64);
    h = pCreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == (HANDLE)-1) { wcopy(label, L"InfraWAN", 48); return; }
    pReadFile(h, buf, sizeof buf - 1, &n, 0);
    pCloseHandle(h);
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    buf[n] = 0;
    st[0] = 0;
    for (i = 0; i < (int)n; ) {
        int start = i, k;
        while (i < (int)n && buf[i] != '\n' && buf[i] != '\r') i++;
        buf[i] = 0;
        if (starts(buf + start, "up=")) up = buf[start + 3] == '1';
        else if (starts(buf + start, "peer=") && buf[start + 5]) peers++;
        else if (starts(buf + start, "status=")) {
            for (k = 0; buf[start + 7 + k] && k < 23; k++) st[k] = buf[start + 7 + k];
            st[k] = 0;
            status = st;
        }
        if (i < (int)n) i++;
        if (i < (int)n && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
    wcopy(label, L"InfraWAN - ", 48);
    if (!up) {
        if (starts(status, "Connecting")) wcat(label, L"connecting", 48);
        else if (starts(status, "Set the")) wcat(label, L"not set up", 48);
        else wcat(label, L"offline", 48);
    } else {
        int people = peers + 1;   /* this phone is in the room too */
        append_num(label, 48, people);
        wcat(label, people == 1 ? L" person" : L" people", 48);
    }
}

static void open_lobby(void)
{
    DWORD pi[4];
    static const WCHAR exe[] = L"\\Windows\\InfraLobby.exe";
    pi[0] = pi[1] = pi[2] = pi[3] = 0;
    if (pCreateProcessW(exe, 0, 0, 0, 0, 0, 0, 0, 0, pi)) {
        if (pi[0]) pCloseHandle((HANDLE)pi[0]);
        if (pi[1]) pCloseHandle((HANDLE)pi[1]);
    }
}

static int line_height(void)
{
    if (height) return height;
    {
        HDC hdc = pGetDC(0);
        int v = hdc ? pGetDeviceCaps(hdc, LOGPIXELSY) : 96;
        if (hdc) pReleaseDC(0, hdc);
        if (v < 96) v = 96;
        dpi = v;
    }
    height = 22 * dpi / 96;
    if (height < 18) height = 18;
    return height;
}

static long wndproc(HWND hwnd, UINT msg, UINT wParam, LONG lParam)
{
    switch (msg) {
    case WM_TODAYCUSTOM_QUERYREFRESHCACHE: {
        WCHAR before[48];
        /* TRUE makes the shell rebuild every Today row, which is the flicker. */
        wcopy(before, label, 48);
        tell("lobby today");
        read_lobby();
        if (!wsame(before, label)) pInvalidateRect(hwnd, 0, 0);
        return 0;
    }
    case WM_TODAYCUSTOM_CLEARCACHE:
        return 0;
    case WM_DESTROY:
        tell("lobby today stop");
        return 0;
    case WM_TODAYCUSTOM_RECEIVEDSELECTION:
        selected = 1;
        pInvalidateRect(hwnd, 0, 1);
        return 0;
    case WM_TODAYCUSTOM_LOSTSELECTION:
        selected = 0;
        pInvalidateRect(hwnd, 0, 1);
        return 0;
    case WM_LBUTTONUP:
    case WM_TODAYCUSTOM_ACTION:
        open_lobby();
        return 0;
    case WM_ERASEBKGND:
        /* Erasing through the parent redraws the whole Today screen. Paint draws the row. */
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        TODAYDRAWWATERMARKINFO dwi;
        HDC hdc = pBeginPaint(hwnd, &ps);
        RECT rc;
        DWORD color = pSendMessageW(pGetParent(hwnd), TODAYM_GETCOLOR,
            selected ? TODAYCOLOR_HIGHLIGHTEDTEXT : TODAYCOLOR_TEXT, 0);
        pGetClientRect(hwnd, &rc);
        dwi.hdc = hdc;
        dwi.hwnd = hwnd;
        dwi.rc = rc;
        pSendMessageW(pGetParent(hwnd), TODAYM_DRAWWATERMARK, 0, (LONG)&dwi);
        rc.left += 4 * dpi / 96;
        pSetBkMode(hdc, TRANSPARENT);
        pSetTextColor(hdc, color);
        pDrawTextW(hdc, label, -1, &rc, DT_VCENTER | DT_SINGLELINE);
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

/* Ordinal 240. The shell calls this even when the item is switched off. */
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
    h = line_height();
    item->hwnd = pCreateWindowExW(0, itemClass, 0, WS_CHILD | WS_VISIBLE, 0, 0, pGetSystemMetrics(0), h, parent, 0, instance, 0);
    item->cyp = (DWORD)h;
    read_lobby();
    tell("lobby today");
    return item->hwnd;
}
