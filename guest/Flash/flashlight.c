/*
 * HTCFlashlightLib.dll: PocketStylus' stand-in for HTC's flashlight library.
 *
 * HTCFlashlight.exe loads HTCFlashlightLib.dll and calls EnableFlashLight and SetFlashLightDegree
 * (the real library drives a Qualcomm PMIC at 0xAC100000 and imports MSM_CLK.dll and wce_rex.dll,
 * which this board does not have, so the call lands at address 0). The calls, from that exe:
 *   EnableFlashLight(0)  LED off (also at startup and shutdown)
 *   EnableFlashLight(3)  LED on, after SetFlashLightDegree
 *   SetFlashLightDegree  level 0, 1 or 2, passed through a table first; the phone LED is on or off
 *
 * The LED is the phone's camera flash. Bit 8 of the camera device's FLASH register
 * (core/src/board/camera.h, +0x1C) holds it on. The low byte is the Pictures & Videos flash mode
 * and is left alone.
 *
 * Built by tools/make-flashlight.py (NDK clang, freestanding ARMv4T). The helper installs it as
 * \Windows\HTCFlashlightLib.dll and puts it back if an HD2 package copies HTC's own library there.
 */

typedef unsigned int DWORD;
typedef int BOOL;
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

#define CAMERA_PA 0x5F000000u
#define CAMERA_ID 0x4D414357u   /* 'WCAM' */
#define R_FLASH 7               /* +0x1C */
#define FLASH_HOLD 0x100

static volatile DWORD *regs;

static void *(*const pVirtualAlloc)(void *, DWORD, DWORD, DWORD) = (void *(*)(void *, DWORD, DWORD, DWORD))VirtualAlloc;
static BOOL (*const pVirtualCopy)(void *, void *, DWORD, DWORD) = (BOOL (*)(void *, void *, DWORD, DWORD))VirtualCopy;
static BOOL (*const pVirtualFree)(void *, DWORD, DWORD) = (BOOL (*)(void *, DWORD, DWORD))VirtualFree;

static void map(void)
{
    void *va;
    if (regs) return;
    va = pVirtualAlloc(0, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    if (!va) return;
    if (!pVirtualCopy(va, (void *)(CAMERA_PA >> 8), 0x1000, PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL)) {
        pVirtualFree(va, 0, MEM_RELEASE);
        return;
    }
    if (((volatile DWORD *)va)[0] != CAMERA_ID) {
        pVirtualFree(va, 0, MEM_RELEASE);
        return;
    }
    regs = va;
}

static void hold(int on)
{
    DWORD cur, next;
    map();
    if (!regs) return;
    cur = regs[R_FLASH];
    next = on ? (cur | FLASH_HOLD) : (cur & ~FLASH_HOLD);
    if (next != cur) regs[R_FLASH] = next;
}

BOOL DllMain(HANDLE inst, DWORD reason, void *reserved) { (void)inst; (void)reason; (void)reserved; return 1; }

/* mode 0 releases the LED. HTC's app passes 3 to light it. */
int EnableFlashLight(int mode)
{
    hold(mode != 0);
    return 1;
}

/* The phone's LED has no brightness steps. The call is accepted so the app can continue. */
int SetFlashLightDegree(int degree)
{
    (void)degree;
    return 1;
}
