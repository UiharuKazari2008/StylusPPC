"""Push ROM images from the archive to the phone, where WMDroid looks for them.

usage: python tools/push-images.py [profile ...]     (no args: list profiles)
       python tools/push-images.py all
Images go to /sdcard/Android/data/<pkg>/files/images/<profile>/<file>.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ARCHIVE = os.path.normpath(os.path.join(HERE, '..', '..', 'ARM-based'))
PROFILES_KT = os.path.join(HERE, '..', 'app', 'src', 'main', 'java', 'jp', 'net', 'seventhreeseven', 'wmdroid',
                           'OsProfiles.kt')
PKG = 'jp.net.seventhreeseven.wmdroid'
ADB = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk', 'platform-tools', 'adb.exe')


def profiles():
    """Parse OsProfiles.kt so the two never disagree."""
    src = open(PROFILES_KT, encoding='utf-8').read()
    out = {}
    for block in re.split(r'\n        OsProfile\(', src)[1:]:
        pid = re.search(r'"([a-z0-9]+)"', block).group(1)
        strings = re.findall(r'"([^"]*)"', block)
        folder = strings[3]
        files = re.findall(r'ImageVariant\("[^"]+", "[^"]+", "([^"]+)"', block)
        out[pid] = (folder, files)
    return out


def main():
    profs = profiles()
    args = sys.argv[1:]
    if not args:
        for pid, (folder, files) in profs.items():
            print(f'{pid:12s} {folder}  ({len(files)} images)')
        return
    if args == ['all']:
        args = list(profs)
    adb = ADB if os.path.exists(ADB) else 'adb'
    # Files created by the adb shell user are invisible to the app, so stage them in
    # /data/local/tmp and copy them in as the app's own user (debug builds allow run-as).
    stage = '/data/local/tmp/wmdroid-push'
    subprocess.run([adb, 'shell', 'mkdir', '-p', stage], check=True)
    for pid in args:
        folder, files = profs[pid]
        dest = f'/sdcard/Android/data/{PKG}/files/images/{pid}'
        subprocess.run([adb, 'shell', 'run-as', PKG, 'mkdir', '-p', dest], check=True)
        for f in files:
            src = os.path.join(ARCHIVE, folder, 'Emulation Images', f)
            if not os.path.exists(src):
                print('missing', src)
                continue
            print('push', pid, f)
            subprocess.run([adb, 'push', src, f'{stage}/{f}'], check=True)
            subprocess.run([adb, 'shell', 'run-as', PKG, 'cp', f'{stage}/{f}', f'{dest}/{f}'], check=True)
            subprocess.run([adb, 'shell', 'rm', f'{stage}/{f}'], check=True)


if __name__ == '__main__':
    main()
