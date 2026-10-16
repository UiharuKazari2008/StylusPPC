"""Builds applink.exe (guest/AppLink/applink.c): the template of the per-app launchers PocketStylus
puts in Windows Mobile's Start > Programs > Android Apps (AndroidApps.kt fills in each app's icon).

Compiled and linked like autorun.exe (tools/make-autorun.py), plus a resource section with one icon
group (ID 1) of 32-bit icons in SIZES. Each icon is a BITMAPINFOHEADER, BGRA rows bottom-up and
an AND mask; the app finds each one by its header (40, w, 2h, 1 plane, 32 bits) and writes the
pixels in place, so a copy is the template with other pixels: no PE work on the phone.

    python tools/make-applink.py [--out path]   (default: app/src/main/assets/guest/AppLink.exe)
"""
import importlib.util, os, struct, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'guest', 'AppLink', 'applink.c')
OUT = os.path.join(ROOT, 'app', 'src', 'main', 'assets', 'guest', 'AppLink.exe')
SIZES = (16, 24, 32, 48, 64)  # keep in step with AndroidApps.ICON_SIZES

spec = importlib.util.spec_from_file_location('make_autorun', os.path.join(ROOT, 'tools', 'make-autorun.py'))
ma = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ma)


def icon_blob(s):
    """A placeholder: a grey rounded square."""
    xor = bytearray()
    andm = bytearray()
    row = ((s + 31) // 32) * 4
    r = s * 0.2
    for y in range(s - 1, -1, -1):
        bits = bytearray(row)
        for x in range(s):
            dx = max(r - x, 0, x - (s - 1 - r))
            dy = max(r - y, 0, y - (s - 1 - r))
            inside = dx * dx + dy * dy <= r * r
            xor += bytes((150, 150, 150, 255)) if inside else bytes(4)
            if not inside:
                bits[x // 8] |= 0x80 >> (x % 8)
        andm += bits
    return struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0) + xor + andm


def resources(rva):
    """IMAGE_RESOURCE_DIRECTORY tree: RT_ICON (3) ids 1..n and RT_GROUP_ICON (14) id 1, language 0x409."""
    icons = [icon_blob(s) for s in SIZES]
    group = struct.pack('<HHH', 0, 1, len(SIZES))
    for i, (s, b) in enumerate(zip(SIZES, icons)):
        group += struct.pack('<BBBBHHIH', s % 256, s % 256, 0, 0, 1, 32, len(b), i + 1)

    def directory(entries):  # entries: [(id, offset | flag)]
        return struct.pack('<IIHHHH', 0, 0, 0, 0, 0, len(entries)) + b''.join(struct.pack('<II', i, o) for i, o in entries)

    n = len(SIZES)
    sub = 0x80000000
    root_len = 16 + 8 * 2
    icon_dir = root_len
    group_dir = icon_dir + 16 + 8 * n
    lang_dirs = group_dir + 16 + 8          # n + 1 language directories of one entry each
    data_entries = lang_dirs + (n + 1) * 24  # n + 1 IMAGE_RESOURCE_DATA_ENTRY
    data = data_entries + (n + 1) * 16
    out = bytearray()
    out += directory([(3, sub | icon_dir), (14, sub | group_dir)])
    out += directory([(i + 1, sub | (lang_dirs + 24 * i)) for i in range(n)])
    out += directory([(1, sub | (lang_dirs + 24 * n))])
    for i in range(n + 1):
        out += directory([(0x409, data_entries + 16 * i)])
    blobs = icons + [group]
    offs, at = [], data
    for b in blobs:
        offs.append(at)
        at += (len(b) + 3) & ~3
    for b, o in zip(blobs, offs):
        out += struct.pack('<IIII', rva + o, len(b), 0, 0)
    assert len(out) == data
    for b in blobs:
        out += b + bytes(((len(b) + 3) & ~3) - len(b))
    return bytes(out)


def main():
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else OUT
    with tempfile.TemporaryDirectory() as d:
        entry, sections, symbols = ma.parse_elf(ma.build_elf(d, SRC))
    exe, imports = ma.build_pe(entry, sections, symbols, rsrc=resources)
    with open(out, 'wb') as f:
        f.write(exe)
    print('applink.exe: %d bytes, %d imports -> %s' % (len(exe), len(imports), out))


if __name__ == '__main__':
    main()
