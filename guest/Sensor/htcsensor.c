/*
 * HTCSensorService.dll: PocketStylus' stand-in for HTC's G-sensor stream driver (SEN0:).
 *
 * HTC's sensor apps (Teeter = Labyrinth.exe, and anything built on HTCSensorSDK) open "SEN0:" and
 * talk to it with DeviceIoControl. If SEN0: doesn't exist they register it themselves with
 * RegisterDevice(L"SEN", 0, L"HTCSensorService.dll", 1), so installing this DLL as
 * \Windows\HTCSensorService.dll is enough; no registry entry is needed.
 *
 * The protocol, from Teeter's embedded SensorSDK (docs/research-hd2-port.md, "Teeter sensor"):
 *   0x0100000F  start the tilt sensor (8 bytes in: two DWORDs, 0 and 1 from Teeter)
 *   0x01000001  read the tilt sensor: SENSORDATA (20 bytes) into the out buffer. Teeter passes an
 *               out size of 0, so the size is not checked
 *   0x02000001  read the light sensor
 *   0x02000000|n, 0x01000000|n  other light/tilt requests: accepted, out buffer zeroed
 * Gravity comes from the PocketStylus motion device (core/src/board/motion.h) in HTC's frame:
 * 1000 = 1 g, face up = 0, 0, -1000, left edge down x < 0, top edge down y < 0. On a board without it (Microsoft's
 * Device Emulator) the phone reads as lying face up.
 *
 * Built by tools/make-htcsensor.py (NDK clang, freestanding ARMv4T; the script makes the PE DLL
 * with its exports, imports and relocations, and writes guest/Helper/SensorDll.cs).
 */

typedef unsigned int DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef short SHORT;
typedef void *HANDLE;

#define IMPORT(dll, name) \
    __attribute__((section(".iat"), used)) void *iat_##dll##_##name = 0; \
    __attribute__((naked)) void name(void) { \
        __asm__ volatile("ldr r12, 1f\n\tldr r12, [r12]\n\tbx r12\n1:\t.word iat_" #dll "_" #name); }
#define END(dll) __attribute__((section(".iat"), used)) void *iat_##dll##__end = 0;

IMPORT(coredll, VirtualAlloc)
IMPORT(coredll, VirtualCopy)
IMPORT(coredll, VirtualFree)
END(coredll)

#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
#define PAGE_NOCACHE 0x200
#define PAGE_PHYSICAL 0x400

#define MOTION_PA 0x5F200000u   /* Motion::BASE */
#define MOTION_ID 0x4E544F4Du   /* 'MOTN' */
enum { R_ID = 0, R_OPEN = 1, R_SEQ = 2, R_X = 3, R_Y = 4, R_Z = 5, R_FLAGS = 6, R_IOCTL = 7 };

#define IOCTL_TILT_START 0x0100000Fu
#define IOCTL_TILT_READ 0x01000001u
#define IOCTL_LIGHT_READ 0x02000001u

typedef struct {
    SHORT tiltX, tiltY, tiltZ;   /* gravity, about 1000 per g */
    SHORT unknown1;              /* always 0 on HTC's driver */
    int angleY, angleX;          /* 0..359 */
    int unknown2;
} SENSORDATA;

static volatile DWORD *regs;     /* the motion device page, or 0 */
static int opens;

static void *(*const pVirtualAlloc)(void *, DWORD, DWORD, DWORD) = (void *(*)(void *, DWORD, DWORD, DWORD))VirtualAlloc;
static BOOL (*const pVirtualCopy)(void *, void *, DWORD, DWORD) = (BOOL (*)(void *, void *, DWORD, DWORD))VirtualCopy;
static BOOL (*const pVirtualFree)(void *, DWORD, DWORD) = (BOOL (*)(void *, DWORD, DWORD))VirtualFree;

static void zero(void *p, DWORD n) { BYTE *b = p; while (n--) *b++ = 0; }

static void map(void)
{
    void *va;
    if (regs) return;
    va = pVirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    if (!va) return;
    if (!pVirtualCopy(va, (void *)(MOTION_PA >> 8), 0x1000, PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL)) {
        pVirtualFree(va, 0, MEM_RELEASE);
        return;
    }
    if (((volatile DWORD *)va)[R_ID] != MOTION_ID) {   /* not PocketStylus */
        pVirtualFree(va, 0, MEM_RELEASE);
        return;
    }
    regs = va;
}

static SHORT clamp16(int v) { return (SHORT)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void tilt(SENSORDATA *d)
{
    zero(d, sizeof *d);
    if (regs) {
        d->tiltX = clamp16((int)regs[R_X]);
        d->tiltY = clamp16((int)regs[R_Y]);
        d->tiltZ = clamp16((int)regs[R_Z]);
    } else {
        d->tiltZ = -1000;
    }
}

BOOL DllMain(HANDLE inst, DWORD reason, void *reserved) { (void)inst; (void)reason; (void)reserved; return 1; }

DWORD SEN_Init(DWORD context) { (void)context; map(); return 0x53454E30; /* 'SEN0' */ }
BOOL SEN_Deinit(DWORD dev) { (void)dev; return 1; }

DWORD SEN_Open(DWORD dev, DWORD access, DWORD share)
{
    (void)access; (void)share;
    map();
    opens++;
    if (regs) regs[R_OPEN] = (DWORD)opens;
    return dev;
}

BOOL SEN_Close(DWORD open)
{
    (void)open;
    if (opens > 0) opens--;
    if (regs) regs[R_OPEN] = (DWORD)opens;
    return 1;
}

DWORD SEN_Read(DWORD open, void *buf, DWORD n) { (void)open; (void)buf; (void)n; return 0; }
DWORD SEN_Write(DWORD open, const void *buf, DWORD n) { (void)open; (void)buf; (void)n; return 0; }
DWORD SEN_Seek(DWORD open, int pos, DWORD how) { (void)open; (void)pos; (void)how; return (DWORD)-1; }
void SEN_PowerUp(DWORD dev) { (void)dev; }
void SEN_PowerDown(DWORD dev) { (void)dev; }

BOOL SEN_IOControl(DWORD open, DWORD code, BYTE *in, DWORD inLen, BYTE *out, DWORD outLen, DWORD *actual)
{
    (void)open; (void)in; (void)inLen;
    map();
    if (code == IOCTL_TILT_READ) {
        if (!out) return 0;
        tilt((SENSORDATA *)out);
        if (actual) *actual = sizeof(SENSORDATA);
        return 1;
    }
    if (code == IOCTL_LIGHT_READ) {          /* a lit room; layout unknown, first DWORD = level */
        if (out && outLen >= 4) { zero(out, outLen); *(DWORD *)out = 5; }
        if (actual) *actual = out ? outLen : 0;
        return 1;
    }
    if (code != IOCTL_TILT_START && regs) regs[R_IOCTL] = code;   /* host logs codes we don't know */
    if (out && outLen) zero(out, outLen);
    if (actual) *actual = 0;
    return 1;
}
