# Samsung CLP-620ND — colour driver for macOS

A CUPS driver that gets full-colour output from a Samsung CLP-620ND on stock
macOS. No Ghostscript, no Homebrew, no runtime dependencies beyond what macOS
already ships.

## Why the stock setup prints black & white

macOS matches this printer to Apple's `generpcl.ppd`, which declares:

```
*ColorDevice: False
*DefaultColorSpace: Gray
*cupsFilter: "application/vnd.cups-raster 50 rastertohp"
*Resolution 600dpi: "<< /cupsBitsPerColor 1 /cupsColorSpace 3 >>"   # 3 = K only, 1 bit
```

Colour is discarded in the filter, before anything reaches the printer. The
hardware was never the limitation.

## How this driver works

```
app ──▶ PDF ──▶ cgpdftoraster ──▶ CUPS raster ──▶ rastertoclp620 ──▶ PCL5c ──▶ clp620://…:9100
                (stock macOS)      RGB 8-bit       (this driver)                  (wraps socket)
```

`cgpdftoraster` is Apple's own rasteriser and already produces 8-bit RGB when
the PPD asks for it. `rastertoclp620` is a small C program that turns those
scanlines into PCL5c direct-RGB raster. Each row is encoded twice, with TIFF
PackBits and with PCL delta-row, and the smaller result wins; the compression
mode is switched per row. Delta-row matters because a row identical to the one
above codes to nothing at all. It links against `libcups` only, so it has
nothing to install alongside it.

Page geometry, duplex, tray, media type, copies and toner save are set with
PJL, using the enum spellings the firmware itself reports. PCL5c carries only
the raster.

### Why not PCL6?

PCL5c was chosen over PCL6 because its direct-RGB raster escape sequences are
simple and fully specified, whereas PCL-XL needs a binary operator and
attribute encoder. Fidelity is identical: both carry 8-bit RGB pixels at the
same resolution.

## Install

```sh
make                                                  # builds a universal binary
sudo ./scripts/install.sh [printer-ip] [queue-name]   # defaults: 192.168.179.180 CLP620ND
./scripts/testprint.sh CLP620ND
sudo ./scripts/uninstall.sh [queue-name]
```

Building needs the Xcode Command Line Tools. Running needs nothing.

macOS seals `/usr/libexec/cups/filter` under SIP, so the filter installs to
`/Library/Printers/Samsung/CLP-620ND/filter/` and the PPD's `cupsFilter2` line
points at that absolute path. That directory is one the print sandbox allows.

`/usr/libexec/cups/backend` is *not* SIP-protected, so the `clp620` backend
installs there alongside the stock ones. See [the phantom paper
jam](#the-phantom-paper-jam) for what it is for.

## Layout

| path | purpose |
|---|---|
| `src/rastertoclp620.c` | the filter: CUPS raster to PCL5c |
| `src/clp620-backend.c` | the backend: `socket` plus the phantom-jam fix |
| `tools/genppd.sh` | generates the PPD — edit this, not the PPD |
| `tools/pclxl-dis.py` | PCL-XL disassembler, kept from the PCL6 investigation |
| `ppd/Samsung-CLP-620ND.ppd` | 16 paper sizes, colour, duplex, trays, media types |
| `scripts/install.sh`, `uninstall.sh` | driver and queue management |
| `scripts/probe.sh` | PJL queries, e.g. `./scripts/probe.sh 192.168.179.180 "INFO VARIABLES"` |
| `scripts/testprint.sh` | print the colour test page |
| `scripts/check-state.sh` | decode SNMP error bits, tray levels and supply percentages |
| `tools/pcl5c-decode.py` | decode filter output back to PNG, to check pages without printing |
| `tools/pjl-fsdump.py` | walk the printer's PJL filesystem, read-only (`make fsdump`) |
| `test/testpage.pdf` | CMYK ramps, RGB swatches, grey wedge, hairlines, registration marks |
| `test/marginruler.ps` | numbered bars for reading the unprintable margins off a print |
| `test/run_tests.py` | filter tests: colour conversion, banding, PJL setup, failure paths |
| `test/test_backend.py` | backend tests: jam filtering, fail-safe, exit-status propagation |
| `test/mkraster.c`, `decode.py` | synthetic CUPS rasters in, decoded pixels out |
| `test/fakeagent.py`, `stub_socket.sh` | stand-ins for the printer's SNMP agent and the socket backend |

`make check` runs the offline test suites — no printer needed. `make lint`
syntax-checks the scripts and validates the PPD with `cupstestppd`.

## Device facts

From `@PJL INFO CONFIG` / `INFO VARIABLES` and the printer's Configuration Report:

| | |
|---|---|
| Model, serial | CLP-620 Series, `Z32DBAHB700266N` |
| Firmware | V2.20.01.51 (2011-05-20), engine 1.02.27 |
| Languages | PCL5Ce v5.94.03, PCL6 v6.44, SPL-C v5.43 |
| Personalities | `PCL`, `PCLXL`, `QPDL`, `COLOR`, … |
| Resolutions | 200, 300, 600, FAST1200, 1200 dpi |
| Memory | 256 MB |
| Duplex unit | installed |
| Trays | `INTRAY1`, `MPTRAY`; `MEDIASOURCE` = AUTO/TRAY1/MPF/TRAY2/TRAY3/MANUAL/TRAY4 |
| Paper sizes | 17, including A4/Letter/Legal/Folio/Oficio and 5 envelopes |

Two findings worth recording.

**There is no `RENDERMODE` PJL variable on this model.** Ghostscript emits
`@PJL SET RENDERMODE=COLOR` and the firmware ignores it. Colour comes from the
page language alone.

**PostScript is not implemented**, despite `POSTSCRIPT` and `PS3` appearing in
the `PERSONALITY` enum. The Configuration Report lists only PCL5Ce, PCL6 and
SPL-C, so a PostScript PPD would have been a dead end.

## Banding, and why it is required

The engine refuses any single raster block taller than somewhere between 4000
and 6000 rows. It discards the whole page silently: no error, no output, and
`@PJL USTATUS JOB` reports `PAGES=0`. Full-page A4 at 600 dpi is 6816 rows,
over that ceiling.

Compression has nothing to do with it. A 62 KB page failed exactly as a 12 MB
one did, at identical geometry. Measured directly at 600 dpi:

| raster | result |
|---|---|
| 4758x6817, one block | discarded |
| 4758x6000, one block | discarded |
| 4758x4000, one block | prints |
| 4758x6816, five blocks of 1704 | prints, full page |

So the filter emits each page as several raster blocks of `BAND_ROWS` rows,
positioning each with the PCL cursor. That lifts the ceiling entirely and full
600 dpi colour works.

### Host halftoning was tried and abandoned

An earlier version halftoned to 1-bit CMY with Floyd-Steinberg, on the theory
that 3 bits per pixel would fit where 24 would not. The device accepts CID
`{1,3,0,1,1,1}` and then ignores the declared depth, consuming each row as
8-bit pixels, so the image printed at one eighth of its intended width.

Sending continuous tone and letting the engine halftone is both correct and
better looking, so that path was removed. `tools/pcl5c-decode.py` decodes
filter output back to PNG, which is how the 1-bit misbehaviour was identified
without wasting paper.

## Margins

The unprintable margins are not symmetric and are considerably larger than the
4.23 mm a generic PPD assumes, especially at the bottom:

| edge | unprintable |
|---|---|
| top | 6 mm |
| left | 6 mm |
| right | 9 mm |
| bottom | 12 mm |

That leaves about 195 x 279 mm of A4 actually printable. Assuming 4.23 mm all
round makes the raster 201.5 x 288.6 mm, so the right edge and the bottom of
every page are silently clipped.

`test/marginruler.ps` measures this without a ruler or a photograph. It prints
numbered bars at 0, 3, 6 ... 30 mm in from each edge; bars in the unprintable
region never appear, so the lowest visible number on each edge is the margin.
Reprint it after any change to `ImageableArea`.

## The phantom paper jam

A queue reporting `media-jam-warning` with no jam anywhere is an **empty
MP/bypass tray**. The `clp620` backend fixes this; the mechanism is below.

The firmware raises a correct `inputMediaSupplyEmpty` alert (`prtAlertCode 808`)
for the bypass tray, then reports it in `hrPrinterDetectedErrorState` as bit 5,
`jammed`. The encodings say what went wrong:

```
inputTrayEmpty   bit 13   ->   00 04      <- what it means
jammed           bit  5   ->   04 00      <- what it sends
```

The right bit value, written into the wrong octet. The device contradicts itself
at the same instant — `prtAlertTable` says 808 and lists no jam, while the
Host-Resources bitmap says jam — so only the bitmap is wrong.

```
hrPrinterDetectedErrorState = 0400     <- bit 5, "jammed"
MP Tray  level=0  status=8             <- empty, critical
alert: "The paper supply in the Bypass Tray is empty."
```

`socket` relays that byte faithfully and CUPS maps bit 5 to
`media-jam-warning`. Because an empty bypass tray is this printer's normal
resting state, the false jam is permanent.

**The fix.** `src/clp620-backend.c` is a CUPS backend that execs the stock
`socket` backend, passes everything through, and drops `media-jam-warning` from
its `STATE:` lines only when `prtAlertTable` confirms no jam. A real jam still
reports — detection accepts either IANA `prtAlertCode` 8 or the word "jam" in
`prtAlertDescription`. If SNMP does not answer it fails safe and passes the jam
through unfiltered, on the grounds that a phantom jam is an annoyance and a
concealed real one is not.

It speaks SNMPv1 directly over UDP rather than linking net-snmp, whose ABI moves
between macOS releases and which wants MIB files the backend sandbox need not
grant. The backend links nothing but libSystem.

`install.sh` uses `clp620://ip:9100` by default and falls back to plain
`socket://` with a warning if `/usr/libexec/cups/backend` is ever locked down.
Pass `socket://ip:9100` as the third argument to bypass the filtering.

Do not reach for `lpadmin -o cupsSNMPSupplies=false`. It silences the false jam
but disables the same SNMP query that reports toner levels, so you lose both —
which is the whole reason the backend exists. Run `./scripts/check-state.sh` to
see the decoded bits, tray levels, supply percentages and pending alerts.

## Limitations

- No ICC colour management. Output is Apple's RGB rasterisation sent straight
  to the printer's own colour engine, not a profiled match.
- Greyscale is sent as 24-bit RGB rather than a single channel, which wastes
  bandwidth the compression mostly hides.
- `BAND_ROWS` is 1704, chosen well below the measured ceiling rather than
  tuned. Larger bands would cut per-block overhead slightly.
