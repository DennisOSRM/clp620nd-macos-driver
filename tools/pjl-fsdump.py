#!/usr/bin/env python3
"""Walks the printer's PJL filesystem over port 9100, and optionally pulls it down.

Read-only by design: it sends INFO, FSQUERY, FSDIRLIST and FSUPLOAD, and never
FSDOWNLOAD, FSDELETE, FSMKDIR or DEFAULT.  Nothing it does can modify the device.

  ./tools/pjl-fsdump.py [ip]                    walk and list
  ./tools/pjl-fsdump.py [ip] --download out/    walk, then fetch every file

Reading the result.  FSDIRLIST on a volume that does not exist answers
FILEERROR=16, so an error proves the handler is alive and the syntax was
accepted.  Silence for a volume that INFO FILESYS advertises is the opposite:
the volume is declared for PJL conformance but the directory handler is stubbed
out.  On the CLP-620ND that is what happens, and the two advertised volumes are
about 972 KB anyway - scratch space for downloaded fonts and macros, orders of
magnitude too small to hold a firmware image.

If this returns nothing, note that the vendor .hd route is largely dead for the
CLP-620: the only images still circulating are patched ones sold by toner-chip
reset vendors (versions tagged 45f/50f/51f/55f, the "f" meaning modified), not
stock Samsung firmware.  Reading the flash off the formatter board is the only
route left to a stock image.
"""
import argparse, os, re, socket, sys, time

UEL = b'\x1b%-12345X'
FF  = b'\x0c'

class PJL:
    def __init__(self, ip, port=9100, timeout=15.0, verbose=False):
        self.verbose = verbose
        self.timeout = timeout
        self.buf = b''
        self.sock = socket.create_connection((ip, port), timeout=10.0)
        self.sock.settimeout(timeout)
        self.send(b'')                      # UEL + bare @PJL wakes the parser

    def send(self, cmd):
        line = UEL + b'@PJL\r\n'
        if cmd:
            line += b'@PJL ' + cmd + b'\r\n'
        if self.verbose and cmd:
            print("  >>", cmd.decode('latin-1'), file=sys.stderr)
        self.sock.sendall(line)

    def _fill(self):
        """Read one more chunk. Returns False on timeout or a closed socket."""
        try:
            d = self.sock.recv(65536)
        except socket.timeout:
            return False
        if not d:
            return False
        self.buf += d
        return True

    def drain(self, quiet=1.5):
        """Swallow a late reply so it cannot be read as the next command's."""
        self.sock.settimeout(quiet)
        try:
            while True:
                try:
                    if not self.sock.recv(65536):
                        break
                except socket.timeout:
                    break
        finally:
            self.sock.settimeout(self.timeout)
        self.buf = b''

    def read_to_ff(self):
        """Response text up to the terminating form feed. b'' means no reply."""
        while FF not in self.buf:
            if not self._fill():
                out, self.buf = self.buf, b''
                self.drain()
                return out
        i = self.buf.index(FF)
        out, self.buf = self.buf[:i], self.buf[i+1:]
        return out

    def read_exact(self, n):
        while len(self.buf) < n:
            if not self._fill():
                break
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def read_line(self):
        while b'\n' not in self.buf:
            if not self._fill():
                out, self.buf = self.buf, b''
                return out
        i = self.buf.index(b'\n')
        out, self.buf = self.buf[:i+1], self.buf[i+1:]
        return out

    def ask(self, cmd):
        self.send(cmd)
        return self.read_to_ff().decode('latin-1').replace('\r', '')

    def close(self):
        try:
            self.sock.sendall(UEL)
            self.sock.close()
        except OSError:
            pass


ENTRY = re.compile(r'^(?P<name>.*?)\s+TYPE\s*=\s*(?P<type>DIR|FILE)'
                   r'(?:\s+SIZE\s*=\s*(?P<size>\d+))?\s*$')
ERRLINE = re.compile(r'FILEERROR\s*=\s*(\d+)', re.I)

def dirlist(p, path, count=512):
    """-> (entries, error) where entries is [(name, 'DIR'|'FILE', size)]."""
    resp = p.ask(('FSDIRLIST NAME="%s" ENTRY=1 COUNT=%d' % (path, count)).encode('latin-1'))
    if not resp.strip():
        return [], 'no response (handler stubbed out)'
    m = ERRLINE.search(resp)
    if m:
        return [], 'FILEERROR=%s' % m.group(1)
    out = []
    for line in resp.splitlines():
        line = line.rstrip()
        if not line or line.startswith('@PJL'):
            continue
        e = ENTRY.match(line)
        if not e:
            continue
        name = e.group('name').strip()
        if name in ('.', '..'):
            continue
        out.append((name, e.group('type'),
                    int(e.group('size')) if e.group('size') else None))
    return out, None


def walk(p, root, maxdepth=8, maxentries=20000):
    """Depth-first walk. Yields (path, type, size)."""
    seen, stack, n = set(), [(root, 0)], 0
    while stack:
        path, depth = stack.pop()
        if path in seen or depth > maxdepth or n > maxentries:
            continue
        seen.add(path)
        entries, err = dirlist(p, path)
        if err:
            if depth == 0:
                yield (path, 'ERROR', err)
            continue
        for name, typ, size in entries:
            n += 1
            child = path.rstrip('\\') + '\\' + name
            yield (child, typ, size)
            if typ == 'DIR':
                stack.append((child, depth + 1))


def upload(p, path, size, chunk=65536):
    """Fetch one file with FSUPLOAD. Returns bytes (possibly short)."""
    data = b''
    offset = 0
    limit = size if size is not None else 1 << 30
    while offset < limit:
        want = min(chunk, limit - offset)
        p.send(('FSUPLOAD NAME="%s" OFFSET=%d SIZE=%d' % (path, offset, want)).encode('latin-1'))
        header = b''
        while b'\r\n' not in header and b'\n' not in header:
            line = p.read_line()
            if not line:
                return data
            header += line
            if header.strip().startswith(b'@PJL FSUPLOAD'):
                break
        h = header.decode('latin-1')
        if 'FILEERROR' in h.upper():
            break
        m = re.search(r'SIZE\s*=\s*(\d+)', h)
        got = int(m.group(1)) if m else 0
        if got == 0:
            p.read_to_ff()
            break
        blob = p.read_exact(got)
        p.read_to_ff()                      # trailing form feed
        data += blob
        offset += len(blob)
        if len(blob) < got:
            break
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ip', nargs='?', default='192.168.179.180')
    ap.add_argument('--download', metavar='DIR', help='fetch every file into DIR')
    ap.add_argument('--volumes', default='', help='comma list to try, e.g. "0:,1:"')
    ap.add_argument('--max-size', type=int, default=64 << 20)
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--timeout', type=float, default=15.0)
    a = ap.parse_args()

    p = PJL(a.ip, timeout=a.timeout, verbose=a.verbose)

    ident = p.ask(b'INFO ID')
    if not ident.strip():
        print("no PJL response from %s:9100 - is the printer awake?" % a.ip)
        return 2
    print("== INFO ID ==\n%s" % ident.strip())

    fsys = p.ask(b'INFO FILESYS')
    print("\n== INFO FILESYS ==\n%s" % (fsys.strip() or "(no response - "
          "the firmware does not advertise a PJL filesystem)"))

    advertised = dict(re.findall(r'^\s*(\d+:)\s+(\d+)', fsys, re.M))

    vols = [v.strip() for v in a.volumes.split(',') if v.strip()]
    if not vols:
        vols = list(advertised)
        for v in ('0:', '1:', '2:'):
            if v not in vols:
                vols.append(v)

    print("\n== volumes ==")
    for v in vols:
        q = p.ask(('FSQUERY NAME="%s\\"' % v).encode('latin-1'))
        note = ''
        if v in advertised:
            kb = int(advertised[v])
            note = '  [advertised %s KB%s]' % (
                format(kb, ','),
                ', far too small for a firmware image' if kb < 4096 else '')
        print("  %-4s %s%s" % (v, ' '.join(q.split()) or '(no response)', note))

    files, total = [], 0
    print("\n== walk ==")
    for v in vols:
        root = v + '\\'
        found = False
        for path, typ, size in walk(p, root):
            found = True
            if typ == 'ERROR':
                print("  %-40s  %s" % (path, size))
            elif typ == 'DIR':
                print("  %-40s  DIR" % path)
            else:
                print("  %-40s  %s bytes" % (path, size if size is not None else '?'))
                files.append((path, size))
                total += size or 0
        if not found:
            print("  %-40s  empty or inaccessible" % root)

    print("\n%d file(s), %s bytes total" % (len(files), format(total, ',')))

    if not files:
        mute = [v for v in vols if v in advertised]
        if mute:
            print("\nVolume(s) %s are advertised by INFO FILESYS but the directory\n"
                  "handler answers nothing, while a non-existent volume answers\n"
                  "FILEERROR=16.  The PJL filesystem is stubbed out: there is no\n"
                  "firmware to pull over port 9100.  The only route left to a stock\n"
                  "image is reading the flash off the formatter board." % ', '.join(mute))

    if a.download and files:
        os.makedirs(a.download, exist_ok=True)
        print("\n== download -> %s ==" % a.download)
        for path, size in files:
            if size is not None and size > a.max_size:
                print("  skip %s (%s bytes > --max-size)" % (path, format(size, ',')))
                continue
            rel = path.replace(':', '_').replace('\\', '/').lstrip('/')
            dest = os.path.join(a.download, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            blob = upload(p, path, size)
            with open(dest, 'wb') as fh:
                fh.write(blob)
            flag = '' if size in (None, len(blob)) else '  (SHORT, wanted %s)' % size
            print("  %-40s  %s bytes%s" % (rel, format(len(blob), ','), flag))
    elif a.download:
        print("\nnothing to download")

    p.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
