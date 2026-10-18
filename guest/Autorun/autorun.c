/*
 * autorun.exe: the native bootstrap PocketStylus serves as \Storage Card\2577\autorun.exe.
 *
 * Windows Mobile runs "2577\autorun.exe install" whenever the card mounts. The helper itself is a
 * .NET Compact Framework 3.5 program, which WM 6.5 has in ROM but WM 5 (CF 1.0) and WM 6.0/6.1
 * (CF 2.0) do not; started there it only shows "requires a newer version of the .NET Compact
 * Framework". So this small ARM program comes first:
 *
 *   1. If the runtime is missing, it installs one silently from the CAB next to it, copied to
 *      \Temp first (wceload deletes the CAB it installed, and the card can go away meanwhile),
 *      then "\Windows\wceload.exe /noui" on the copy, waiting for it to finish (/noui installs to
 *      the default place, the device, without asking; WM 5's wceload fails at once, E_FAIL, when
 *      also given /noaskdest or /silent).
 *      Windows Mobile 2003 is CE 4.2, so it gets .NET CF 2.0 (NETCFv2.wce4.ARMV4.cab, marker
 *      \Windows\mscoree2_0.dll). CE 5 and later get .NET CF 3.5 (NETCFv35.wm.armv4i.cab,
 *      \Windows\mscoree3_5.dll).
 *   2. It starts the real helper, helper.exe from the same folder, with the same arguments
 *      ("install", "update", ...), and exits without waiting for it.
 *
 * Instances run one after another (a named mutex), so a card re-inserted during the install waits
 * for it instead of starting a second wceload. What it does goes to \Storage Card\.wmdroid\
 * helper.log, like the helper's own log.
 *
 * Built by tools/make-autorun.py: NDK clang compiles this freestanding for ARMv4T (ARM state,
 * interworking calls into coredll), ld.lld links it at the EXE's fixed base 0x10000 and the
 * script wraps it in a Windows CE ARM PE with coredll imports by name. Every global below in
 * section .iat is one import: the script names it after the variable.
 */

typedef __WCHAR_TYPE__ WCHAR;   /* 16-bit: built with -fshort-wchar */
typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef int BOOL;
typedef void *HANDLE;

typedef struct { HANDLE process, thread; DWORD pid, tid; } PROCESS_INFORMATION;
typedef struct { WORD year, month, weekday, day, hour, minute, second, ms; } SYSTEMTIME;
typedef struct { DWORD size, major, minor, build, platform; WCHAR csd[128]; } OSVERSIONINFOW;

#define IMPORT(ret, name, args) __attribute__((section(".iat"), used)) ret (*name) args = 0;
IMPORT(DWORD, GetModuleFileNameW, (HANDLE module, WCHAR *buf, DWORD size))
IMPORT(DWORD, GetFileAttributesW, (const WCHAR *path))
IMPORT(BOOL, CopyFileW, (const WCHAR *from, const WCHAR *to, BOOL failIfExists))
IMPORT(BOOL, DeleteFileW, (const WCHAR *path))
IMPORT(BOOL, CreateDirectoryW, (const WCHAR *path, void *security))
IMPORT(HANDLE, CreateFileW, (const WCHAR *path, DWORD access, DWORD share, void *security, DWORD disposition,
                             DWORD flags, HANDLE templ))
IMPORT(DWORD, SetFilePointer, (HANDLE file, int distance, int *high, DWORD method))
IMPORT(BOOL, WriteFile, (HANDLE file, const void *buf, DWORD size, DWORD *written, void *overlapped))
IMPORT(BOOL, CloseHandle, (HANDLE h))
IMPORT(BOOL, CreateProcessW, (const WCHAR *image, WCHAR *cmdline, void *psec, void *tsec, BOOL inherit,
                              DWORD flags, void *env, WCHAR *dir, void *startup, PROCESS_INFORMATION *pi))
IMPORT(DWORD, WaitForSingleObject, (HANDLE h, DWORD ms))
IMPORT(BOOL, GetExitCodeProcess, (HANDLE process, DWORD *code))
IMPORT(HANDLE, CreateMutexW, (void *security, BOOL owner, const WCHAR *name))
IMPORT(BOOL, ReleaseMutex, (HANDLE h))
IMPORT(DWORD, GetLastError, (void))
IMPORT(DWORD, GetTickCount, (void))
IMPORT(void, GetLocalTime, (SYSTEMTIME *t))
IMPORT(BOOL, GetVersionExW, (OSVERSIONINFOW *v))

#define MAXPATH 260
#define MISSING 0xFFFFFFFFu

static const WCHAR CF35[] = L"\\Windows\\mscoree3_5.dll";
static const WCHAR CF2[] = L"\\Windows\\mscoree2_0.dll";
static const WCHAR CAB35[] = L"NETCFv35.wm.armv4i.cab";
static const WCHAR CAB2[] = L"NETCFv2.wce4.ARMV4.cab";
static const WCHAR TEMPDIR[] = L"\\Temp";
static const WCHAR TEMP35[] = L"\\Temp\\NETCFv35.wm.armv4i.cab";
static const WCHAR TEMP2[] = L"\\Temp\\NETCFv2.wce4.ARMV4.cab";
static const WCHAR ARGS35[] = L"/noui \"\\Temp\\NETCFv35.wm.armv4i.cab\"";
static const WCHAR ARGS2[] = L"/noui \"\\Temp\\NETCFv2.wce4.ARMV4.cab\"";
static const WCHAR WCELOAD[] = L"\\Windows\\wceload.exe";
static const WCHAR HELPER[] = L"helper.exe";
static const WCHAR CARD_DIR[] = L"\\Storage Card\\2577";

/* clang may call these for struct initialisation even when freestanding. */
void *memset(void *d, int c, unsigned n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static unsigned wlen(const WCHAR *s) { unsigned n = 0; while (s[n]) n++; return n; }

/* Appends s to d (capacity MAXPATH). */
static void wcat(WCHAR *d, const WCHAR *s)
{
    unsigned n = wlen(d);
    while (*s && n < MAXPATH - 1) d[n++] = *s++;
    d[n] = 0;
}

static void wcopy(WCHAR *d, const WCHAR *s) { d[0] = 0; wcat(d, s); }

/* Cuts s after its last backslash's predecessor: "\a\b\c" -> "\a\b". */
static void parent(WCHAR *s)
{
    unsigned n = wlen(s);
    while (n > 0 && s[n - 1] != '\\') n--;
    s[n > 0 ? n - 1 : 0] = 0;
}

static BOOL exists(const WCHAR *path) { return GetFileAttributesW(path) != MISSING; }

/* True when s starts with prefix (ASCII letters compared case-insensitively). */
static BOOL startsWithI(const WCHAR *s, const WCHAR *prefix)
{
    for (; *prefix; s++, prefix++) {
        WCHAR a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* True when the argument line starts with the word w (ASCII, case-insensitive). */
static BOOL argIs(const WCHAR *cmd, const char *w)
{
    if (!cmd) return 0;
    while (*cmd == ' ' || *cmd == '"') cmd++;
    for (; *w; w++, cmd++) if ((*cmd | 0x20) != *w) return 0;
    return *cmd == 0 || *cmd == ' ' || *cmd == '"';
}

/* ---- log: \Storage Card\.wmdroid\helper.log, one ASCII line per call ---- */

static WCHAR logPath[MAXPATH];

static char *putNum(char *p, unsigned v, int width)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) t[n++] = '0';
    while (n) *p++ = t[--n];
    return p;
}

/* "0x%08X" of v into buf[11]. */
static const char *hex(char *buf, DWORD v)
{
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) buf[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    buf[10] = 0;
    return buf;
}

static char *putStr(char *p, char *end, const char *s) { while (*s && p < end) *p++ = *s++; return p; }

static char *putWide(char *p, char *end, const WCHAR *s)
{
    for (; s && *s && p < end; s++) *p++ = *s < 0x80 ? (char)*s : '?';
    return p;
}

/* "HH:mm:ss autorun: <a><w><b>", any part may be null; n >= 0 is appended as a number. */
static void logLine(const char *a, const WCHAR *w, const char *b, int n)
{
    char buf[400], *p = buf, *end = buf + sizeof buf - 16;
    SYSTEMTIME t;
    GetLocalTime(&t);
    p = putNum(p, t.hour, 2); *p++ = ':';
    p = putNum(p, t.minute, 2); *p++ = ':';
    p = putNum(p, t.second, 2);
    p = putStr(p, end, " autorun: ");
    if (a) p = putStr(p, end, a);
    if (w) p = putWide(p, end, w);
    if (b) p = putStr(p, end, b);
    if (n >= 0) p = putNum(p, (unsigned)n, 1);
    *p++ = '\r'; *p++ = '\n';
    HANDLE f = CreateFileW(logPath, 0x40000000 /* GENERIC_WRITE */, 3 /* share read|write */, 0,
                           4 /* OPEN_ALWAYS */, 0x80 /* NORMAL */, 0);
    if (f == (HANDLE)-1) return;
    DWORD done;
    SetFilePointer(f, 0, 0, 2 /* FILE_END */);
    WriteFile(f, buf, (DWORD)(p - buf), &done, 0);
    CloseHandle(f);
}

/* ---- .NET CF: 2.0 on CE 4.2 (WM 2003), 3.5 on CE 5 and later ---- */

static void installCab(const WCHAR *dir, const WCHAR *cabName, const WCHAR *tempCab, const WCHAR *argsIn,
                       const WCHAR *marker, const char *label)
{
    WCHAR cab[MAXPATH];
    wcopy(cab, dir);
    wcat(cab, L"\\");
    wcat(cab, cabName);
    if (!exists(cab)) { logLine(label, cab, " is missing", -1); return; }
    CreateDirectoryW(TEMPDIR, 0);
    if (!CopyFileW(cab, tempCab, 0)) {
        logLine("copying ", cab, " to \\Temp failed, error ", (int)GetLastError());
        return;
    }
    logLine("installing ", cab, 0, -1);
    DWORD start = GetTickCount(), code = MISSING;
    WCHAR args[MAXPATH];
    wcopy(args, argsIn);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(WCELOAD, args, 0, 0, 0, 0, 0, 0, 0, &pi)) {
        logLine("starting wceload failed, error ", 0, 0, (int)GetLastError());
    } else {
        WaitForSingleObject(pi.process, 10 * 60 * 1000);
        GetExitCodeProcess(pi.process, &code);
        CloseHandle(pi.thread);
        CloseHandle(pi.process);
    }
    DeleteFileW(tempCab);  /* wceload deletes it after a successful install */
    logLine("wceload finished, seconds: ", 0, 0, (int)((GetTickCount() - start) / 1000));
    char h[11];
    if (code != 0) logLine("wceload exit code ", 0, hex(h, code), -1);
    if (exists(marker)) logLine(label, 0, " installed", -1);
    else logLine(label, 0, " install FAILED", -1);
}

static void installCf(const WCHAR *dir)
{
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof v);
    v.size = sizeof v;
    if (GetVersionExW(&v) && v.major > 0 && v.major < 5) {
        if (exists(CF2)) return;
        installCab(dir, CAB2, TEMP2, ARGS2, CF2, ".NET CF 2.0");
        return;
    }
    if (exists(CF35)) return;
    installCab(dir, CAB35, TEMP35, ARGS35, CF35, ".NET CF 3.5");
}

/* The Windows CE loader calls an EXE's entry point as WinMain(instance, 0, arguments, show). */
int start(HANDLE instance, HANDLE prev, WCHAR *cmd, int show)
{
    (void)prev; (void)show;
    WCHAR dir[MAXPATH], helper[MAXPATH];
    DWORD n = GetModuleFileNameW(instance, dir, MAXPATH);
    if (n == 0 || n >= MAXPATH) wcopy(dir, CARD_DIR);
    else parent(dir);
    wcopy(helper, dir);
    wcat(helper, L"\\");
    wcat(helper, HELPER);
    if (!exists(helper)) {
        /* Not started from the card's folder: the shell runs a copy, \Windows\Autorun\<card>\
           autorun.exe. Normally <card> is Storage Card, but a card that comes back while that name
           is still taken mounts as Storage Card2: its own 2577 folder then. */
        static const WCHAR COPIES[] = L"\\Windows\\Autorun\\";
        helper[0] = 0;
        if (startsWithI(dir, COPIES) && dir[wlen(COPIES)]) {
            wcopy(helper, L"\\");
            wcat(helper, dir + wlen(COPIES));
            wcat(helper, L"\\2577");
        }
        if (helper[0] && exists(helper)) wcopy(dir, helper);
        else wcopy(dir, CARD_DIR);
        wcopy(helper, dir);
        wcat(helper, L"\\");
        wcat(helper, HELPER);
    }
    wcopy(logPath, dir);
    parent(logPath);
    wcat(logPath, L"\\.wmdroid");
    CreateDirectoryW(logPath, 0);
    wcat(logPath, L"\\helper.log");

    /* The shell runs "uninstall" when the card goes: nothing to do (the helper stays installed). */
    if (argIs(cmd, "uninstall")) return 0;

    HANDLE mutex = CreateMutexW(0, 1, L"WMDroidAutorun");
    if (mutex && GetLastError() == 183 /* ERROR_ALREADY_EXISTS */) WaitForSingleObject(mutex, 0xFFFFFFFFu);

    if (!exists(CF35)) installCf(dir);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    if (CreateProcessW(helper, cmd && *cmd ? cmd : 0, 0, 0, 0, 0, 0, 0, 0, &pi)) {
        CloseHandle(pi.thread);
        CloseHandle(pi.process);
    } else {
        logLine("starting ", helper, " failed, error ", (int)GetLastError());
    }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
