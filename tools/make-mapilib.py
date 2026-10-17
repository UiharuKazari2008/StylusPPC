"""Builds MAPIlib.dll, the native half of MAPIdotnet (third_party/mapidotnet), for Windows Mobile.

NDK clang compiles MAPIdotnet's MAPIlib.cpp and guest/MapiLib (additions and an import shim) for
ARMv4T against the Windows Mobile 6 SDK headers (third_party/wm6sdk/inc, unpacked from the SDK's
MSI; third_party/shim holds patched copies of headers clang cannot read). ld.lld links at a
preferred base and keeps its relocations (--emit-relocs); this script turns that into a Windows CE
ARM PE DLL: exports by name, base relocations (every R_ARM_ABS32), and imports from cemapi.dll and
coredll.dll (each variable iat_<dll>_<function> in section .iat is one import slot).

    python tools/make-mapilib.py [--out path]   (default: app/src/main/assets/guest/MAPIlib.dll)
"""
import importlib.util, os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('make_autorun', os.path.join(ROOT, 'tools', 'make-autorun.py'))
ma = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ma)

TP = os.path.join(ROOT, 'third_party')
MAPILIB = os.path.join(TP, 'mapidotnet', 'mapidotnet-code-r20', 'MAPIdotnet', 'MAPIlib')
OUT = os.path.join(ROOT, 'app', 'src', 'main', 'assets', 'guest', 'MAPIlib.dll')
IMAGEBASE = 0x10000000
SECT_ALIGN, FILE_ALIGN, HEADERS = 0x1000, 0x200, 0x400
LINKER_SCRIPT = '''
SECTIONS {
  . = 0x%x;
  .text : { *(.text*) }
  . = ALIGN(0x1000);
  .rdata : { *(.rodata*) *(.data.rel.ro*) }
  . = ALIGN(0x1000);
  .data : { iat_start = .; KEEP(*(.iat)) iat_end = .; *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) }
  /DISCARD/ : { *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
''' % (IMAGEBASE + 0x1000)
# .idata read-only: Windows CE refuses a DLL whose import tables are writable (ERROR_BAD_EXE_FORMAT).
FLAGS = {'.text': 0x60000020, '.rdata': 0x40000040, '.data': 0xC0000040, '.idata': 0x40000040,
         '.edata': 0x40000040, '.reloc': 0x42000040}
EXPORT_PREFIXES = ('IMAPI', 'IMsgStore', 'IMessage', 'MAPIlib', 'Release')

CFLAGS = ['--target=armv4t-none-eabi', '-marm', '-Os', '-ffreestanding', '-fno-builtin', '-fshort-wchar',
          '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-fno-stack-protector', '-fno-common',
          '-mno-unaligned-access', '-Wno-everything']
CXXFLAGS = CFLAGS + ['-fno-exceptions', '-fno-rtti', '-fms-extensions', '-fms-compatibility', '-D_WIN32', '-DWIN32',
                     '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WCE=0x502', '-DUNDER_CE=0x502', '-DWINCE', '-DARM', '-D_ARM_',
                     '-DARMV4I', '-D_M_ARM', '-DUNICODE', '-D_UNICODE', '-DWIN32_PLATFORM_PSPC',
                     '-I' + os.path.join(TP, 'shim'), '-I' + os.path.join(TP, 'wm6sdk', 'inc'), '-I' + MAPILIB]


def build_elf(d, srcs=None):
    clang, lld = ma.tools()
    clangxx = clang.replace('clang.exe', 'clang++.exe')
    objs = []
    for src, cxx in srcs or ((os.path.join(MAPILIB, 'MAPIlib.cpp'), True), (os.path.join(ROOT, 'guest', 'MapiLib', 'ext.cpp'), True),
                             (os.path.join(ROOT, 'guest', 'MapiLib', 'shim.c'), False)):
        obj = os.path.join(d, os.path.basename(src) + '.o')
        subprocess.check_call([clangxx if cxx else clang] + (CXXFLAGS if cxx else CFLAGS) + ['-c', src, '-o', obj])
        objs.append(obj)
    lds, elf = os.path.join(d, 'mapilib.lds'), os.path.join(d, 'mapilib.elf')
    with open(lds, 'w') as f:
        f.write(LINKER_SCRIPT)
    subprocess.check_call([lld, '-T', lds, '-e', 'DllMain', '--no-undefined', '--emit-relocs', '-o', elf] + objs)
    return open(elf, 'rb').read()


def abs32_relocs(b, sections):
    """Addresses of every R_ARM_ABS32 (and TARGET1) in the allocated sections."""
    shoff, = struct.unpack_from('<I', b, 0x20)
    shentsize, shnum, _ = struct.unpack_from('<HHH', b, 0x2E)
    shdrs = [struct.unpack_from('<10I', b, shoff + i * shentsize) for i in range(shnum)]
    out = []
    for sh in shdrs:
        if sh[1] != 9:  # SHT_REL
            continue
        target = shdrs[sh[7]]
        if not target[2] & 2:  # relocations of a non-allocated section (debug info)
            continue
        for i in range(sh[5] // 8):
            off, info = struct.unpack_from('<II', b, sh[4] + i * 8)
            if info & 0xFF in (2, 38):
                out.append(off)
    return sorted(set(out))


def build_pe(entry, sections, symbols, relocs, dllname='MAPIlib.dll', prefixes=EXPORT_PREFIXES):
    iat_start, iat_end = symbols['iat_start'], symbols['iat_end']
    slots = sorted((a, n) for n, a in symbols.items() if n.startswith('iat_') and n not in ('iat_start', 'iat_end'))
    assert len(slots) * 4 == iat_end - iat_start, 'every .iat slot must be one iat_<dll>_<name> variable'
    if not slots:
        iat_start = iat_end = 0
    exports = sorted(n for n in symbols if n.startswith(prefixes) and symbols[n] >= IMAGEBASE)

    parts = []
    for n in ('.text', '.rdata', '.data'):
        if n in sections:
            addr, size, data = sections[n]
            parts.append([n, addr - IMAGEBASE, size, bytearray(data if data is not None else b'')])

    def next_rva():
        return ma.align(parts[-1][1] + parts[-1][2], SECT_ALIGN)

    # .idata: one descriptor per DLL; the IAT is the .iat slots (grouped by DLL: sorted by address,
    # and the shim declares each DLL's imports together).
    groups = []
    for addr, n in slots:
        dll, func = n[4:].split('_', 1)
        if not groups or groups[-1][0] != dll:
            groups.append((dll, []))
        groups[-1][1].append((addr, func))
    rva = next_rva()
    desc_size = 20 * (len(groups) + 1)
    idata = bytearray(desc_size)
    data = next((p for p in parts if p[0] == '.data'), None)
    for gi, (dll, funcs) in enumerate(groups):
        # The zero slot (iat_<dll>__end) closes this DLL's part of the IAT and is not an import.
        assert funcs[-1][1] == '_end' and all(f != '_end' for _, f in funcs[:-1]), 'iat slots of %s out of order' % dll
        funcs = funcs[:-1]
        while len(idata) % 4:  # lookup tables are arrays of words
            idata += bytes(1)
        ilt = len(idata)
        idata += bytes(4 * (len(funcs) + 1))
        for fi, (addr, func) in enumerate(funcs):
            if len(idata) % 2:
                idata += b'\0'
            hint = rva + len(idata)
            idata += struct.pack('<H', 0) + func.encode() + b'\0'
            struct.pack_into('<I', idata, ilt + 4 * fi, hint)
            struct.pack_into('<I', data[3], addr - IMAGEBASE - data[1], hint)
        if len(idata) % 2:
            idata += b'\0'
        name = rva + len(idata)
        idata += (dll.upper() + '.dll').encode() + b'\0'
        struct.pack_into('<5I', idata, 20 * gi, rva + ilt, 0, 0, name, funcs[0][0] - IMAGEBASE)
    parts.append(['.idata', rva, len(idata), idata])

    # .edata: exports by name
    rva = next_rva()
    n = len(exports)
    edata = bytearray(40)
    funcs_off = len(edata); edata += bytes(4 * n)
    names_off = len(edata); edata += bytes(4 * n)
    ords_off = len(edata); edata += bytes(2 * n)
    name_rva = rva + len(edata); edata += dllname.encode() + b'\0'
    for i, e in enumerate(exports):
        struct.pack_into('<I', edata, funcs_off + 4 * i, symbols[e] - IMAGEBASE)
        struct.pack_into('<I', edata, names_off + 4 * i, rva + len(edata))
        struct.pack_into('<H', edata, ords_off + 2 * i, i)
        edata += e.encode() + b'\0'
    struct.pack_into('<IIHHIIIIIII', edata, 0, 0, 0, 0, 0, name_rva, 1, n, n, rva + funcs_off, rva + names_off, rva + ords_off)
    parts.append(['.edata', rva, len(edata), edata])

    # .reloc: HIGHLOW for every absolute address, in 4K blocks
    rva = next_rva()
    reloc = bytearray()
    pages = {}
    for a in relocs:
        r = a - IMAGEBASE
        pages.setdefault(r & ~0xFFF, []).append(r & 0xFFF)
    for page in sorted(pages):
        entries = [0x3000 | o for o in pages[page]]
        if len(entries) % 2:
            entries.append(0)
        reloc += struct.pack('<II', page, 8 + 2 * len(entries)) + struct.pack('<%dH' % len(entries), *entries)
    if not reloc:
        reloc = bytearray(struct.pack('<IIHH', 0x1000, 12, 0, 0))  # an empty block, as the loader expects one
    parts.append(['.reloc', rva, len(reloc), reloc])
    image_size = ma.align(rva + len(reloc), SECT_ALIGN)

    shdrs, body, ptr = b'', b'', HEADERS
    for name, srva, vsize, raw_data in parts:
        raw = ma.align(len(raw_data), FILE_ALIGN)
        shdrs += struct.pack('<8sIIIIIIHHI', name.encode(), max(vsize, 1), srva, raw, ptr if raw else 0, 0, 0, 0, 0, FLAGS[name])
        body += bytes(raw_data).ljust(raw, b'\0')
        ptr += raw
    dos = bytearray(0x40)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x40)
    # ARM; executable | line numbers, symbols stripped | 32-bit | DLL
    coff = struct.pack('<HHIIIHH', 0x01C0, len(parts), 0, 0, 0, 0xE0, 0x0002 | 0x000C | 0x0100 | 0x2000)
    dirs = [(0, 0)] * 16
    find = {p[0]: p for p in parts}
    dirs[0] = (find['.edata'][1], len(find['.edata'][3]))
    if slots:
        dirs[1] = (find['.idata'][1], desc_size)
    dirs[5] = (find['.reloc'][1], len(find['.reloc'][3]))
    if slots:
        dirs[12] = (iat_start - IMAGEBASE, iat_end - iat_start)
    code_size = ma.align(parts[0][2], FILE_ALIGN)
    data_size = sum(ma.align(p[2], FILE_ALIGN) for p in parts[1:])
    opt = struct.pack('<HBBIIIIIII', 0x10B, 6, 0, code_size, data_size, 0, entry - IMAGEBASE, parts[0][1], parts[1][1], IMAGEBASE)
    opt += struct.pack('<IIHHHHHHIIIIHHIIIIII', SECT_ALIGN, FILE_ALIGN, 4, 20, 0, 0, 4, 20, 0, image_size, HEADERS,
                       0, 9, 0, 0x10000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b''.join(struct.pack('<II', *d) for d in dirs)
    hdr = bytes(dos) + b'PE\0\0' + coff + opt + shdrs
    assert len(hdr) <= HEADERS
    return hdr.ljust(HEADERS, b'\0') + body, exports, [g[0] for g in groups], len(relocs)


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else OUT
    # --test file.c: a test DLL from one C file (loader experiments)
    srcs = [(sys.argv[sys.argv.index('--test') + 1], False)] if '--test' in sys.argv else None
    with tempfile.TemporaryDirectory() as d:
        elf = build_elf(d, srcs)
    entry, sections, symbols = ma.parse_elf(elf)
    dll, exports, dlls, nrel = build_pe(entry, sections, symbols, abs32_relocs(elf, sections))
    with open(out, 'wb') as f:
        f.write(dll)
    print('MAPIlib.dll: %d bytes, %d exports, imports from %s, %d relocations -> %s' % (len(dll), len(exports), ', '.join(dlls), nrel, out))


if __name__ == '__main__':
    main()
