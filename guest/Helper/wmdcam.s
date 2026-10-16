@ wmdcam.dll: the frame generator WMDroid plugs into NULLCAM.dll (the WM 6.x sample camera driver).
@
@ NULLCAM loads HKLM\Drivers\Capture\NullCam LibraryName / FunctionName on every pin open and calls
@     DWORD Fill(BYTE *buf, CS_VIDEOINFOHEADER *vih, DWORD mode, BOOL flag, DWORD *extra)
@ for each frame; the return value is the frame's DataUsed. This DLL forwards the two pointers to
@ WMDroid's camera device (core/src/board/camera.h), which writes the frame into guest memory.
@
@ Built by tools/make-wmdcam.py: NDK clang assembles it, ld.lld links .text/.rdata/.data at
@ IMAGEBASE + 0x1000/0x2000/0x3000, and the script wraps them in a PE (with base relocations for the
@ .data pointers) and writes guest/Helper/CameraDll.cs, which the helper installs as
@ \Windows\wmdcam.dll. ARM code, ARMv4T+.

        .syntax unified
        .arm

        .equ IMAGEBASE, 0x10000000      @ tables below hold RVAs: label - IMAGEBASE
        .equ CAM_PA_SHR8, 0x005F0000    @ Camera::BASE (0x5F000000) >> 8, for VirtualCopy
        .equ REG_ID, 0x00
        .equ REG_CTRL, 0x04
        .equ REG_BUF, 0x08
        .equ REG_VIH, 0x0C
        .equ REG_GO, 0x10
        .equ REG_RESULT, 0x14
        .equ REG_ORIENT, 0x18
        @ .data layout
        .equ D_MMIO, 0
        .equ D_VIRTUALALLOC, 4
        .equ D_VIRTUALCOPY, 8
        .equ D_VIRTUALFREE, 12

@ \reg = address of .data. CE 5.2 keeps a DLL's writable sections apart from its code, so this is
@ an absolute address with a base relocation (listed in .wmdrel for make-wmdcam.py).
        .macro DATA reg
        ldr     \reg, 1f
        b       2f
1:      .word   data
        .pushsection .wmdrel, "a"
        .word   1b
        .popsection
2:
        .endm

        .text

@ BOOL WINAPI DllMain(HANDLE module, DWORD reason, LPVOID reserved)
@ Process attach (the driver opened a pin): map the device and say the camera is opening.
@ Process detach (the last pin closed): say it closed, so the phone's camera stops at once.
DllMain:
        push    {r4, r5, r6, lr}
        cmp     r1, #1
        bne     1f
        bl      map
        b       9f
1:      cmp     r1, #0
        bne     9f
        DATA    r5
        ldr     r4, [r5, #D_MMIO]
        cmp     r4, #0
        beq     9f
        mov     r0, #0
        str     r0, [r4, #REG_CTRL]
        str     r0, [r5, #D_MMIO]
        mov     r0, r4
        mov     r1, #0
        mov     r2, #0x8000             @ MEM_RELEASE
        ldr     r12, [r5, #D_VIRTUALFREE]
        blx     r12
9:      mov     r0, #1
        pop     {r4, r5, r6, pc}

@ Returns the device's registers (mapping them on first use), or 0.
map:
        push    {r4, r5, r6, lr}
        DATA    r5
        ldr     r4, [r5, #D_MMIO]
        cmp     r4, #0
        bne     8f
        mov     r0, #0
        mov     r1, #0x1000
        mov     r2, #0x2000             @ MEM_RESERVE
        mov     r3, #1                  @ PAGE_NOACCESS
        ldr     r12, [r5, #D_VIRTUALALLOC]
        blx     r12
        movs    r4, r0
        beq     8f
        mov     r1, #CAM_PA_SHR8
        mov     r2, #0x1000
        mov     r3, #0x600              @ PAGE_PHYSICAL | PAGE_NOCACHE
        orr     r3, r3, #4              @ | PAGE_READWRITE
        ldr     r12, [r5, #D_VIRTUALCOPY]
        blx     r12
        cmp     r0, #0
        beq     7f
        ldr     r0, [r4, #REG_ID]       @ is WMDroid's device there?
        ldr     r1, id
        cmp     r0, r1
        bne     7f
        str     r4, [r5, #D_MMIO]
        @ From inside device.exe: the emulator turns the driver's formats to the sensor's
        @ orientation (portrait on a portrait panel) before anything streams.
        mov     r0, #0x100
        str     r0, [r4, #REG_ORIENT]
        mov     r0, #1
        str     r0, [r4, #REG_CTRL]
        b       8f
7:      mov     r0, r4
        mov     r1, #0
        mov     r2, #0x8000             @ MEM_RELEASE
        ldr     r12, [r5, #D_VIRTUALFREE]
        blx     r12
        mov     r4, #0
8:      mov     r0, r4
        pop     {r4, r5, r6, pc}

@ DWORD Fill(BYTE *buf, CS_VIDEOINFOHEADER *vih, DWORD mode, BOOL flag, DWORD *extra)
Fill:
        push    {r4-r7, lr}
        mov     r5, r0
        mov     r6, r1
        bl      map
        movs    r4, r0
        mvneq   r0, #0                  @ no device: -1, as NULLCAM's own generator fails
        beq     9f
        mov     r7, #1024               @ pages we may have to commit
1:      str     r5, [r4, #REG_BUF]
        str     r6, [r4, #REG_VIH]
        str     r5, [r4, #REG_GO]
        ldr     r0, [r4, #REG_RESULT]
        cmp     r0, #0
        bge     9f                      @ bytes written
        cmn     r0, #1
        beq     9f                      @ unsupported format: -1
        bic     r1, r0, #0x80000000     @ offset of a page that is not committed yet:
        ldrb    r2, [r5, r1]            @ touch it (the kernel commits it) and ask again
        strb    r2, [r5, r1]
        subs    r7, r7, #1
        bne     1b
        mvn     r0, #0
9:      pop     {r4-r7, pc}

        .balign 4
id:     .word 0x4D414357                @ 'WCAM'

@ ---- read-only data: directory, exports, imports (all RVAs) ----
        .section .rdata, "a"
        .balign 4
@ Directory for make-wmdcam.py.
        .word DllMain - IMAGEBASE
        .word exports - IMAGEBASE, exports_end - exports
        .word imports - IMAGEBASE, imports_end - imports
        .word iat - IMAGEBASE, iat_end - iat

exports:
        .word 0, 0                      @ Characteristics, TimeDateStamp
        .hword 0, 0                     @ version
        .word dllname - IMAGEBASE
        .word 1                         @ ordinal base
        .word 1, 1                      @ functions, names
        .word exp_funcs - IMAGEBASE, exp_names - IMAGEBASE, exp_ords - IMAGEBASE
exp_funcs:
        .word Fill - IMAGEBASE
exp_names:
        .word name_Fill - IMAGEBASE
exp_ords:
        .hword 0
dllname:
        .asciz "wmdcam.dll"
name_Fill:
        .asciz "Fill"
exports_end:

        .balign 4
imports:                                @ coredll, by name
        .word ilt - IMAGEBASE, 0, 0, coredll - IMAGEBASE, iat - IMAGEBASE
        .word 0, 0, 0, 0, 0
imports_end:
ilt:
        .word hn_VirtualAlloc - IMAGEBASE, hn_VirtualCopy - IMAGEBASE, hn_VirtualFree - IMAGEBASE, 0
        .balign 2
hn_VirtualAlloc:
        .hword 0
        .asciz "VirtualAlloc"
        .balign 2
hn_VirtualCopy:
        .hword 0
        .asciz "VirtualCopy"
        .balign 2
hn_VirtualFree:
        .hword 0
        .asciz "VirtualFree"
        .balign 2
coredll:
        .asciz "COREDLL.dll"

@ ---- writable data: the mapped registers and the import address table ----
        .data
        .balign 4
data:
        .word 0                                 @ D_MMIO
iat:
        .word hn_VirtualAlloc - IMAGEBASE       @ D_VIRTUALALLOC
        .word hn_VirtualCopy - IMAGEBASE        @ D_VIRTUALCOPY
        .word hn_VirtualFree - IMAGEBASE        @ D_VIRTUALFREE
        .word 0
iat_end:
