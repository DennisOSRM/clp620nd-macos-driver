"""Decode rastertoclp620 output back into pages of RGB pixels."""
import re, sys

def unpackbits(d, n):
    out = bytearray(); i = 0
    while i < len(d) and len(out) < n:
        c = d[i]; i += 1
        if c < 128:
            out += d[i:i+c+1]; i += c+1
        elif c > 128:
            out += bytes([d[i]]) * (257-c); i += 1
    out = bytes(out[:n])
    return out + bytes(max(0, n-len(out)))

def delta(prev, d, n):
    cur = bytearray(prev); pos = 0; i = 0
    while i < len(d):
        ctrl = d[i]; i += 1
        cnt = (ctrl >> 5) + 1
        off = ctrl & 0x1F
        if off == 31:
            while i < len(d):
                b = d[i]; i += 1; off += b
                if b != 255: break
        pos += off
        for _ in range(cnt):
            if pos < n and i < len(d):
                cur[pos] = d[i]; i += 1; pos += 1
    return bytes(cur)

def parse(data):
    """-> (pjl_lines, cid, pages) where pages is a list of {y: rowbytes}."""
    pjl = [l for l in re.findall(rb'@PJL[^\r\n]*', data)]
    cids = re.findall(rb'\x1b\*v6W(.{6})', data, re.S)
    pages = []; rows = {}
    W = None; ytop = 0; ycur = 0; mode = 0; prev = None
    i = 0; n = len(data)
    while i < n:
        b = data[i]
        if b == 0x0c:
            pages.append(rows); rows = {}; i += 1; continue
        if b != 0x1b:
            i += 1; continue
        m = re.match(rb'\x1b([\*&])([a-zA-Z])(-?\d*)([a-zA-Z])', data[i:])
        if not m:
            if data[i:i+2] == b'\x1bE': i += 2; continue
            i += 1; continue
        grp, cls, num, term = m.group(1), m.group(2), m.group(3), m.group(4)
        seq = (grp, cls, term)
        val = int(num) if num else 0
        i += m.end()
        if seq == (b'*', b'r', b'S'):
            W = val
        elif seq == (b'*', b'p', b'Y'):
            ytop = val
        elif seq == (b'*', b'r', b'A'):
            ycur = ytop; prev = bytes(W*3); mode = 0
        elif seq == (b'*', b'b', b'M'):
            mode = val
        elif seq == (b'*', b'b', b'W'):
            chunk = data[i:i+val]; i += val
            nb = W*3
            if mode == 5:
                # Adaptive: a run of sub-blocks, each with a 3-byte header.
                # Methods 0-3 carry one row, 4 emits rows of zero bytes, and 5
                # repeats the previous row with no data of its own.
                j = 0
                while j + 3 <= len(chunk):
                    meth = chunk[j]
                    cnt  = (chunk[j+1] << 8) | chunk[j+2]
                    j += 3
                    if meth == 5:
                        for _ in range(cnt):
                            rows[ycur] = prev; ycur += 1
                        continue
                    if meth == 4:
                        for _ in range(cnt):
                            prev = bytes(nb); rows[ycur] = prev; ycur += 1
                        continue
                    sub = chunk[j:j+cnt]; j += cnt
                    if meth == 3:   row = delta(prev, sub, nb)
                    elif meth == 2: row = unpackbits(sub, nb)
                    else:           row = sub.ljust(nb, b'\0')[:nb]
                    rows[ycur] = row; prev = row; ycur += 1
                continue
            if mode == 3:   row = delta(prev, chunk, nb)
            elif mode == 2: row = unpackbits(chunk, nb)
            else:           row = chunk.ljust(nb, b'\0')[:nb]
            rows[ycur] = row; prev = row; ycur += 1
    if rows: pages.append(rows)
    return pjl, (list(cids[0]) if cids else None), pages
