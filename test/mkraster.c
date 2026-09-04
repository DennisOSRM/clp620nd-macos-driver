/* Synthetic CUPS raster generator for testing rastertoclp620. */
#include <cups/raster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 11) {
        fputs("usage: mkraster out cspace bpc order W H res pages pattern rows_to_write [pw ph]\n", stderr);
        return 2;
    }
    const char *out = argv[1];
    unsigned cs = atoi(argv[2]), bpc = atoi(argv[3]), order = atoi(argv[4]);
    unsigned W = atoi(argv[5]), H = atoi(argv[6]), res = atoi(argv[7]);
    unsigned pages = atoi(argv[8]);
    const char *pat = argv[9];
    int rows = atoi(argv[10]);      /* -1 = all H rows */
    double pw = argc > 11 ? atof(argv[11]) : 595.0;
    double ph = argc > 12 ? atof(argv[12]) : 842.0;
    if (rows < 0) rows = (int)H;

    unsigned nc;
    switch (cs) {
        case CUPS_CSPACE_K: case CUPS_CSPACE_W: case CUPS_CSPACE_SW: nc = 1; break;
        case CUPS_CSPACE_RGB: case CUPS_CSPACE_SRGB: nc = 3; break;
        case CUPS_CSPACE_RGBA: case CUPS_CSPACE_CMYK: nc = 4; break;
        default: nc = 3; break;
    }
    unsigned bpl = W * nc * bpc / 8;

    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    cups_raster_t *r = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
    if (!r) { fputs("cupsRasterOpen failed\n", stderr); return 1; }

    unsigned char *line = calloc(1, bpl ? bpl : 1);

    for (unsigned p = 0; p < pages; p++) {
        cups_page_header2_t h;
        memset(&h, 0, sizeof h);
        h.HWResolution[0] = h.HWResolution[1] = res;
        h.cupsWidth = W; h.cupsHeight = H;
        h.cupsBitsPerColor = bpc;
        h.cupsBitsPerPixel = bpc * nc;
        h.cupsBytesPerLine = bpl;
        h.cupsColorOrder = order;
        h.cupsColorSpace = cs;
        h.cupsNumColors = nc;
        h.PageSize[0] = (unsigned)pw; h.PageSize[1] = (unsigned)ph;
        h.cupsPageSize[0] = (float)pw; h.cupsPageSize[1] = (float)ph;
        h.cupsImagingBBox[0] = 0; h.cupsImagingBBox[1] = 0;
        h.cupsImagingBBox[2] = (float)pw; h.cupsImagingBBox[3] = (float)ph;
        h.NumCopies = 1;
        if (!cupsRasterWriteHeader2(r, &h)) {
            fputs("write header failed\n", stderr); return 1;
        }
        for (int y = 0; y < rows; y++) {
            if (!strncmp(pat, "fill:", 5)) {
                memset(line, atoi(pat + 5) & 0xff, bpl);
            } else if (!strcmp(pat, "ramp")) {
                for (unsigned x = 0; x < W; x++) {
                    unsigned char v = W > 1 ? (unsigned char)(x * 255 / (W - 1)) : 0;
                    for (unsigned c = 0; c < nc; c++) line[x * nc + c] = v;
                }
            } else if (!strcmp(pat, "rgbtest")) {
                /* r ramps with x, g fixed 128, b tracks y */
                for (unsigned x = 0; x < W; x++) {
                    line[x * nc + 0] = W > 1 ? (unsigned char)(x * 255 / (W - 1)) : 0;
                    if (nc > 1) line[x * nc + 1] = 128;
                    if (nc > 2) line[x * nc + 2] = (unsigned char)(y & 0xff);
                    if (nc > 3) line[x * nc + 3] = 200;   /* alpha / K */
                }
            }
            if (cupsRasterWritePixels(r, line, bpl) < bpl) {
                fputs("write pixels failed\n", stderr); return 1;
            }
        }
    }
    cupsRasterClose(r);
    close(fd);
    return 0;
}
