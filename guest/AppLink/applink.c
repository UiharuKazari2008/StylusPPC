/*
 * applink.exe: one copy per Android app (and per pinned Android shortcut) in Windows Mobile's
 * Start > Programs > Android Apps. PocketStylus writes the copies to the storage card's .android
 * folder (\Storage Card\.android\<id>_<ver>.exe), each with the app's icon in its resources, and the
 * helper points a Start menu shortcut at each one (guest/Helper/AndroidApps.cs).
 *
 * Tapping one asks the phone app to open the Android app: this writes "android launch <id>_<ver>"
 * (its own file name) to the mailbox next to it, <card>\.wmdroid\out\, and exits at once. The app
 * reads the mailbox every second (GuestBridge.poll) and starts the app or shortcut (AndroidApps.kt).
 *
 * Built by tools/make-applink.py the same way as autorun.exe (tools/make-autorun.py), with a
 * resource section holding a placeholder icon whose pixels the app replaces per copy.
 */

typedef __WCHAR_TYPE__ WCHAR;   /* 16-bit: built with -fshort-wchar */
typedef unsigned int DWORD;
typedef int BOOL;
typedef void *HANDLE;

#define IMPORT(ret, name, args) __attribute__((section(".iat"), used)) ret (*name) args = 0;
IMPORT(DWORD, GetModuleFileNameW, (HANDLE module, WCHAR *buf, DWORD size))
IMPORT(HANDLE, CreateFileW, (const WCHAR *path, DWORD access, DWORD share, void *security, DWORD disposition,
                             DWORD flags, HANDLE templ))
IMPORT(BOOL, WriteFile, (HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped))
IMPORT(BOOL, CloseHandle, (HANDLE h))
IMPORT(BOOL, MoveFileW, (const WCHAR *from, const WCHAR *to))
IMPORT(DWORD, GetTickCount, (void))

#define MAXPATH 260

void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static unsigned wlen(const WCHAR *s) { unsigned n = 0; while (s[n]) n++; return n; }

static void wcat(WCHAR *d, const WCHAR *s)
{
    unsigned n = wlen(d);
    while (*s && n < MAXPATH - 1) d[n++] = *s++;
    d[n] = 0;
}

/* Cuts s at its last backslash: "\a\b\c" -> "\a\b". */
static void parent(WCHAR *s)
{
    unsigned n = wlen(s);
    while (n > 0 && s[n - 1] != '\\') n--;
    s[n > 0 ? n - 1 : 0] = 0;
}

/* The Windows CE loader calls an EXE's entry point as WinMain(instance, 0, arguments, show). */
int start(HANDLE instance, HANDLE prev, WCHAR *cmd, int show)
{
    (void)prev; (void)cmd; (void)show;
    WCHAR self[MAXPATH], tmp[MAXPATH], msg[MAXPATH];
    DWORD n = GetModuleFileNameW(instance, self, MAXPATH);
    if (n == 0 || n >= MAXPATH) return 1;
    /* The message: "android launch " + the file name without its extension (ASCII). */
    static const char VERB[] = "android launch ";
    char text[96];
    unsigned len = 0, i = n, dot = n;
    while (i > 0 && self[i - 1] != '\\') i--;
    for (unsigned j = i; j < n; j++) if (self[j] == '.') dot = j;
    for (const char *v = VERB; *v; v++) text[len++] = *v;
    for (unsigned j = i; j < dot && len < sizeof text; j++) text[len++] = self[j] < 0x80 ? (char)self[j] : '_';
    /* <card>\.android\x.exe -> <card>\.wmdroid\out\launch-<tick>.msg */
    parent(self);
    parent(self);
    tmp[0] = 0;
    wcat(tmp, self);
    wcat(tmp, L"\\.wmdroid\\out\\launch-");
    WCHAR num[9];
    DWORD t = GetTickCount();
    for (int k = 0; k < 8; k++) num[k] = (WCHAR)"0123456789abcdef"[(t >> (28 - 4 * k)) & 15];
    num[8] = 0;
    wcat(tmp, num);
    msg[0] = 0;
    wcat(msg, tmp);
    wcat(tmp, L".tmp");
    wcat(msg, L".msg");
    HANDLE f = CreateFileW(tmp, 0x40000000 /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
    if (f == (HANDLE)-1) return 1;
    DWORD done;
    WriteFile(f, text, len, &done, 0);
    CloseHandle(f);
    MoveFileW(tmp, msg);
    return 0;
}
