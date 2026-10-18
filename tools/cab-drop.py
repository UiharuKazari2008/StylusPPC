"""Drop window: extract a Windows Mobile CAB from an installer and put it on the phone's storage card.

The phone is running PocketStylus. While Windows Mobile is open it answers a UDP broadcast and
accepts the cabinet on TCP port 7377 (CardDrop.kt), writing it onto the folder mounted as
\\Storage Card.

    python tools/cab-drop.py
"""
import importlib.util, os, socket, struct, sys, threading, time
import tkinter as tk
from tkinter import filedialog

PORT = 7377
HERE = os.path.dirname(os.path.abspath(__file__))


def extractor():
    path = os.path.join(HERE, 'extract-wm-cab.py')
    spec = importlib.util.spec_from_file_location('extract_wm_cab', path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def host_file():
    base = os.environ.get('APPDATA') or os.path.expanduser('~')
    folder = os.path.join(base, 'WMDroid')
    os.makedirs(folder, exist_ok=True)
    return os.path.join(folder, 'cab-drop.host')


def broadcasts():
    out = ['255.255.255.255']
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            parts = ip.split('.')
            if len(parts) == 4 and not ip.startswith('127.'):
                out.append('.'.join(parts[:3] + ['255']))
    except OSError:
        pass
    return list(dict.fromkeys(out))


def discover(timeout=1.5):
    """The phone's address, from its reply to a WMDCAB? broadcast."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.settimeout(0.4)
        s.bind(('', 0))
        deadline = time.time() + timeout
        while time.time() < deadline:
            for dest in broadcasts():
                try:
                    s.sendto(b'WMDCAB?', (dest, PORT))
                except OSError:
                    pass
            s.settimeout(max(0.2, deadline - time.time()))
            try:
                data, addr = s.recvfrom(128)
            except socket.timeout:
                continue
            if data.startswith(b'WMDCAB'):
                return addr[0]
        return None
    finally:
        s.close()


def send_cab(host, port, name, blob):
    s = socket.create_connection((host, port), timeout=8)
    try:
        s.settimeout(60)
        s.sendall(f'PUT {name}\n'.encode('utf-8') + struct.pack('>I', len(blob)) + blob)
        buf = b''
        while b'\n' not in buf and len(buf) < 300:
            chunk = s.recv(300)
            if not chunk:
                break
            buf += chunk
    finally:
        s.close()
    line = buf.decode('utf-8', 'replace').strip()
    if not line.startswith('OK'):
        raise OSError(line or 'the phone did not answer')


def parse_host(text):
    text = text.strip()
    if not text:
        return None, PORT
    if ':' in text:
        host, port = text.rsplit(':', 1)
        return host, int(port)
    return text, PORT


class DropWindow:
    def __init__(self, extract):
        self.extract = extract
        self.root = tk.Tk()
        self.root.title('Windows Mobile CAB')
        self.root.geometry('440x280')
        self.root.minsize(360, 240)
        self.root.configure(bg='#f4f1ea')
        self.busy = False
        saved = ''
        try:
            saved = open(host_file(), encoding='utf-8').read().strip()
        except OSError:
            pass
        self.address = tk.StringVar(value=saved)

        tk.Label(self.root, text='Drop an installer here', bg='#f4f1ea', fg='#1c1915',
                 font=('Segoe UI', 16)).pack(pady=(28, 4))
        tk.Label(self.root, text='The cabinet is copied onto the phone\'s storage card.',
                 bg='#f4f1ea', fg='#5c564c', font=('Segoe UI', 10)).pack()
        tk.Button(self.root, text='Choose a file', command=self.browse, font=('Segoe UI', 10),
                  padx=12, pady=4).pack(pady=16)
        row = tk.Frame(self.root, bg='#f4f1ea')
        row.pack(fill='x', padx=24)
        tk.Label(row, text='Phone', bg='#f4f1ea', fg='#5c564c', font=('Segoe UI', 10)).pack(side='left')
        tk.Entry(row, textvariable=self.address, font=('Segoe UI', 10)).pack(side='left', fill='x', expand=True, padx=(8, 0))
        self.status = tk.Label(self.root, text='Waiting for a file. Windows Mobile has to be open.',
                               bg='#f4f1ea', fg='#1c1915', font=('Segoe UI', 10), wraplength=400, justify='left')
        self.status.pack(padx=24, pady=18, anchor='w')
        self.root.update_idletasks()
        self.hook_drop()
        self.root.after(200, self.find_phone)

    def set_status(self, text):
        self.root.after(0, lambda: self.status.configure(text=text))

    def find_phone(self):
        def run():
            found = discover()
            if found and not self.address.get().strip():
                self.root.after(0, lambda: self.address.set(found))
                self.set_status(f'Phone found at {found}. Drop an installer.')
        threading.Thread(target=run, daemon=True).start()

    def browse(self):
        paths = filedialog.askopenfilenames(
            title='Windows Mobile installers',
            filetypes=[('Installers and cabinets', '*.exe *.cab *.zip *.rar'), ('All files', '*.*')])
        if paths:
            self.accept(paths)

    def accept(self, paths):
        if self.busy or not paths:
            return
        self.busy = True
        threading.Thread(target=self.work, args=(list(paths),), daemon=True).start()

    def work(self, paths):
        try:
            host, port = parse_host(self.address.get())
            if not host:
                self.set_status('Looking for the phone…')
                host = discover()
                if host:
                    self.root.after(0, lambda: self.address.set(host))
            if not host:
                self.set_status('Phone not found. Open Windows Mobile, or type its address.')
                return
            try:
                open(host_file(), 'w', encoding='utf-8').write(host if port == PORT else f'{host}:{port}')
            except OSError:
                pass
            sent, skipped = [], []
            for path in paths:
                self.set_status('Reading ' + os.path.basename(path))
                cabs, note = self.extract.collect(path)
                if not cabs:
                    skipped.append(os.path.basename(path) + (' (' + note + ')' if note else ''))
                    continue
                for name, blob in cabs:
                    self.set_status('Sending ' + name)
                    send_cab(host, port, os.path.basename(name), blob)
                    sent.append(os.path.basename(name))
            text = 'On the storage card: ' + ', '.join(sent) if sent else 'No Windows Mobile CAB found'
            if skipped:
                text += '. Skipped: ' + ', '.join(skipped)
            self.set_status(text)
        except Exception as e:
            self.set_status(str(e) or e.__class__.__name__)
        finally:
            self.busy = False

    def hook_drop(self):
        """WM_DROPFILES on this window. The wndproc has to stay referenced or it is collected."""
        import ctypes
        from ctypes import wintypes
        user32, shell32 = ctypes.windll.user32, ctypes.windll.shell32
        WM_DROPFILES = 0x0233
        GWLP_WNDPROC = -4
        LRESULT = ctypes.c_ssize_t
        WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)
        set_long = getattr(user32, 'SetWindowLongPtrW', user32.SetWindowLongW)
        set_long.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_void_p]
        set_long.restype = ctypes.c_void_p
        user32.CallWindowProcW.argtypes = [ctypes.c_void_p, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        user32.CallWindowProcW.restype = LRESULT
        shell32.DragAcceptFiles.argtypes = [wintypes.HWND, wintypes.BOOL]
        shell32.DragQueryFileW.argtypes = [wintypes.WPARAM, wintypes.UINT, ctypes.c_wchar_p, wintypes.UINT]
        shell32.DragQueryFileW.restype = wintypes.UINT
        shell32.DragFinish.argtypes = [wintypes.WPARAM]

        def hwnd_key(hw):
            if isinstance(hw, int):
                return hw
            value = getattr(hw, 'value', None)
            if value is None:
                value = ctypes.cast(hw, ctypes.c_void_p).value
            return int(value or 0)

        hwnd = wintypes.HWND(self.root.winfo_id())
        owner = self

        def wndproc(hw, msg, wp, lp):
            if msg == WM_DROPFILES:
                count = shell32.DragQueryFileW(wp, 0xFFFFFFFF, None, 0)
                files = []
                for i in range(count):
                    n = shell32.DragQueryFileW(wp, i, None, 0)
                    buf = ctypes.create_unicode_buffer(n + 1)
                    shell32.DragQueryFileW(wp, i, buf, n + 1)
                    files.append(buf.value)
                shell32.DragFinish(wp)
                owner.root.after(0, lambda f=list(files): owner.accept(f))
                return 0
            return user32.CallWindowProcW(owner._old_procs[hwnd_key(hw)], hw, msg, wp, lp)

        self._wndproc = WNDPROC(wndproc)
        self._old_procs = {}

        def subclass(hw):
            key = hwnd_key(hw)
            if key in self._old_procs:
                return
            self._old_procs[key] = set_long(hw, GWLP_WNDPROC, ctypes.cast(self._wndproc, ctypes.c_void_p))
            shell32.DragAcceptFiles(hw, True)

        subclass(hwnd)
        children = []
        ENUM = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def each(child, _lp):
            children.append(child)
            return True

        cb = ENUM(each)
        user32.EnumChildWindows.argtypes = [wintypes.HWND, ENUM, wintypes.LPARAM]
        user32.EnumChildWindows.restype = wintypes.BOOL
        user32.EnumChildWindows(hwnd, cb, 0)
        self._enum_cb = cb
        for child in children:
            subclass(child)

    def run(self):
        self.root.mainloop()


def main():
    try:
        extract = extractor()
    except Exception as e:
        print('could not load extract-wm-cab.py:', e, file=sys.stderr)
        return 1
    DropWindow(extract).run()
    return 0


if __name__ == '__main__':
    sys.exit(main())
