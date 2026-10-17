# List WM 6.x ROM packages from extracted .dsm manifests.
#   python -I dsm.py <files-dir> [name-substring]      -> package name, guid, files
# DSM layout (observed): u32 header size; +0x14 u32 package-name length, +0x18 its offset; then
# 20-byte file records (u32 name length, u32 flags 0x1000xxxx, u32 name offset, u32 0, u32 ?).
# Names are UTF-16LE without terminators.
import sys, os, struct

def read_dsm(path):
    d = open(path, 'rb').read()
    hsz = struct.unpack_from('<I', d, 0)[0]
    plen, poff = struct.unpack_from('<II', d, 0x14)
    pkg = d[poff:poff + plen * 2].decode('utf-16le', 'replace')
    files = []
    p = hsz
    while p + 20 <= len(d):
        n, flags, noff = struct.unpack_from('<III', d, p)
        if flags & 0xFFFF0000 != 0x10000000 or not (0 < noff < len(d)):
            break
        files.append(d[noff:noff + n * 2].decode('utf-16le', 'replace'))
        p += 20
    return pkg, files

def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    root = sys.argv[1]
    pat = sys.argv[2].lower() if len(sys.argv) > 2 else ''
    for fn in sorted(os.listdir(root)):
        if not fn.lower().endswith('.dsm'): continue
        pkg, files = read_dsm(os.path.join(root, fn))
        if not files: continue
        blob = ' '.join([pkg or ''] + files).lower()
        if pat and pat not in blob: continue
        print('%s  %s  (%d files)' % (fn[:-4], pkg, len(files)))
        if pat:
            print('   ' + ' '.join(files))

main()
