#!/usr/bin/env python3
"""Decodes a PCL5c raster stream back to a PNG, so filter output can be
checked without printing. Handles CID direct-RGB 8/8/8 and direct-CMY 1/1/1,
compression modes 0, 2 (PackBits) and 3 (delta row).
Usage: pcl5c-decode.py in.pcl out.png [max_rows]"""
import sys, re, zlib, struct

def unpack_packbits(d, n):
    out = bytearray(); i = 0
    while i < len(d) and len(out) < n:
        c = d[i]; i += 1
        if c < 128:
            out += d[i:i+c+1]; i += c+1
        elif c > 128:
            out += bytes([d[i]]) * (257-c); i += 1
    return bytes(out[:n]) + bytes(max(0, n-len(out)))

def apply_delta(prev, d, n):
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
        for k in range(cnt):
            if pos < n and i < len(d):
                cur[pos] = d[i]; i += 1; pos += 1
    return bytes(cur)

def main():
    data = open(sys.argv[1], 'rb').read()
    out_png = sys.argv[2]
    maxrows = int(sys.argv[3]) if len(sys.argv) > 3 else 100000

    m = re.search(rb'\x1b\*v6W(.{6})', data, re.S)
    cid = m.group(1); print("CID:", list(cid))
    cs, enc, bpi, br, bg, bb = cid
    W = int(re.search(rb'\x1b\*r(\d+)S', data).group(1))
    # Total height spans every band: last cursor offset plus that block's rows.
    offs = [int(m.group(1)) for m in re.finditer(rb'\x1b\*p0X\x1b\*p(\d+)Y', data)]
    hs   = [int(m.group(1)) for m in re.finditer(rb'\x1b\*r(\d+)T', data)]
    H = (offs[-1] + hs[-1]) if offs else (hs[0] if hs else 0)
    print(f"raster {W}x{H}, colorspace={cs} enc={enc} bits={br}/{bg}/{bb}")

    onebit = (br == 1)
    rowbytes = (W*3+7)//8 if onebit else W*3
    pos = data.find(b'\x1b*r1A')
    if pos < 0: pos = data.find(b'\x1b*r0A')
    pos += 5

    mode = 0
    prev = bytes(rowbytes)
    rows = []
    pat  = re.compile(rb'\x1b\*b(\d+)([WM])')
    band = re.compile(rb'\x1b\*p0X\x1b\*p(\d+)Y')   # start of a new raster block
    pos = 0
    while pos < len(data) and len(rows) < min(H, maxrows):
        mb = band.match(data, pos)
        if mb:
            # new band: reseed the delta reference and pad to its Y offset
            prev = bytes(rowbytes); mode = 0
            target = int(mb.group(1))
            while len(rows) < target: rows.append(bytes(rowbytes))
            nxt = data.find(b'\x1b*r1A', pos)
            pos = nxt + 5 if nxt >= 0 else mb.end()
            continue
        m = pat.match(data, pos)
        if not m:
            pos += 1; continue
        val = int(m.group(1)); kind = m.group(2)
        pos = m.end()
        if kind == b'M':
            mode = val; continue
        chunk = data[pos:pos+val]; pos += val
        if mode == 5:
            # Adaptive: the transfer is a run of 3-byte-headed sub-blocks.
            # Methods 0-3 carry one row; method 5 repeats the previous row and
            # carries no data; method 4 would emit rows of zero bytes.
            j = 0
            while j + 3 <= len(chunk) and len(rows) < min(H, maxrows):
                meth = chunk[j]
                cnt  = (chunk[j+1] << 8) | chunk[j+2]
                j += 3
                if meth == 5:
                    for _ in range(cnt):
                        rows.append(prev)
                    continue
                if meth == 4:
                    for _ in range(cnt):
                        prev = bytes(rowbytes); rows.append(prev)
                    continue
                sub = chunk[j:j+cnt]; j += cnt
                if meth == 0:   raw = sub.ljust(rowbytes, b'\0')[:rowbytes]
                elif meth == 1: raw = sub.ljust(rowbytes, b'\0')[:rowbytes]
                elif meth == 2: raw = unpack_packbits(sub, rowbytes)
                elif meth == 3: raw = apply_delta(prev, sub, rowbytes)
                else:           raw = bytes(rowbytes)
                prev = raw
                rows.append(raw)
            continue
        if mode == 0:   raw = chunk.ljust(rowbytes, b'\0')[:rowbytes]
        elif mode == 2: raw = unpack_packbits(chunk, rowbytes)
        elif mode == 3: raw = apply_delta(prev, chunk, rowbytes)
        else:           raw = bytes(rowbytes)
        prev = raw
        rows.append(raw)

    print(f"decoded {len(rows)} rows (expected {H})")
    px = bytearray()
    for r in rows:
        px.append(0)                      # PNG filter byte
        if onebit:
            for x in range(W):
                bit = x*3
                v = 0
                for b in range(3):
                    bp = bit+b
                    v = (v<<1) | ((r[bp>>3] >> (7-(bp & 7))) & 1)
                c, mg, y = (v>>2)&1, (v>>1)&1, v&1
                px += bytes((255*(1-c), 255*(1-mg), 255*(1-y)))
        else:
            px += r[:W*3]

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d))
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', W, len(rows), 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(px), 6))
           + chunk(b'IEND', b''))
    open(out_png, 'wb').write(png)
    print("wrote", out_png)

main()
