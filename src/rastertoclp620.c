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
 * Contone RGB throughout.  Host halftoning to 1-bit CMY was tried and
 * abandoned: the device accepts CID {1,3,0,1,1,1} and then ignores the depth,
 * consuming the row as 8-bit pixels, so the image prints at one eighth width.
 * Sending continuous tone and letting the engine halftone is both correct and
 * better looking.
 *
 * Page geometry, duplex, tray, media and copies are set with PJL, using the
 * enum spellings the firmware reports via INFO CONFIG / INFO VARIABLES.
 */

#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#define BAND_ROWS 1704      /* comfortably under the observed row ceiling */

static int Canceled = 0;
static void cancel_job(int sig) { (void)sig; Canceled = 1; }

/* PPD PageSize keyword -> PJL PAPER name (from @PJL INFO CONFIG PAPERS). */
static const char *pjl_paper(const char *ps)
{
    if (!ps || !*ps)                           return "A4";
    if (!strcasecmp(ps, "Letter"))             return "LETTER";
    if (!strcasecmp(ps, "Legal"))              return "LEGAL";
    if (!strcasecmp(ps, "Executive"))          return "EXECUTIVE";
    if (!strcasecmp(ps, "B5"))                 return "JISB5";
    if (!strcasecmp(ps, "ISOB5"))              return "ISOB5";
    if (!strcasecmp(ps, "A5"))                 return "A5";
    if (!strcasecmp(ps, "A6"))                 return "A6";
    if (!strcasecmp(ps, "FanFoldGermanLegal")) return "FOLIO";
    if (!strcasecmp(ps, "8.5x13.5"))           return "OFICIO";
    if (!strcasecmp(ps, "Statement"))          return "STATEMENT";
    if (!strcasecmp(ps, "Env10"))              return "NO10ENV";
    if (!strcasecmp(ps, "EnvMonarch"))         return "MONARCHENV";
    if (!strcasecmp(ps, "EnvDL"))              return "DLENV";
    if (!strcasecmp(ps, "EnvC5"))              return "C5ENV";
    if (!strcasecmp(ps, "EnvC6"))              return "C6ENV";
    return "A4";
}

/* Look up one "Key=Value" out of the CUPS options string; last wins. */
static void get_opt(const char *opts, const char *key, char *buf, size_t len, const char *def)
{
    const char *p = opts;
    size_t klen = strlen(key);
    buf[0] = '\0';
    while (p && *p) {
        while (*p == ' ') p++;
        if (!strncasecmp(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1, *e = strchr(v, ' ');
            size_t n = e ? (size_t)(e - v) : strlen(v);
            if (n >= len) n = len - 1;
            memcpy(buf, v, n); buf[n] = '\0';
        }
        p = strchr(p, ' ');
    }
    if (!buf[0]) { strncpy(buf, def, len - 1); buf[len - 1] = '\0'; }
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

    const char *title = argv[3], *opts = argv[5];
    int copies = atoi(argv[4]); if (copies < 1) copies = 1;

    char pagesize[64], colormodel[32], duplex[32], slot[32], media[32], tsave[16];
    get_opt(opts, "PageSize",   pagesize,   sizeof pagesize,   "A4");
    get_opt(opts, "ColorModel", colormodel, sizeof colormodel, "RGB");
    get_opt(opts, "Duplex",     duplex,     sizeof duplex,     "None");
    get_opt(opts, "InputSlot",  slot,       sizeof slot,       "Auto");
    get_opt(opts, "MediaType",  media,      sizeof media,      "PLAIN");
    get_opt(opts, "TonerSave",  tsave,      sizeof tsave,      "False");

    struct sigaction action;
    memset(&action, 0, sizeof action);
    sigemptyset(&action.sa_mask);
    action.sa_handler = cancel_job;
    sigaction(SIGTERM, &action, NULL);

    cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!ras) { fputs("ERROR: rastertoclp620: cannot read raster stream\n", stderr); return 1; }

    cups_page_header2_t h;
    int page = 0;
    unsigned char *line = NULL, *rgb = NULL, *comp = NULL, *prev = NULL, *dcomp = NULL;
    size_t linecap = 0;

    while (!Canceled && cupsRasterReadHeader2(ras, &h)) {
        page++;
        fprintf(stderr, "PAGE: %d %d\n", page, 1);

        unsigned W = h.cupsWidth, H = h.cupsHeight, res = h.HWResolution[0];
        size_t rgbn = (size_t)W * 3;
        fprintf(stderr, "DEBUG: rastertoclp620: page %d %ux%u @%udpi, %u band(s)\n",
                page, W, H, res, (H + BAND_ROWS - 1) / BAND_ROWS);

        if (page == 1) {
            printf("\033%%-12345X@PJL JOB NAME=\"%.60s\"\r\n", title ? title : "CUPS");
            printf("@PJL SET RESOLUTION=%u\r\n", res);
            printf("@PJL SET PAPER=%s\r\n", pjl_paper(pagesize));
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
            printf("@PJL SET TONERSAVE=%s\r\n",
                   (!strcasecmp(tsave, "True") || !strcasecmp(tsave, "On")) ? "ON" : "OFF");
            printf("@PJL ENTER LANGUAGE = PCL\r\n");
            fputs("\033E", stdout);
        }

        printf("\033&u%uD", res);                       /* PCL units match the raster */
        printf("\033*t%uR", res);                       /* raster resolution */
        fputs("\033*r0F", stdout);                      /* follow logical page */
        fputs("\033*v6W", stdout);
        { unsigned char cid[6] = {0, 3, 0, 8, 8, 8};    /* devRGB, direct pixel, 8/8/8 */
          fwrite(cid, 1, 6, stdout); }

        if (rgbn > linecap || !line) {
            free(line); free(rgb); free(comp); free(prev); free(dcomp);
            linecap = rgbn > h.cupsBytesPerLine ? rgbn : h.cupsBytesPerLine;
            line  = malloc(linecap);
            rgb   = malloc(rgbn);
            comp  = malloc(rgbn + rgbn / 128 + 16);
            prev  = malloc(rgbn);
            dcomp = malloc(2 * rgbn + rgbn / 255 + 16);
            if (!line || !rgb || !comp || !prev || !dcomp) {
                fputs("ERROR: rastertoclp620: out of memory\n", stderr);
                cupsRasterClose(ras); return 1;
            }
        }

        int curmode = -1;
        for (unsigned y = 0; y < H && !Canceled; y++) {
            if (y % BAND_ROWS == 0) {
                if (y) fputs("\033*rC", stdout);        /* close previous band */
                unsigned rows = H - y < BAND_ROWS ? H - y : BAND_ROWS;
                printf("\033*p0X\033*p%uY", y);         /* cursor to top of band */
                printf("\033*r%uS", W);
                printf("\033*r%uT", rows);
                fputs("\033*r1A", stdout);
                curmode = -1;
                memset(prev, 0, rgbn);                  /* each block reseeds from zero */
            }

            if (cupsRasterReadPixels(ras, line, h.cupsBytesPerLine) < 1) break;

            if (h.cupsColorSpace == CUPS_CSPACE_RGB ||
                h.cupsColorSpace == CUPS_CSPACE_SRGB) {
                memcpy(rgb, line, rgbn);
            } else if (h.cupsColorSpace == CUPS_CSPACE_K ||
                       h.cupsColorSpace == CUPS_CSPACE_W ||
                       h.cupsColorSpace == CUPS_CSPACE_SW) {
                int invert = (h.cupsColorSpace == CUPS_CSPACE_K);
                for (unsigned x = 0; x < W; x++) {
                    unsigned char v = line[x];
                    if (invert) v = 255 - v;
                    rgb[x*3] = rgb[x*3+1] = rgb[x*3+2] = v;
                }
            } else if (h.cupsColorSpace == CUPS_CSPACE_CMYK) {
                for (unsigned x = 0; x < W; x++) {
                    unsigned c = line[x*4], m = line[x*4+1],
                             yy = line[x*4+2], k = line[x*4+3];
                    rgb[x*3]   = (unsigned char)((255 - c)  * (255 - k) / 255);
                    rgb[x*3+1] = (unsigned char)((255 - m)  * (255 - k) / 255);
                    rgb[x*3+2] = (unsigned char)((255 - yy) * (255 - k) / 255);
                }
            } else {
                memset(rgb, 255, rgbn);
            }

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
            memcpy(prev, rgb, rgbn);
        }

        fputs("\033*rC", stdout);                       /* close last band */
        fputs("\014", stdout);                          /* eject */
    }

    if (page > 0) {
        fputs("\033E", stdout);
        fputs("\033%-12345X", stdout);
        printf("@PJL EOJ NAME=\"%.60s\"\r\n", title ? title : "CUPS");
        fputs("\033%-12345X", stdout);
    } else {
        fputs("ERROR: rastertoclp620: no pages found\n", stderr);
    }

    free(line); free(rgb); free(comp); free(prev); free(dcomp);
    cupsRasterClose(ras);
    if (fd) close(fd);
    fprintf(stderr, "DEBUG: rastertoclp620: %d page(s)\n", page);
    return page ? 0 : 1;
}
