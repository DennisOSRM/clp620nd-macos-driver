"""Offline test suite for rastertoclp620.

Feeds synthetic CUPS rasters (test/mkraster.c) through the filter and decodes
the PCL5c back to pixels (test/decode.py), so colour conversion, banding,
PJL setup, and the failure paths can be checked without a printer.
Run with `make check`.
"""
import os, re, signal, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from decode import parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
S    = os.environ.get("TESTDIR", os.path.join(HERE, "tmp"))
os.makedirs(S, exist_ok=True)
MK   = os.path.join(S, "mkraster")
FLT  = os.path.join(ROOT, "filter", "rastertoclp620")

# cups colour spaces
W_, RGB, RGBA, K, CMYK, SW, SRGB = 0, 1, 2, 3, 6, 18, 19

fails = []
def check(name, cond, detail=""):
    print(("  PASS  " if cond else "  FAIL  ") + name + (("   " + detail) if detail and not cond else ""))
    if not cond: fails.append(name)

def mkras(path, cs, bpc, order, W, H, res, pages, pat, rows=-1, pw=595, ph=842):
    subprocess.run([MK, path, str(cs), str(bpc), str(order), str(W), str(H),
                    str(res), str(pages), pat, str(rows), str(pw), str(ph)],
                   check=True, capture_output=True)

def run(ras, opts="", copies="1", title="t"):
    p = subprocess.run([FLT, "1", "dennis", title, copies, opts, ras],
                       capture_output=True)
    return p.returncode, p.stdout, p.stderr.decode()

def px(rows, y, x):
    r = rows[y]; return (r[x*3], r[x*3+1], r[x*3+2])

print("\n== colour conversion ==")
for name, cs, pat, want in [
    ("RGB solid black",      RGB,  "fill:0",   (0,0,0)),
    ("RGB solid white",      RGB,  "fill:255", (255,255,255)),
    ("sRGB solid black",     SRGB, "fill:0",   (0,0,0)),
    ("K black (255=toner)",  K,    "fill:255", (0,0,0)),
    ("K white (0=no toner)", K,    "fill:0",   (255,255,255)),
    ("W black (0=black)",    W_,   "fill:0",   (0,0,0)),
    ("W white (255=white)",  W_,   "fill:255", (255,255,255)),
    ("SW black",             SW,   "fill:0",   (0,0,0)),
    ("SW white",             SW,   "fill:255", (255,255,255)),
    ("CMYK white",           CMYK, "fill:0",   (255,255,255)),
    ("CMYK black (K=255)",   CMYK, "fill:255", (0,0,0)),
]:
    f = os.path.join(S, "t.ras"); mkras(f, cs, 8, 0, 64, 8, 300, 1, pat)
    rc, out, err = run(f)
    _, cid, pages = parse(out)
    got = px(pages[0], 0, 0) if pages and 0 in pages[0] else None
    check(name, rc == 0 and got == want, f"rc={rc} got={got} want={want}")

# grayscale ramp: K must invert, W must not
f = os.path.join(S, "t.ras"); mkras(f, K, 8, 0, 256, 4, 300, 1, "ramp")
rc, out, err = run(f); _, _, pages = parse(out)
r = pages[0][0]
check("K ramp inverted", rc == 0 and r[0:3] == b'\xff\xff\xff' and r[255*3:255*3+3] == b'\x00\x00\x00',
      f"first={list(r[0:3])} last={list(r[255*3:255*3+3])}")

mkras(f, W_, 8, 0, 256, 4, 300, 1, "ramp")
rc, out, err = run(f); _, _, pages = parse(out)
r = pages[0][0]
check("W ramp not inverted", rc == 0 and r[0:3] == b'\x00\x00\x00' and r[255*3:255*3+3] == b'\xff\xff\xff',
      f"first={list(r[0:3])} last={list(r[255*3:255*3+3])}")

print("\n== RGBA ==")
mkras(f, RGBA, 8, 0, 64, 4, 300, 1, "rgbtest")
rc, out, err = run(f); _, _, pages = parse(out)
ok = rc == 0 and pages and px(pages[0], 0, 0) == (0,128,0) and px(pages[0], 0, 63) == (255,128,0)
check("RGBA drops alpha, keeps RGB", ok,
      f"rc={rc} p0={px(pages[0],0,0) if pages else None} p63={px(pages[0],0,63) if pages else None}")
mkras(f, RGBA, 8, 0, 64, 4, 300, 1, "fill:255")
rc, out, _ = run(f); _, _, pages = parse(out)
check("RGBA white", rc == 0 and px(pages[0],0,0) == (255,255,255))

print("\n== RGB fidelity ==")
mkras(f, RGB, 8, 0, 200, 300, 300, 1, "rgbtest")
rc, out, _ = run(f); _, cid, pages = parse(out)
exp_ok = all(px(pages[0], y, x) == (x*255//199, 128, y & 0xff)
             for y in (0, 1, 128, 255, 299) for x in (0, 1, 99, 199))
check("rgbtest round-trips exactly", rc == 0 and exp_ok)
check("CID is {0,3,0,8,8,8}", cid == [0,3,0,8,8,8], str(cid))

print("\n== banding / resolution ==")
mkras(f, RGB, 8, 0, 2480, 3508, 300, 1, "rgbtest")
rc, out, err = run(f)
check("A4 @300 exit 0", rc == 0)
check("A4 @300 band count 3", "3 band(s)" in err, err.strip().splitlines()[-2:])
check("A4 @300 PJL RESOLUTION=300", b"@PJL SET RESOLUTION=300\r" in out)
_, _, pages = parse(out)
check("A4 @300 all 3508 rows present", len(pages[0]) == 3508, str(len(pages[0])))
check("A4 @300 row 3507 correct", px(pages[0], 3507, 100) == (100*255//2479, 128, 3507 & 0xff))

mkras(f, RGB, 8, 0, 4960, 7016, 600, 1, "rgbtest")
rc, out, err = run(f)
check("A4 @600 exit 0", rc == 0)
check("A4 @600 band count 5", "5 band(s)" in err)
check("A4 @600 PJL RESOLUTION=600", b"@PJL SET RESOLUTION=600\r" in out)
_, _, pages = parse(out)
check("A4 @600 all 7016 rows present", len(pages[0]) == 7016, str(len(pages[0])))
check("A4 @600 last row correct", px(pages[0], 7015, 4000) == (4000*255//4959, 128, 7015 & 0xff))
tallest = max(int(m) for m in re.findall(rb'\x1b\*r(\d+)T', out))
check("no raster block over 1704 rows", tallest <= 1704, str(tallest))
check("band closes match band opens",
      out.count(b'\x1b*rC') == out.count(b'\x1b*r1A'),
      f"{out.count(b'*rC')} vs {out.count(b'*r1A')}")

print("\n== multi-page ==")
mkras(f, RGB, 8, 0, 64, 100, 300, 3, "rgbtest")
rc, out, err = run(f)
_, _, pages = parse(out)
check("3 pages emitted", rc == 0 and len(pages) == 3, str(len(pages)))
check("3 PAGE: lines", err.count("PAGE: ") == 3)
check("3 form feeds", sum(1 for _ in re.finditer(rb'\x1b\*rC\x0c', out)) == 3,
      str(sum(1 for _ in re.finditer(rb'\x1b\*rC\x0c', out))))
check("one PJL JOB", out.count(b'@PJL JOB') == 1)
check("one PJL EOJ", out.count(b'@PJL EOJ') == 1)
check("one CID per page", len(re.findall(rb'\x1b\*v6W', out)) == 3)

print("\n== raster validation ==")
mkras(f, RGB, 16, 0, 64, 8, 300, 1, "fill:0")
rc, out, err = run(f)
check("16 bpc rejected", rc != 0 and "bits per colour" in err, f"rc={rc}")
check("16 bpc emits no PCL", b'\x1b*v6W' not in out)

mkras(f, RGB, 8, 1, 64, 8, 300, 1, "fill:0")   # CUPS_ORDER_BANDED
rc, out, err = run(f)
check("banded order rejected", rc != 0 and "colour order" in err, f"rc={rc}")

mkras(f, 4, 8, 0, 64, 8, 300, 1, "fill:0")     # CUPS_CSPACE_CMY
rc, out, err = run(f)
check("unsupported cspace rejected", rc != 0 and "unsupported colour" in err, f"rc={rc}")

print("\n== truncated input ==")
mkras(f, RGB, 8, 0, 64, 500, 300, 1, "rgbtest", rows=200)
rc, out, err = run(f)
check("truncated raster exits nonzero", rc != 0, f"rc={rc}")
check("truncated raster is diagnosed", "truncated raster" in err, err.strip())
check("truncated raster emits no EOJ", b'@PJL EOJ' not in out)
check("truncated raster emits no page eject", b'\x1b*rC\x0c' not in out)
check("truncated raster stopped at the short row",
      len(parse(out)[2][0]) < 500, str(len(parse(out)[2][0])))

print("\n== empty input ==")
open(os.path.join(S, "empty.ras"), "wb").close()
rc, out, err = run(os.path.join(S, "empty.ras"))
check("empty input exits nonzero", rc != 0, f"rc={rc}")

print("\n== cancellation ==")
mkras(f, RGB, 8, 0, 4960, 7016, 600, 4, "rgbtest")
p = subprocess.Popen([FLT, "1", "dennis", "cancelme", "1", "", f],
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(0.35)
p.send_signal(signal.SIGTERM)
out, err = p.communicate(timeout=30)
err = err.decode()
check("cancelled job exits nonzero", p.returncode != 0, f"rc={p.returncode}")
check("cancellation is diagnosed", "job cancelled" in err, err.strip()[-200:])
check("cancelled job emits no EOJ", b'@PJL EOJ' not in out)
check("cancelled job ends with UEL", out.endswith(b'\x1b%-12345X'))
check("cancelled job stopped early", err.count("PAGE: ") < 4, str(err.count("PAGE: ")))

print("\n== PJL options ==")
mkras(f, RGB, 8, 0, 64, 8, 300, 1, "fill:0")

def pjl(opts, copies="1"):
    rc, out, err = run(f, opts, copies)
    return rc, [l.decode() for l in parse(out)[0]]

rc, L = pjl("")
check("defaults: DUPLEX=OFF", "@PJL SET DUPLEX=OFF" in L, str(L))
check("defaults: MEDIASOURCE=AUTO", "@PJL SET MEDIASOURCE=AUTO" in L)
check("defaults: MEDIATYPE=PLAIN", "@PJL SET MEDIATYPE=PLAIN" in L)
check("defaults: ECONOMODE=OFF", "@PJL SET ECONOMODE=OFF" in L)
check("defaults: no COPIES line", not any("COPIES" in l for l in L))

rc, L = pjl("Duplex=DuplexNoTumble")
check("duplex long edge", "@PJL SET DUPLEX=ON" in L and "@PJL SET BINDING=LONGEDGE" in L, str(L))
rc, L = pjl("Duplex=DuplexTumble")
check("duplex short edge", "@PJL SET DUPLEX=ON" in L and "@PJL SET BINDING=SHORTEDGE" in L)
rc, L = pjl("Duplex=None")
check("duplex off", "@PJL SET DUPLEX=OFF" in L)

for opt, want in [("Tray1","TRAY1"), ("MPTray","MPF"), ("Auto","AUTO")]:
    rc, L = pjl("InputSlot=%s" % opt)
    check("tray %s -> %s" % (opt, want), "@PJL SET MEDIASOURCE=%s" % want in L, str(L))
rc, L = pjl("InputSlot=Manual")
check("tray Manual -> MANUAL + MANUALFEED",
      "@PJL SET MEDIASOURCE=MANUAL" in L and "@PJL SET MANUALFEED=ON" in L, str(L))

for mt in ["PLAIN","THICK","THIN","BOND","COLORED","CARDSTOCK","LABEL","OHP",
           "ENVELOPE","RECYCLED","LETTERHEAD","PREPRINTED"]:
    rc, L = pjl("MediaType=%s" % mt)
    check("media %s accepted" % mt, "@PJL SET MEDIATYPE=%s" % mt in L, str(L))
rc, L = pjl("MediaType=cardstock")
check("media lower case normalised", "@PJL SET MEDIATYPE=CARDSTOCK" in L, str(L))

rc, out, err = run(f, "MediaType=GLITTER")
L = [l.decode() for l in parse(out)[0]]
check("bogus MediaType falls back to PLAIN", "@PJL SET MEDIATYPE=PLAIN" in L, str(L))
check("bogus MediaType warns", "unknown MediaType" in err, err)

# a value that survives cupsParseOptions intact and would inject PJL if echoed
rc, out, err = run(f, 'MediaType=\'PLAIN\r\n@PJL SET DUPLEX=ON\'')
L = [l.decode() for l in parse(out)[0]]
check("MediaType cannot inject PJL",
      "@PJL SET MEDIATYPE=PLAIN" in L
      and [l for l in L if l.startswith("@PJL SET DUPLEX")] == ["@PJL SET DUPLEX=OFF"], str(L))

rc, L = pjl("TonerSave=True")
check("tonersave on", "@PJL SET ECONOMODE=ON" in L)
rc, L = pjl("TonerSave=False")
check("tonersave off", "@PJL SET ECONOMODE=OFF" in L)

rc, L = pjl("", copies="7")
check("copies 7", "@PJL SET COPIES=7" in L, str(L))
rc, L = pjl("", copies="0")
check("copies 0 -> no COPIES line", not any("COPIES" in l for l in L), str(L))

print("\n== paper size ==")
for ps, want in [("A4","A4"), ("Letter","LETTER"), ("Legal","LEGAL"),
                 ("Env10","NO10ENV"), ("EnvDL","DLENV"), ("A6","A6"),
                 ("8.5x13.5","OFICIO"), ("FanFoldGermanLegal","FOLIO")]:
    rc, L = pjl("PageSize=%s" % ps)
    check("PageSize %s -> %s" % (ps, want), "@PJL SET PAPER=%s" % want in L, str(L))

# no PageSize option: recover from the raster header
for pw, ph, want in [(595,842,"A4"), (612,792,"LETTER"), (842,595,"A4"), (297,684,"NO10ENV")]:
    mkras(f, RGB, 8, 0, 64, 8, 300, 1, "fill:0", pw=pw, ph=ph)
    rc, out, err = run(f, "")
    L = [l.decode() for l in parse(out)[0]]
    check("no PageSize, %gx%g pt -> %s" % (pw, ph, want),
          "@PJL SET PAPER=%s" % want in L, str(L))

mkras(f, RGB, 8, 0, 64, 8, 300, 1, "fill:0", pw=1000, ph=1400)
rc, out, err = run(f, "")
L = [l.decode() for l in parse(out)[0]]
check("unknown size falls back to A4 with a warning",
      "@PJL SET PAPER=A4" in L and "cannot identify paper" in err, str(L) + err)

print("\n== job wrapper ==")
mkras(f, RGB, 8, 0, 64, 8, 300, 1, "fill:0")
rc, out, err = run(f, "", title="My Report")
check("starts with UEL", out.startswith(b'\x1b%-12345X@PJL JOB NAME="My Report"\r\n'), out[:60])
check("ends with UEL", out.endswith(b'\x1b%-12345X'))
check("EOJ carries the title", b'@PJL EOJ NAME="My Report"\r\n' in out)
check("enters PCL", b'@PJL ENTER LANGUAGE = PCL\r\n\x1bE' in out)

print()
if fails:
    print("%d FAILED: %s" % (len(fails), ", ".join(fails)))
    sys.exit(1)
print("all tests passed")
