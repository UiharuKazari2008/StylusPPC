# Copy one ROM package (by exact package-name prefix) plus extra files into a folder for the guest.
#   python -I stage.py <files-dir> <out-dir> <package-name-prefix> [extra-file ...] [--redirect OLD.dll=NEW.dll ...]
# Registry from the package's .rgu is converted to UTF-8 <out>/<pkg>.reg for review.
# --redirect renames an imported DLL in the staged EXEs/DLLs (same length), e.g. DDRAW.dll=HDRAW.dll
# sends HTC games to PocketStylus' DirectDraw caps shim (guest/DdShim/hdraw.c).
import sys, os, shutil, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsm import read_dsm

args = sys.argv[1:]
redirects = []
while '--redirect' in args:
    i = args.index('--redirect')
    old, new = args[i + 1].split('=')
    assert len(old) == len(new), 'a redirect keeps the name length'
    redirects.append((old.lower(), new))
    del args[i:i + 2]
root, out, prefix = args[0], args[1], args[2].lower()


def redirect_imports(path):
    d = bytearray(open(path, 'rb').read())
    if d[:2] != b'MZ':
        return
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0':
        return
    opt = pe + 24
    osz = struct.unpack_from('<H', d, pe + 20)[0]
    secs = [struct.unpack_from('<IIII', d, opt + osz + i * 40 + 8) for i in range(struct.unpack_from('<H', d, pe + 6)[0])]
    def off(r):
        for vs, va, rs, rp in secs:
            if va <= r < va + max(vs, rs):
                return rp + r - va
    o = off(struct.unpack_from('<I', d, opt + 104)[0] or 0x7FFFFFFF)
    changed = False
    while o is not None:
        name = struct.unpack_from('<I', d, o + 12)[0]
        if not name:
            break
        no = off(name)
        dll = bytes(d[no:d.index(b'\0', no)]).decode('latin1')
        for old, new in redirects:
            if dll.lower() == old:
                d[no:no + len(new)] = new.encode()
                print('  %s: import %s -> %s' % (os.path.basename(path), dll, new))
                changed = True
        o += 20
    if changed:
        # The change breaks the Authenticode signature, and CE refuses a badly signed binary (an
        # unsigned one runs): drop the signature.
        so, ss = struct.unpack_from('<II', d, opt + 96 + 4 * 8)
        if so and ss and so + ss == len(d):
            del d[so:]
            struct.pack_into('<II', d, opt + 96 + 4 * 8, 0, 0)
            print('  %s: signature removed' % os.path.basename(path))
        open(path, 'wb').write(d)
os.makedirs(out, exist_ok=True)
hits = []
for fn in os.listdir(root):
    if fn.lower().endswith('.dsm'):
        pkg, files = read_dsm(os.path.join(root, fn))
        if pkg and pkg.lower().startswith(prefix):
            hits.append((pkg, files))
if len(hits) != 1:
    sys.exit('need exactly one package, got %s' % [h[0] for h in hits])
pkg, files = hits[0]
lower = {f.lower(): f for f in os.listdir(root)}
for f in files + args[3:]:
    src = lower.get(f.lower())
    if not src:
        print('missing', f); continue
    if f.lower().endswith('.rgu'):
        txt = open(os.path.join(root, src), 'rb').read().decode('utf-16')
        open(os.path.join(out, pkg + '.reg'), 'w', encoding='utf-8').write(txt)
        continue
    shutil.copyfile(os.path.join(root, src), os.path.join(out, src))
    if redirects and src.lower().endswith(('.exe', '.dll')):
        redirect_imports(os.path.join(out, src))
print(pkg, len(files), 'files ->', out)
