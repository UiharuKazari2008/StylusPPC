"""Pull Windows Mobile installable CABs out of a desktop setup program.

SPB and most Pocket PC setup EXEs are self-extractors (WinRAR, ZIP, or a raw
Microsoft cabinet) with the device package inside. An installable cabinet is the
one wceload accepts: it contains _setup.xml, or a legacy .000 setup INF.

Stored cabinets are carved from the file. Compressed ones are unpacked with
7-Zip when `7z` is on PATH.

    python tools/extract-wm-cab.py setup.exe [-o directory]
"""
import hashlib, io, os, shutil, struct, subprocess, sys, tempfile, zipfile, zlib

RAR4 = b'Rar!\x1a\x07\x00'
RAR_TYPES = set(range(0x72, 0x7c))


def parse_cab(data, off):
    """A Microsoft cabinet at off, or None when the header is not a real cabinet."""
    if off < 0 or off + 36 > len(data) or data[off:off + 4] != b'MSCF':
        return None
    cb, coff, vmin, vmaj, folders, files, _flags = struct.unpack_from('<I4xI4xBBHHH', data, off + 8)
    if vmaj != 1 or vmin > 3 or not (36 <= cb <= len(data) - off):
        return None
    if not (0 < files <= 100000) or folders > 100000 or coff >= cb:
        return None
    end = off + cb
    pos = off + coff
    names = []
    for _ in range(files):
        if pos + 16 > end:
            return None
        attrs, = struct.unpack_from('<H', data, pos + 14)
        pos += 16
        nul = data.find(b'\0', pos, end)
        if nul < 0:
            return None
        raw = data[pos:nul]
        try:
            names.append(raw.decode('utf-8' if attrs & 0x80 else 'latin1'))
        except UnicodeDecodeError:
            names.append(raw.decode('latin1', 'replace'))
        pos = nul + 1
    return {'size': cb, 'names': names, 'blob': data[off:end]}


def is_wm(names):
    for name in names:
        base = name.replace('\\', '/').rsplit('/', 1)[-1].lower()
        if base == '_setup.xml' or base.endswith('.000'):
            return True
    return False


def iter_cabs(data):
    off = 0
    while off < len(data):
        i = data.find(b'MSCF', off)
        if i < 0:
            return
        cab = parse_cab(data, i)
        if cab:
            yield i, cab
            off = i + cab['size']
        else:
            off = i + 4


def rar_members(data):
    """(name, data_offset, size, method) for each file in an embedded RAR 4 archive."""
    start = 0
    while True:
        sig = data.find(RAR4, start)
        if sig < 0:
            return
        pos = sig + 7
        start = pos
        while pos + 7 <= len(data):
            _crc, typ, flags, hsize = struct.unpack_from('<HBHH', data, pos)
            if typ not in RAR_TYPES or hsize < 7 or pos + hsize > len(data):
                break
            add = struct.unpack_from('<I', data, pos + 7)[0] if flags & 0x8000 else 0
            if pos + hsize + add > len(data):
                break
            if typ == 0x74:
                base = pos + (11 if flags & 0x8000 else 7)
                if base + 21 <= pos + hsize:
                    _unp, _host, _crc, _time, _ver, method, nsize, _attr = struct.unpack_from('<IBIIBBHI', data, base)
                    noff = base + 21 + (8 if flags & 0x100 else 0)
                    raw = data[noff:noff + nsize]
                    raw = raw.split(b'\0', 1)[0]
                    name = raw.decode('latin1')
                    if name:
                        yield name, pos + hsize, add, method
            if typ == 0x7b:
                break
            nxt = pos + hsize + add
            if nxt <= pos:
                break
            pos = nxt
        start = max(start, pos)


def name_covering(members, off):
    for name, start, size, _method in members:
        if start <= off < start + size:
            return os.path.basename(name.replace('\\', '/'))
    return None


def zip_members(data):
    try:
        zf = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        return
    for info in zf.infolist():
        if info.is_dir():
            continue
        try:
            yield info.filename, zf.read(info)
        except (RuntimeError, zipfile.BadZipFile, zlib.error):
            continue


def find_7z():
    return shutil.which('7z') or shutil.which('7z.exe')


def cabs_via_7z(path):
    exe = find_7z()
    if not exe:
        return [], '7-Zip is not on PATH'
    td = tempfile.mkdtemp(prefix='wmcab-')
    note = ''
    found = []
    try:
        r = subprocess.run([exe, 'x', '-y', '-bb0', '-o' + td, '--', path],
                           capture_output=True, text=True, errors='replace')
        text = (r.stdout or '') + (r.stderr or '')
        if r.returncode != 0:
            note = '7-Zip reported a damaged archive' if ('Data Error' in text or 'Headers Error' in text) else '7-Zip could not unpack the installer'
        for root, _dirs, names in os.walk(td):
            for name in names:
                fp = os.path.join(root, name)
                if os.path.getsize(fp) > 256 << 20:
                    continue
                blob = open(fp, 'rb').read()
                cab = parse_cab(blob, 0)
                if name.lower().endswith('.cab') and cab and is_wm(cab['names']):
                    found.append((name, cab['blob']))
                    continue
                for off, cab in iter_cabs(blob):
                    if is_wm(cab['names']):
                        found.append((f'{name}-{off:x}.cab', cab['blob']))
    finally:
        shutil.rmtree(td, ignore_errors=True)
    return found, note


def expand_inner(blob, names):
    """Device CABs stored as members of an outer desktop cabinet."""
    if not any(os.path.basename(n).lower().endswith('.cab') for n in names):
        return []
    root = os.environ.get('SYSTEMROOT', r'C:\Windows')
    expand = os.path.join(root, 'System32', 'expand.exe')
    if not os.path.isfile(expand):
        return []
    td = tempfile.mkdtemp(prefix='wmcab-')
    found = []
    try:
        src = os.path.join(td, 'outer.cab')
        dest = os.path.join(td, 'out')
        os.mkdir(dest)
        open(src, 'wb').write(blob)
        subprocess.run([expand, src, '-F:*.cab', dest], capture_output=True)
        for dirpath, _dirs, files in os.walk(dest):
            for name in files:
                raw = open(os.path.join(dirpath, name), 'rb').read()
                cab = parse_cab(raw, 0)
                if cab and is_wm(cab['names']):
                    found.append((name, cab['blob']))
    finally:
        shutil.rmtree(td, ignore_errors=True)
    return found


def collect(path):
    data = open(path, 'rb').read()
    if not data:
        return [], 'file is empty'
    members = list(rar_members(data))
    found = []
    seen = set()

    def add(name, blob):
        digest = hashlib.sha256(blob).digest()
        if digest in seen or not blob:
            return
        seen.add(digest)
        found.append((name, blob))

    source = os.path.basename(path)
    for off, cab in iter_cabs(data):
        if is_wm(cab['names']):
            add(name_covering(members, off) or (source if off == 0 else f'package-{off:x}.cab'), cab['blob'])
        else:
            for name, blob in expand_inner(cab['blob'], cab['names']):
                add(name, blob)
    for name, blob in zip_members(data):
        cab = parse_cab(blob, 0)
        if cab and is_wm(cab['names']):
            add(os.path.basename(name.replace('\\', '/')), cab['blob'])
        else:
            for off, inner in iter_cabs(blob):
                if is_wm(inner['names']):
                    add(f'{os.path.basename(name)}-{off:x}.cab', inner['blob'])
    compressed = [n for n, _o, _s, method in members if n.lower().endswith('.cab') and method != 0x30]
    note = ''
    if not found or compressed:
        extra, note = cabs_via_7z(path)
        for name, blob in extra:
            add(name, blob)
        if found:
            note = ''
    return found, note


def safe_name(name, used):
    base = os.path.basename(name.replace('\\', '/').replace('/', os.sep))
    base = ''.join(c if c not in '<>:"|?*' and ord(c) >= 32 else '_' for c in base).strip(' .')
    if not base.lower().endswith('.cab'):
        base += '.cab'
    stem, ext = os.path.splitext(base)
    candidate = base
    n = 2
    while candidate.lower() in used:
        candidate = f'{stem}-{n}{ext}'
        n += 1
    used.add(candidate.lower())
    return candidate


def main(argv):
    args = list(argv)
    out = None
    if '-o' in args:
        i = args.index('-o')
        if i + 1 >= len(args):
            print('extract-wm-cab: -o needs a directory', file=sys.stderr)
            return 2
        out = args[i + 1]
        del args[i:i + 2]
    if not args or args[0] in ('-h', '--help'):
        print(__doc__.strip())
        return 0 if args else 2
    paths = args
    if out is None:
        stem = os.path.splitext(os.path.basename(paths[0]))[0]
        out = os.path.join(os.path.dirname(os.path.abspath(paths[0])), stem + '-cabs')
    os.makedirs(out, exist_ok=True)
    used = {n.lower() for n in os.listdir(out)}
    wrote = 0
    for path in paths:
        if not os.path.isfile(path):
            print(f'{path}: not a file', file=sys.stderr)
            continue
        cabs, note = collect(path)
        if not cabs:
            print(f'{path}: no Windows Mobile CAB found' + (f' ({note})' if note else ''), file=sys.stderr)
            continue
        prefix = ''
        if len(paths) > 1:
            prefix = os.path.splitext(os.path.basename(path))[0] + '-'
        for name, blob in cabs:
            dest = os.path.join(out, safe_name(prefix + name, used))
            open(dest, 'wb').write(blob)
            print(f'{os.path.basename(dest)}  {len(blob)} bytes')
            wrote += 1
    if not wrote:
        return 1
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
