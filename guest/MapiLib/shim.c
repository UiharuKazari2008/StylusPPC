/*
 * What MAPIlib.dll needs from outside, for tools/make-mapilib.py: imported functions (each a thunk
 * that jumps through its import slot; a slot is a variable in section .iat named
 * iat_<dll>_<function>, filled by the loader) and the C++ runtime pieces (new, delete, pure virtual).
 */
#define IMPORT(dll, name) \
    __attribute__((section(".iat"), used)) void *iat_##dll##_##name = 0; \
    __attribute__((naked)) void name(void) { \
        __asm__ volatile("ldr r12, 1f\n\tldr r12, [r12]\n\tbx r12\n1:\t.word iat_" #dll "_" #name); }

/* Each DLL's slots end with a zero slot (the loader walks the table to it). */
#define END(dll) __attribute__((section(".iat"), used)) void *iat_##dll##__end = 0;

IMPORT(cemapi, MAPILogonEx)
END(cemapi)
IMPORT(coredll, LocalAlloc)
IMPORT(coredll, LocalFree)
END(coredll)

/* operator new(unsigned), operator delete(void *, unsigned), operator delete(void *) */
void *_Znwj(unsigned size) { return ((void *(*)(unsigned, unsigned))LocalAlloc)(0x40 /* LPTR */, size); }
void _ZdlPvj(void *p, unsigned size) { if (p) ((void *(*)(void *))LocalFree)(p); }
void _ZdlPv(void *p) { if (p) ((void *(*)(void *))LocalFree)(p); }
void __cxa_pure_virtual(void) { for (;;) {} }
