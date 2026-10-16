"""Builds autorun.exe, PocketStylus' native bootstrap on the storage card (guest/Autorun/autorun.c).

NDK clang compiles the C file freestanding for ARMv4T, ld.lld links it at a fixed base (Windows CE
loads an EXE at 0x10000 in its own process slot, so it needs no relocations) and this script wraps
the sections in a Windows CE ARM PE executable. Imports: every variable the C file puts in section
.iat is one coredll function of the same name; the script builds the import directory and fills
those slots with the hint/name RVAs the loader replaces with the function addresses.

    python tools/make-autorun.py [--out path] [--src file.c]   (default: app/src/main/assets/guest/Autorun.exe)
"""
import glob, os, struct, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'guest', 'Autorun', 'autorun.c')
OUT = os.path.join(ROOT, 'app', 'src', 'main', 'assets', 'guest', 'Autorun.exe')
DLL = 'COREDLL.dll'

IMAGEBASE = 0x10000
SECT_ALIGN, FILE_ALIGN, HEADERS = 0x1000, 0x200, 0x400
LINKER_SCRIPT = '''
SECTIONS {
  . = 0x%x;
  .text : { *(.text*) }
  . = ALIGN(0x1000);
  .rdata : { *(.rodata*) }
  . = ALIGN(0x1000);
  .data : { iat_start = .; KEEP(*(.iat)) iat_end = .; *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) }
  /DISCARD/ : { *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
''' % (IMAGEBASE + 0x1000)
# PE section characteristics
FLAGS = {'.text': 0x60000020, '.rdata': 0x40000040, '.data': 0xC0000040, '.idata': 0xC0000040, '.rsrc': 0x40000040}


def tools():
    sdk = os.environ.get('ANDROID_SDK') or os.path.join(os.environ['LOCALAPPDATA'], 'Android', 'Sdk')
    ndk = sorted(glob.glob(os.path.join(sdk, 'ndk', '*')))[-1]
    b = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin')
    return [os.path.join(b, n) for n in ('clang.exe', 'ld.lld.exe')]


def build_elf(d, src):
    clang, lld = tools()
    obj, elf, lds = (os.path.join(d, 'autorun' + e) for e in ('.o', '.elf', '.lds'))
    with open(lds, 'w') as f:
        f.write(LINKER_SCRIPT)
    subprocess.check_call([clang, '--target=armv4t-none-eabi', '-marm', '-Os', '-ffreestanding', '-fno-builtin',
                           '-fshort-wchar', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                           '-fno-stack-protector', '-fno-common', '-mno-unaligned-access', '-Wall', '-Werror',
                           '-c', src, '-o', obj])
    subprocess.check_call([lld, '-T', lds, '-e', 'start', '--no-undefined', '-o', elf, obj])
    return open(elf, 'rb').read()


def parse_elf(b):
    """(entry, {name: (addr, size, data or None for NOBITS)}, {symbol: addr})"""
    assert b[:4] == b'\x7fELF' and b[4] == 1 and b[5] == 1, 'ELF32 LE expected'
    entry, = struct.unpack_from('<I', b, 0x18)
    shoff, = struct.unpack_from('<I', b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x2E)
    shdrs = [struct.unpack_from('<10I', b, shoff + i * shentsize) for i in range(shnum)]
    strtab = shdrs[shstrndx]

    def name(tab, off):
        o = tab[4] + off
        return b[o:b.index(b'\0', o)].decode()

    sections, symbols = {}, {}
    for sh in shdrs:
        n = name(strtab, sh[0])
        if sh[1] in (1, 8) and sh[2] & 2:  # PROGBITS / NOBITS, SHF_ALLOC
            sections[n] = (sh[3], sh[5], b[sh[4]:sh[4] + sh[5]] if sh[1] == 1 else None)
        if sh[1] == 2:  # SYMTAB
            strs = shdrs[sh[6]]
            for i in range(sh[5] // 16):
                st_name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', b, sh[4] + i * 16)
                if st_name:
                    symbols[name(strs, st_name)] = value
    return entry, sections, symbols


def align(v, a):
    return (v + a - 1) // a * a


def build_pe(entry, sections, symbols, rsrc=None):
    """rsrc: optional function(rva) -> bytes of a resource section placed at that RVA (make-applink.py)."""
    iat_start, iat_end = symbols['iat_start'], symbols['iat_end']
    imports = sorted((a, n) for n, a in symbols.items() if iat_start <= a < iat_end and n not in ('iat_start', 'iat_end'))
    assert imports and len(imports) * 4 == iat_end - iat_start, 'every .iat slot must be one named variable'

    parts = []  # (name, rva, virtual size, bytearray)
    for n in ('.text', '.rdata', '.data'):
        if n not in sections:
            continue
        addr, size, data = sections[n]
        parts.append([n, addr - IMAGEBASE, size, bytearray(data if data is not None else b'')])
    for p in parts:
        assert p[1] % SECT_ALIGN == 0, p

    # .idata: descriptor, terminator, lookup table, hint/names, dll name (all RVAs)
    idata_rva = align(parts[-1][1] + parts[-1][2], SECT_ALIGN)
    n = len(imports)
    ilt_off = 40
    names_off = ilt_off + 4 * (n + 1)
    idata = bytearray(names_off)
    hint_rvas = []
    for _, name in imports:
        if len(idata) % 2:
            idata += b'\0'
        hint_rvas.append(idata_rva + len(idata))
        idata += struct.pack('<H', 0) + name.encode() + b'\0'
    if len(idata) % 2:
        idata += b'\0'
    dll_rva = idata_rva + len(idata)
    idata += DLL.encode() + b'\0'
    struct.pack_into('<5I', idata, 0, idata_rva + ilt_off, 0, 0, dll_rva, iat_start - IMAGEBASE)
    for i, r in enumerate(hint_rvas):
        struct.pack_into('<I', idata, ilt_off + 4 * i, r)

    # The IAT slots start out as the hint/name RVAs, as the loader expects.
    data = next(p for p in parts if p[0] == '.data')
    for (addr, _), r in zip(imports, hint_rvas):
        struct.pack_into('<I', data[3], addr - IMAGEBASE - data[1], r)
    parts.append(['.idata', idata_rva, len(idata), idata])
    image_size = align(idata_rva + len(idata), SECT_ALIGN)
    rsrc_rva = rsrc_len = 0
    if rsrc:
        rsrc_rva = image_size
        res = bytearray(rsrc(rsrc_rva))
        rsrc_len = len(res)
        parts.append(['.rsrc', rsrc_rva, rsrc_len, res])
        image_size = align(rsrc_rva + rsrc_len, SECT_ALIGN)

    shdrs, body, ptr = b'', b'', HEADERS
    for name, rva, vsize, raw_data in parts:
        raw = align(len(raw_data), FILE_ALIGN)
        shdrs += struct.pack('<8sIIIIIIHHI', name.encode(), vsize, rva, raw, ptr if raw else 0, 0, 0, 0, 0, FLAGS[name])
        body += bytes(raw_data).ljust(raw, b'\0')
        ptr += raw

    dos = bytearray(0x40)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 0x3C, 0x40)
    # ARM; relocations stripped | executable | line numbers, symbols stripped | 32-bit
    coff = struct.pack('<HHIIIHH', 0x01C0, len(parts), 0, 0, 0, 0xE0, 0x0001 | 0x0002 | 0x000C | 0x0100)
    dirs = [(0, 0)] * 16
    dirs[1] = (idata_rva, 40)
    if rsrc:
        dirs[2] = (rsrc_rva, rsrc_len)
    dirs[12] = (iat_start - IMAGEBASE, iat_end - iat_start)
    code_size = align(parts[0][2], FILE_ALIGN)
    data_size = sum(align(p[2], FILE_ALIGN) for p in parts[1:])
    opt = struct.pack('<HBBIIIIIII', 0x10B, 6, 0, code_size, data_size, 0, entry - IMAGEBASE, parts[0][1],
                      parts[1][1] if len(parts) > 1 else 0, IMAGEBASE)
    # OS 4.20, subsystem Windows CE GUI 4.20 (runs on CE 4.2 .. WM 6.5)
    opt += struct.pack('<IIHHHHHHIIIIHHIIIIII', SECT_ALIGN, FILE_ALIGN, 4, 20, 0, 0, 4, 20, 0, image_size, HEADERS,
                       0, 9, 0, 0x10000, 0x1000, 0x100000, 0x1000, 0, 16)
    opt += b''.join(struct.pack('<II', *d) for d in dirs)
    assert len(opt) == 0xE0
    hdr = bytes(dos) + b'PE\0\0' + coff + opt + shdrs
    assert len(hdr) <= HEADERS
    return hdr.ljust(HEADERS, b'\0') + body, [n for _, n in imports]


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else OUT
    src = sys.argv[sys.argv.index('--src') + 1] if '--src' in sys.argv else SRC
    with tempfile.TemporaryDirectory() as d:
        entry, sections, symbols = parse_elf(build_elf(d, src))
    exe, imports = build_pe(entry, sections, symbols)
    with open(out, 'wb') as f:
        f.write(exe)
    print('autorun.exe: %d bytes, %d imports -> %s' % (len(exe), len(imports), out))


if __name__ == '__main__':
    main()
