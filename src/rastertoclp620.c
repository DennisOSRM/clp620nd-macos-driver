/*
 * rastertoclp620 - CUPS raster -> PCL5c filter for the Samsung CLP-620ND.
 *
 * cupsd calls: rastertoclp620 job user title copies options [file]
 * Input  : CUPS raster, produced by the stock macOS cgpdftoraster filter.
 * Output : PJL job setup + PCL5c direct-RGB raster.
 *
 * Links only against libcups, which ships with macOS.  No Ghostscript and no
 * Homebrew: the print sandbox (deny default) grants no access to /opt, so an
 * external binary there is unreachable from a filter.
 *
 * Banding is the reason this works at 600 dpi.  The engine refuses any single
 * raster block taller than somewhere between 4000 and 6000 rows: it discards
 * the page silently, reporting PAGES=0 and no error.  Full-page A4 at 600 dpi
 * is 6816 rows, over that ceiling, so the page is emitted as several raster
 * blocks of BAND_ROWS each, positioned with the PCL cursor.  Compression is
 * irrelevant to the ceiling; a 62 KB page failed just as a 12 MB one did.
 *
 * Contone RGB throughout, at 300 and at 600 dpi.  Host halftoning to 1-bit CMY
 * was tried and abandoned: the device accepts CID {1,3,0,1,1,1} and then
 * ignores the depth, consuming the row as 8-bit pixels, so the image prints at
 * one eighth width.  Sending continuous tone and letting the engine halftone is
 * both correct and better looking.
 *
 * Page geometry, duplex, tray, media and copies are set with PJL, using the
 * enum spellings the firmware reports via INFO CONFIG / INFO VARIABLES.
 */

#include <cups/cups.h>
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#define BAND_ROWS 1704      /* comfortably under the observed row ceiling */

static volatile sig_atomic_t Canceled = 0;
static void cancel_job(int sig) { (void)sig; Canceled = 1; }

/*
 * PPD PageSize keyword -> PJL PAPER name (from @PJL INFO CONFIG PAPERS), with
 * the media dimensions in points so a missing PageSize option can be recovered
 * from the raster header instead of being guessed.  Keep in step with the
 * PAGESIZES list in tools/genppd.sh.
 */
static const struct paper {
    const char *ppd, *pjl;
    int w, h;
} Papers[] = {
    { "A4",                 "A4",         595,  842 },
    { "Letter",             "LETTER",     612,  792 },
    { "Legal",              "LEGAL",      612, 1008 },
    { "Executive",          "EXECUTIVE",  522,  756 },
    { "B5",                 "JISB5",      516,  729 },
    { "ISOB5",              "ISOB5",      499,  709 },
    { "A5",                 "A5",         420,  595 },
    { "A6",                 "A6",         298,  420 },
    { "FanFoldGermanLegal", "FOLIO",      612,  936 },
    { "8.5x13.5",           "OFICIO",     612,  972 },
    { "Statement",          "STATEMENT",  396,  612 },
    { "Env10",              "NO10ENV",    297,  684 },
    { "EnvMonarch",         "MONARCHENV", 279,  540 },
    { "EnvDL",              "DLENV",      312,  624 },
    { "EnvC5",              "C5ENV",      459,  649 },
    { "EnvC6",              "C6ENV",      323,  459 },
};
#define NPAPERS (sizeof Papers / sizeof Papers[0])

static const char *pjl_paper_by_name(const char *ps)
{
    if (!ps || !*ps) return NULL;
    for (size_t i = 0; i < NPAPERS; i++)
        if (!strcasecmp(ps, Papers[i].ppd)) return Papers[i].pjl;
    return NULL;
}

/*
 * Recover the paper name from the page dimensions the rasteriser recorded.
 * Both orientations are accepted; 4 points of slack covers the rounding
 * between the PPD's integer points and the header's float.
 */
static const char *pjl_paper_by_size(double w, double h)
{
    if (w <= 0 || h <= 0) return NULL;
    for (size_t i = 0; i < NPAPERS; i++) {
        double pw = Papers[i].w, ph = Papers[i].h;
        int upright  = (w > pw - 4 && w < pw + 4) && (h > ph - 4 && h < ph + 4);
        int rotated  = (w > ph - 4 && w < ph + 4) && (h > pw - 4 && h < pw + 4);
        if (upright || rotated) return Papers[i].pjl;
    }
    return NULL;
}

/*
 * Media types the firmware reports.  The value goes straight into a PJL
 * command, so it is whitelisted rather than passed through: an unknown or
 * hostile option string must not be able to inject PJL of its own.
 */
static const char *MediaTypes[] = {
    "PLAIN", "THICK", "THIN", "BOND", "COLORED", "CARDSTOCK",
    "LABEL", "OHP", "ENVELOPE", "RECYCLED", "LETTERHEAD", "PREPRINTED",
};

static const char *pjl_mediatype(const char *m)
{
    if (!m || !*m) return "PLAIN";
    for (size_t i = 0; i < sizeof MediaTypes / sizeof MediaTypes[0]; i++)
        if (!strcasecmp(m, MediaTypes[i])) return MediaTypes[i];
    fprintf(stderr, "WARNING: rastertoclp620: unknown MediaType \"%s\", using PLAIN\n", m);
    return "PLAIN";
}

/*
 * TIFF 4.0 PackBits, PCL compression mode 2.  Literal runs are (n-1) followed
 * by n bytes, repeats are (257-n) followed by the byte.
 * Worst case src + src/128 + 1.
 */
static size_t packbits(const unsigned char *src, size_t n, unsigned char *dst)
{
    size_t i = 0, o = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 127 && src[i + run] == src[i]) run++;
        if (run > 1) {
            dst[o++] = (unsigned char)(257 - run);
            dst[o++] = src[i];
            i += run;
        } else {
            size_t lit = 1;
            while (i + lit < n && lit < 127 &&
                   !(i + lit + 2 < n && src[i + lit] == src[i + lit + 1] &&
                     src[i + lit] == src[i + lit + 2]))
                lit++;
            dst[o++] = (unsigned char)(lit - 1);
            memcpy(dst + o, src + i, lit);
            o += lit; i += lit;
        }
    }
    return o;
}

/*
 * PCL delta row, compression mode 3.  A control byte holds (count-1) in bits
 * 7-5 and a skip offset in bits 4-0; offset 31 means more offset bytes follow,
 * 255 at a time.  A row identical to the one above codes to nothing, which is
 * what keeps whole-page transfers small.  Worst case 2n + n/255 + 16.
 */
static size_t deltarow(const unsigned char *cur, const unsigned char *prev,
                       size_t n, unsigned char *dst)
{
    size_t i = 0, o = 0, last = 0;
    while (i < n) {
        if (cur[i] == prev[i]) { i++; continue; }
        size_t start = i, cnt = 0;
        while (i < n && cnt < 8 && cur[i] != prev[i]) { i++; cnt++; }
        size_t offset = start - last;
        if (offset < 31) {
            dst[o++] = (unsigned char)(((cnt - 1) << 5) | offset);
        } else {
            dst[o++] = (unsigned char)(((cnt - 1) << 5) | 31);
            size_t rem = offset - 31;
            while (rem >= 255) { dst[o++] = 255; rem -= 255; }
            dst[o++] = (unsigned char)rem;
        }
        memcpy(dst + o, cur + start, cnt);
        o += cnt; last = start + cnt;
    }
    return o;
}

/*
 * PCL adaptive compression, mode 5.  The payload of one ESC*b#W is a sequence
 * of sub-blocks, each introduced by a three-byte header:
 *
 *     [method] [count high] [count low]
 *
 * For methods 0-3 the count is the number of data bytes that follow, and the
 * sub-block carries one row.  For method 5 the count is a repeat count and no
 * data follows at all: it replays the preceding row that many times.  That is
 * the entire reason for using mode 5 here.  A 600 dpi A4 page is 7016 rows and
 * the per-row "ESC*b0W" framing costs about 35 KB whatever the page contains -
 * on a white page that is 95% of the output.  A run of identical rows collapses
 * to three bytes.
 *
 * Method 4, "empty row", is deliberately never emitted.  An empty row is a row
 * of zero bytes, and in the direct-RGB space this filter configures (CID
 * {0,3,0,8,8,8}) zero is black, not white.  It would paint solid black over
 * exactly the sparse pages it looks most attractive for.
 *
 * The firmware side of this was read out of the PCL5Ce interpreter rather than
 * assumed: its ESC*b#M dispatch gives modes 1, 2, 3 and 5 their own handlers
 * and sends 0 and 4 to a common "no decompressor" return, and modes 3 and 5 -
 * and only those two - pass the row length into the same seed-row allocator,
 * which is what delta row and adaptive both need.
 */
#define ADAPT_CHUNK   65536u    /* accumulate this much before one ESC*b#W   */
#define ADAPT_MAXRUN  65535u    /* the sub-block count field is 16 bits      */

struct adapt {
    unsigned char *buf;
    size_t len, cap;
    unsigned dup;               /* duplicate rows seen but not yet emitted   */
};

static void adapt_hdr(struct adapt *a, unsigned method, unsigned count)
{
    a->buf[a->len++] = (unsigned char)method;
    a->buf[a->len++] = (unsigned char)(count >> 8);
    a->buf[a->len++] = (unsigned char)(count & 0xff);
}

/* Emit the pending duplicate run, split to fit the 16-bit count field. */
static void adapt_dupflush(struct adapt *a)
{
    while (a->dup) {
        unsigned n = a->dup > ADAPT_MAXRUN ? ADAPT_MAXRUN : a->dup;
        adapt_hdr(a, 5, n);
        a->dup -= n;
    }
}

/* Hand what has accumulated to the printer as one raster transfer. */
static void adapt_emit(struct adapt *a)
{
    if (!a->len) return;
    printf("\033*b%zuW", a->len);
    fwrite(a->buf, 1, a->len, stdout);
    a->len = 0;
}

/*
 * Source bytes per pixel for the colour spaces this filter converts.  Anything
 * not listed here is rejected in check_header() rather than silently turned
 * into a blank page.
 */
static unsigned bytes_per_pixel(cups_cspace_t cs)
{
    switch (cs) {
        case CUPS_CSPACE_K:
        case CUPS_CSPACE_W:
        case CUPS_CSPACE_SW:    return 1;
        case CUPS_CSPACE_RGB:
        case CUPS_CSPACE_SRGB:  return 3;
        case CUPS_CSPACE_RGBA:
        case CUPS_CSPACE_CMYK:  return 4;
        default:                return 0;
    }
}

/* Reject any raster this filter would otherwise misread.  0 = unusable. */
static int check_header(const cups_page_header2_t *h, int page)
{
    unsigned bpp = bytes_per_pixel(h->cupsColorSpace);

    if (!h->cupsWidth || !h->cupsHeight) {
        fprintf(stderr, "ERROR: rastertoclp620: page %d has zero extent (%ux%u)\n",
                page, h->cupsWidth, h->cupsHeight);
        return 0;
    }
    if (h->cupsBitsPerColor != 8) {
        fprintf(stderr, "ERROR: rastertoclp620: page %d is %u bits per colour, "
                "only 8 is supported\n", page, h->cupsBitsPerColor);
        return 0;
    }
    if (h->cupsColorOrder != CUPS_ORDER_CHUNKED) {
        fprintf(stderr, "ERROR: rastertoclp620: page %d uses colour order %u, "
                "only chunked (%d) is supported\n",
                page, h->cupsColorOrder, CUPS_ORDER_CHUNKED);
        return 0;
    }
    if (!bpp) {
        fprintf(stderr, "ERROR: rastertoclp620: page %d uses unsupported colour "
                "space %u\n", page, h->cupsColorSpace);
        return 0;
    }
    if (h->cupsBytesPerLine < (size_t)h->cupsWidth * bpp) {
        fprintf(stderr, "ERROR: rastertoclp620: page %d claims %u bytes per line, "
                "too short for %u pixels at %u bytes each\n",
                page, h->cupsBytesPerLine, h->cupsWidth, bpp);
        return 0;
    }
    if (!h->HWResolution[0]) {
        fprintf(stderr, "ERROR: rastertoclp620: page %d has no resolution\n", page);
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc < 6 || argc > 7) {
        fputs("ERROR: rastertoclp620: wrong argument count\n", stderr);
        return 1;
    }

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: rastertoclp620: cannot open %s\n", argv[6]);
        return 1;
    }

    const char *title = argv[3] && *argv[3] ? argv[3] : "CUPS";
    int copies = atoi(argv[4]); if (copies < 1) copies = 1;

    /*
     * Options come from cupsd as one "key=value key=value" string; let libcups
     * parse it so quoting and escapes behave the way the rest of CUPS expects.
     * ColorModel is deliberately not read: the authoritative colour space is
     * the one in the raster header, which is what actually describes the bytes.
     */
    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    const char *o_pagesize = cupsGetOption("PageSize",  num_options, options);
    const char *o_duplex   = cupsGetOption("Duplex",    num_options, options);
    const char *o_slot     = cupsGetOption("InputSlot", num_options, options);
    const char *o_media    = cupsGetOption("MediaType", num_options, options);
    const char *o_tsave    = cupsGetOption("TonerSave", num_options, options);
    const char *o_adapt    = cupsGetOption("AdaptiveCompression",
                                           num_options, options);

    const char *media  = pjl_mediatype(o_media);
    const char *duplex = o_duplex ? o_duplex : "None";
    const char *slot   = o_slot   ? o_slot   : "Auto";
    int tonersave = o_tsave && (!strcasecmp(o_tsave, "True") ||
                                !strcasecmp(o_tsave, "On"));
    /*
     * Off unless asked for.  Mode 5 was confirmed present by reading the
     * interpreter's dispatch table, not by printing a page through it, so the
     * conservative default stays until someone has seen it come out right.
     */
    int adaptive = o_adapt && (!strcasecmp(o_adapt, "True") ||
                               !strcasecmp(o_adapt, "On"));

    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = cancel_job;
    sigaction(SIGTERM, &action, NULL);

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) {
        fputs("ERROR: rastertoclp620: cannot read raster stream\n", stderr);
        cupsFreeOptions(num_options, options);
        if (fd) close(fd);
        return 1;
    }

    cups_page_header2_t h;
    int page = 0, failed = 0, started = 0;
    unsigned char *line = NULL, *rgb = NULL, *comp = NULL, *prev = NULL, *dcomp = NULL;
    size_t linecap = 0, rgbcap = 0;
    struct adapt ad = { NULL, 0, 0, 0 };

    while (!Canceled && !failed && cupsRasterReadHeader2(ras, &h)) {
        page++;

        if (!check_header(&h, page)) { failed = 1; break; }

        unsigned W = h.cupsWidth, H = h.cupsHeight, res = h.HWResolution[0];
        size_t rgbn = (size_t)W * 3;

        fprintf(stderr, "PAGE: %d %d\n", page, 1);
        fprintf(stderr, "DEBUG: rastertoclp620: page %d %ux%u @%udpi, cspace %u, "
                "%u band(s)\n", page, W, H, res, h.cupsColorSpace,
                (H + BAND_ROWS - 1) / BAND_ROWS);

        if (page == 1) {
            /*
             * Prefer the PageSize option, fall back to the dimensions the
             * rasteriser recorded, and only then to A4.  Blindly assuming A4
             * would silently mis-size every job that arrives without the
             * option set.
             */
            const char *paper = pjl_paper_by_name(o_pagesize);
            if (!paper)
                paper = pjl_paper_by_size(h.cupsPageSize[0], h.cupsPageSize[1]);
            if (!paper)
                paper = pjl_paper_by_size(h.PageSize[0], h.PageSize[1]);
            if (!paper) {
                fprintf(stderr, "WARNING: rastertoclp620: cannot identify paper "
                        "(PageSize=%s, %.0fx%.0f pt), using A4\n",
                        o_pagesize ? o_pagesize : "unset",
                        h.cupsPageSize[0], h.cupsPageSize[1]);
                paper = "A4";
            }

            started = 1;
            printf("\033%%-12345X@PJL JOB NAME=\"%.60s\"\r\n", title);
            printf("@PJL SET RESOLUTION=%u\r\n", res);
            printf("@PJL SET PAPER=%s\r\n", paper);
            if (copies > 1) printf("@PJL SET COPIES=%d\r\n", copies);
            if (!strcasecmp(duplex, "DuplexNoTumble"))
                printf("@PJL SET DUPLEX=ON\r\n@PJL SET BINDING=LONGEDGE\r\n");
            else if (!strcasecmp(duplex, "DuplexTumble"))
                printf("@PJL SET DUPLEX=ON\r\n@PJL SET BINDING=SHORTEDGE\r\n");
            else
                printf("@PJL SET DUPLEX=OFF\r\n");
            if (!strcasecmp(slot, "Tray1"))       printf("@PJL SET MEDIASOURCE=TRAY1\r\n");
            else if (!strcasecmp(slot, "MPTray")) printf("@PJL SET MEDIASOURCE=MPF\r\n");
            else if (!strcasecmp(slot, "Manual")) printf("@PJL SET MEDIASOURCE=MANUAL\r\n@PJL SET MANUALFEED=ON\r\n");
            else                                  printf("@PJL SET MEDIASOURCE=AUTO\r\n");
            printf("@PJL SET MEDIATYPE=%s\r\n", media);
            /*
             * ECONOMODE, not TONERSAVE.  This firmware's INFO VARIABLES lists
             * ECONOMODE and has no TONERSAVE at all, so the old spelling was
             * accepted by the parser and then dropped on the floor - the PPD's
             * Toner Save control did nothing whatsoever.
             */
            printf("@PJL SET ECONOMODE=%s\r\n", tonersave ? "ON" : "OFF");
            printf("@PJL ENTER LANGUAGE = PCL\r\n");
            fputs("\033E", stdout);
        }

        printf("\033&u%uD", res);                       /* PCL units match the raster */
        printf("\033*t%uR", res);                       /* raster resolution */
        fputs("\033*r0F", stdout);                      /* follow logical page */
        fputs("\033*v6W", stdout);
        { unsigned char cid[6] = {0, 3, 0, 8, 8, 8};    /* devRGB, direct pixel, 8/8/8 */
          fwrite(cid, 1, 6, stdout); }

        /*
         * The input row and the derived RGB row grow independently: a 4-byte
         * per pixel space (RGBA, CMYK) needs a longer input buffer than the
         * 3-byte RGB row it produces, so both caps are tracked separately.
         */
        if (h.cupsBytesPerLine > linecap) {
            free(line);
            linecap = h.cupsBytesPerLine;
            line = malloc(linecap);
        }
        if (rgbn > rgbcap) {
            free(rgb); free(comp); free(prev); free(dcomp);
            free(ad.buf);
            rgbcap = rgbn;
            rgb   = malloc(rgbn);
            comp  = malloc(rgbn + rgbn / 128 + 16);
            prev  = malloc(rgbn);
            dcomp = malloc(2 * rgbn + rgbn / 255 + 16);
            /* room for a full chunk plus the one sub-block that overruns it */
            ad.cap = ADAPT_CHUNK + 2 * rgbn + rgbn / 255 + 32;
            ad.buf = malloc(ad.cap);
            ad.len = ad.dup = 0;
        }
        if (!line || !rgb || !comp || !prev || !dcomp || !ad.buf) {
            fputs("ERROR: rastertoclp620: out of memory\n", stderr);
            failed = 1;
            break;
        }

        /*
         * A sub-block count is 16 bits, so a row whose compressed form could
         * exceed 65535 bytes cannot be expressed in mode 5.  No paper this
         * printer takes comes close - A4 at 600 dpi is 14880 bytes a row - but
         * fall back rather than emit a truncated count if one ever did.
         */
        int use_adapt = adaptive;
        if (use_adapt && rgbn + rgbn / 128 + 16 > ADAPT_MAXRUN) {
            fputs("DEBUG: rastertoclp620: row too wide for adaptive "
                  "compression, using per-row modes\n", stderr);
            use_adapt = 0;
        }

        int curmode = -1;
        for (unsigned y = 0; y < H; y++) {
            if (Canceled) break;

            if (y % BAND_ROWS == 0) {
                if (y) {
                    /*
                     * Nothing may straddle the band boundary: ESC*rC ends the
                     * raster block and the next ESC*r1A reseeds the seed row,
                     * so a duplicate run carried across would replay a row the
                     * printer has already forgotten.
                     */
                    if (use_adapt) { adapt_dupflush(&ad); adapt_emit(&ad); }
                    fputs("\033*rC", stdout);           /* close previous band */
                }
                unsigned rows = H - y < BAND_ROWS ? H - y : BAND_ROWS;
                printf("\033*p0X\033*p%uY", y);         /* cursor to top of band */
                printf("\033*r%uS", W);
                printf("\033*r%uT", rows);
                fputs("\033*r1A", stdout);
                curmode = -1;
                memset(prev, 0, rgbn);                  /* each block reseeds from zero */
                if (use_adapt) {
                    fputs("\033*b5M", stdout);
                    curmode = 5;
                    ad.len = ad.dup = 0;
                }
            }

            /*
             * A short read means the raster was truncated.  Half a page is not
             * a successful job: bail out and let cupsd retry or report it,
             * rather than ejecting a partial page and exiting 0.
             */
            unsigned got = cupsRasterReadPixels(ras, line, h.cupsBytesPerLine);
            if (got != h.cupsBytesPerLine) {
                fprintf(stderr, "ERROR: rastertoclp620: truncated raster on page "
                        "%d row %u of %u (got %u of %u bytes)\n",
                        page, y, H, got, h.cupsBytesPerLine);
                failed = 1;
                break;
            }

            if (h.cupsColorSpace == CUPS_CSPACE_RGB ||
                h.cupsColorSpace == CUPS_CSPACE_SRGB) {
                memcpy(rgb, line, rgbn);
            } else if (h.cupsColorSpace == CUPS_CSPACE_RGBA) {
                /* Straight alpha over an implied white page: drop the channel. */
                for (unsigned x = 0; x < W; x++) {
                    rgb[x*3]   = line[x*4];
                    rgb[x*3+1] = line[x*4+1];
                    rgb[x*3+2] = line[x*4+2];
                }
            } else if (h.cupsColorSpace == CUPS_CSPACE_K) {
                /*
                 * DeviceK is a colorant channel, not luminance: 0 means no
                 * toner (white) and 255 means full black, so it inverts into
                 * RGB.  Do not "fix" this to a pass-through - that is what
                 * CUPS_CSPACE_W and CUPS_CSPACE_SW below are for, and getting
                 * the two confused prints every greyscale page as a negative.
                 */
                for (unsigned x = 0; x < W; x++) {
                    unsigned char v = (unsigned char)(255 - line[x]);
                    rgb[x*3] = rgb[x*3+1] = rgb[x*3+2] = v;
                }
            } else if (h.cupsColorSpace == CUPS_CSPACE_W ||
                       h.cupsColorSpace == CUPS_CSPACE_SW) {
                /* Luminance: 0 is black, 255 is white, same sense as RGB. */
                for (unsigned x = 0; x < W; x++) {
                    unsigned char v = line[x];
                    rgb[x*3] = rgb[x*3+1] = rgb[x*3+2] = v;
                }
            } else {    /* CUPS_CSPACE_CMYK; check_header() rejected the rest */
                for (unsigned x = 0; x < W; x++) {
                    unsigned c = line[x*4], m = line[x*4+1],
                             yy = line[x*4+2], k = line[x*4+3];
                    rgb[x*3]   = (unsigned char)((255 - c)  * (255 - k) / 255);
                    rgb[x*3+1] = (unsigned char)((255 - m)  * (255 - k) / 255);
                    rgb[x*3+2] = (unsigned char)((255 - yy) * (255 - k) / 255);
                }
            }

            if (use_adapt) {
                /*
                 * The first row of a band can never be a duplicate: no row has
                 * been transferred into this raster block yet, so there is no
                 * previous row for the printer to repeat.
                 */
                if (y % BAND_ROWS != 0 && !memcmp(rgb, prev, rgbn)) {
                    ad.dup++;
                } else {
                    adapt_dupflush(&ad);
                    size_t np = packbits(rgb, rgbn, comp);
                    size_t nd = deltarow(rgb, prev, rgbn, dcomp);
                    if (nd <= np) {
                        adapt_hdr(&ad, 3, (unsigned)nd);
                        memcpy(ad.buf + ad.len, dcomp, nd);
                        ad.len += nd;
                    } else {
                        adapt_hdr(&ad, 2, (unsigned)np);
                        memcpy(ad.buf + ad.len, comp, np);
                        ad.len += np;
                    }
                }
                if (ad.len >= ADAPT_CHUNK) adapt_emit(&ad);
            } else {
                size_t np = packbits(rgb, rgbn, comp);
                size_t nd = deltarow(rgb, prev, rgbn, dcomp);
                if (nd <= np) {
                    if (curmode != 3) { fputs("\033*b3M", stdout); curmode = 3; }
                    printf("\033*b%zuW", nd);
                    if (nd) fwrite(dcomp, 1, nd, stdout);
                } else {
                    if (curmode != 2) { fputs("\033*b2M", stdout); curmode = 2; }
                    printf("\033*b%zuW", np);
                    fwrite(comp, 1, np, stdout);
                }
            }
            memcpy(prev, rgb, rgbn);
        }

        if (failed || Canceled) break;

        if (use_adapt) { adapt_dupflush(&ad); adapt_emit(&ad); }
        fputs("\033*rC", stdout);                       /* close last band */
        fputs("\014", stdout);                          /* eject */
    }

    /*
     * Three ways out.  A cancelled job resets the engine and leaves without a
     * PJL EOJ, so the printer does not book the job as finished, and reports
     * failure so cupsd does not treat it as a clean completion.  A truncated
     * or malformed job does the same.  Only a run that emitted every page of
     * every header closes the job normally.
     */
    if (Canceled) {
        fputs("ERROR: rastertoclp620: job cancelled\n", stderr);
        if (started) { fputs("\033*rC\033E", stdout); fputs("\033%-12345X", stdout); }
        failed = 1;
    } else if (failed) {
        if (started) { fputs("\033*rC\033E", stdout); fputs("\033%-12345X", stdout); }
    } else if (page > 0) {
        fputs("\033E", stdout);
        fputs("\033%-12345X", stdout);
        printf("@PJL EOJ NAME=\"%.60s\"\r\n", title);
        fputs("\033%-12345X", stdout);
    } else {
        fputs("ERROR: rastertoclp620: no pages found\n", stderr);
        failed = 1;
    }

    if (fflush(stdout) || ferror(stdout)) {
        fputs("ERROR: rastertoclp620: cannot write to the printer\n", stderr);
        failed = 1;
    }

    free(line); free(rgb); free(comp); free(prev); free(dcomp); free(ad.buf);
    cupsRasterClose(ras);
    cupsFreeOptions(num_options, options);
    if (fd) close(fd);
    fprintf(stderr, "DEBUG: rastertoclp620: %d page(s), %s\n",
            page, failed ? "failed" : "ok");
    return failed ? 1 : 0;
}
