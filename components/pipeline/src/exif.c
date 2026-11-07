#include "exif.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5, T_UNDEFINED = 7 };

#define MAX_ENTRIES 24 /* the Exif IFD has 19 */
#define TIFF_START  10 /* FF E1 len(2) "Exif\0\0" */

typedef struct {
    uint16_t tag, type;
    uint32_t count;
    const void *data; /* bytes in file order (already big-endian); NULL = use num */
    uint32_t num;     /* SHORT/LONG value when data == NULL */
} entry_t;

typedef struct {
    entry_t e[MAX_ENTRIES];
    int n;
} ifd_t;

static void add(ifd_t *d, uint16_t tag, uint16_t type, uint32_t count, const void *data, uint32_t num)
{
    if (d->n < MAX_ENTRIES) {
        d->e[d->n++] = (entry_t){tag, type, count, data, num};
    }
}

static void add_str(ifd_t *d, uint16_t tag, const char *s)
{
    if (s) {
        add(d, tag, T_ASCII, (uint32_t)strlen(s) + 1, s, 0);
    }
}

static uint32_t type_size(uint16_t t)
{
    return t == T_SHORT ? 2 : t == T_LONG ? 4 : t == T_RATIONAL ? 8 : 1;
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t ifd_size(const ifd_t *d)
{
    return 2 + 12u * d->n + 4;
}

/* Bytes of out-of-line data an IFD needs (each value padded to an even offset). */
static uint32_t data_size(const ifd_t *d)
{
    uint32_t total = 0;
    for (int i = 0; i < d->n; i++) {
        const uint32_t b = type_size(d->e[i].type) * d->e[i].count;
        if (b > 4) {
            total += (b + 1) & ~1u;
        }
    }
    return total;
}

/* Write the IFD at tiff+at, its data at tiff+*data_at. */
static void write_ifd(uint8_t *tiff, uint32_t at, uint32_t *data_at, const ifd_t *d)
{
    put16(tiff + at, (uint32_t)d->n);
    for (int i = 0; i < d->n; i++) {
        const entry_t *e = &d->e[i];
        uint8_t *p = tiff + at + 2 + 12 * i;
        put16(p, e->tag);
        put16(p + 2, e->type);
        put32(p + 4, e->count);
        memset(p + 8, 0, 4);
        const uint32_t b = type_size(e->type) * e->count;
        if (!e->data) {
            if (e->type == T_SHORT) {
                put16(p + 8, e->num);
            } else {
                put32(p + 8, e->num);
            }
        } else if (b <= 4) {
            memcpy(p + 8, e->data, b);
        } else {
            memcpy(tiff + *data_at, e->data, b);
            put32(p + 8, *data_at);
            *data_at += (b + 1) & ~1u;
        }
    }
    put32(tiff + at + 2 + 12 * d->n, 0); /* no next IFD */
}

size_t exif_build(const exif_info_t *in, uint8_t *out, size_t cap)
{
    static const uint8_t res72[8] = {0, 0, 0, 72, 0, 0, 0, 1};
    char comment[8 + 200];
    size_t comment_len = 0;
    if (in->comment) {
        comment_len = strlen(in->comment);
        if (comment_len > sizeof(comment) - 8) {
            comment_len = sizeof(comment) - 8;
        }
        memcpy(comment, "ASCII\0\0\0", 8);
        memcpy(comment + 8, in->comment, comment_len);
    }
    const char *dt = in->datetime;
    const char *off = dt ? in->offset : NULL;

    /* Entries in ascending tag order, as TIFF requires. */
    ifd_t ifd0 = {.n = 0}, exif = {.n = 0};
    add_str(&ifd0, 0x010e, in->description);
    add_str(&ifd0, 0x010f, in->make);
    add_str(&ifd0, 0x0110, in->model);
    add(&ifd0, 0x0112, T_SHORT, 1, NULL, in->orientation);
    add(&ifd0, 0x011a, T_RATIONAL, 1, res72, 0);
    add(&ifd0, 0x011b, T_RATIONAL, 1, res72, 0);
    add(&ifd0, 0x0128, T_SHORT, 1, NULL, 2); /* inches */
    add_str(&ifd0, 0x0131, in->software);
    add_str(&ifd0, 0x0132, dt);
    add(&ifd0, 0x0213, T_SHORT, 1, NULL, 1); /* centred */
    add(&ifd0, 0x8769, T_LONG, 1, NULL, 0);  /* Exif IFD offset, filled in below */

    add(&exif, 0x9000, T_UNDEFINED, 4, "0232", 0);
    add_str(&exif, 0x9003, dt);
    add_str(&exif, 0x9004, dt);
    add_str(&exif, 0x9010, off);
    add_str(&exif, 0x9011, off);
    add_str(&exif, 0x9012, off);
    add(&exif, 0x9101, T_UNDEFINED, 4, "\x01\x02\x03\x00", 0); /* Y Cb Cr */
    add(&exif, 0x9209, T_SHORT, 1, NULL, 0x20);                /* no flash function */
    if (in->comment) {
        add(&exif, 0x9286, T_UNDEFINED, (uint32_t)(8 + comment_len), comment, 0);
    }
    add(&exif, 0xa000, T_UNDEFINED, 4, "0100", 0);
    add(&exif, 0xa001, T_SHORT, 1, NULL, 1); /* sRGB */
    add(&exif, 0xa002, T_LONG, 1, NULL, in->width);
    add(&exif, 0xa003, T_LONG, 1, NULL, in->height);
    add(&exif, 0xa217, T_SHORT, 1, NULL, 2); /* one-chip colour area sensor */
    add(&exif, 0xa402, T_SHORT, 1, NULL, 0); /* auto exposure */
    add(&exif, 0xa403, T_SHORT, 1, NULL, 0); /* auto white balance */
    add(&exif, 0xa406, T_SHORT, 1, NULL, 0); /* standard scene */
    add_str(&exif, 0xa431, in->serial);

    /* TIFF layout: header(8) IFD0 data0 ExifIFD dataE */
    const uint32_t ifd0_at = 8;
    uint32_t data0_at = ifd0_at + ifd_size(&ifd0);
    const uint32_t exif_at = data0_at + data_size(&ifd0);
    uint32_t datae_at = exif_at + ifd_size(&exif);
    const uint32_t tiff_len = datae_at + data_size(&exif);
    const size_t total = TIFF_START + tiff_len;
    if (total > cap || total - 2 > 0xffff) {
        return 0;
    }
    ifd0.e[ifd0.n - 1].num = exif_at;

    out[0] = 0xff;
    out[1] = 0xe1;
    put16(out + 2, (uint32_t)(total - 2));
    memcpy(out + 4, "Exif\0\0", 6);
    uint8_t *tiff = out + TIFF_START;
    memcpy(tiff, "MM\0\x2a\0\0\0\x08", 8);
    write_ifd(tiff, ifd0_at, &data0_at, &ifd0);
    write_ifd(tiff, exif_at, &datae_at, &exif);
    return total;
}

int exif_read_orientation(const uint8_t *jpeg, size_t len)
{
    /* SOI, APP1 marker, length, "Exif\0\0", TIFF header. */
    if (!jpeg || len < 4 + 2 + 6 + 8 || jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[2] != 0xff || jpeg[3] != 0xe1 ||
        memcmp(jpeg + 6, "Exif\0\0", 6) != 0) {
        return 1;
    }
    size_t seg = (size_t)(jpeg[4] << 8 | jpeg[5]) + 4; /* APP1 ends here */
    if (seg > len) {
        seg = len;
    }
    if (seg < 12 + 8) {
        return 1; /* not even a TIFF header */
    }
    const uint8_t *t = jpeg + 12;
    const size_t tlen = seg - 12;
    const bool be = t[0] == 'M' && t[1] == 'M';
    if (!be && !(t[0] == 'I' && t[1] == 'I')) {
        return 1;
    }
#define RD16(p) (be ? (uint32_t)((p)[0] << 8 | (p)[1]) : (uint32_t)((p)[1] << 8 | (p)[0]))
#define RD32(p) (be ? ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3]) \
                    : ((uint32_t)(p)[3] << 24 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[1] << 8 | (p)[0]))
    const uint32_t ifd = RD32(t + 4);
    if (tlen < 2 || ifd > tlen - 2) { /* no sums that could wrap */
        return 1;
    }
    uint32_t n = RD16(t + ifd);
    const uint32_t fits = (uint32_t)((tlen - ifd - 2) / 12);
    if (n > fits) {
        n = fits;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = t + ifd + 2 + 12 * i;
        if (RD16(e) == 0x0112 && RD16(e + 2) == T_SHORT) {
            const uint32_t v = RD16(e + 8);
            return v >= 1 && v <= 8 ? (int)v : 1;
        }
    }
#undef RD16
#undef RD32
    return 1;
}

void exif_format_datetime(int64_t epoch, int offset_min, char dt[20], char off[7])
{
    const time_t t = (time_t)(epoch + (int64_t)offset_min * 60);
    struct tm tm;
    gmtime_r(&t, &tm);
    /* Range-limit each field so the 19 characters always fit. */
    snprintf(dt, 20, "%04u:%02u:%02u %02u:%02u:%02u", (unsigned)(tm.tm_year + 1900) % 10000u,
             (unsigned)(tm.tm_mon + 1) % 100u, (unsigned)tm.tm_mday % 100u, (unsigned)tm.tm_hour % 100u,
             (unsigned)tm.tm_min % 100u, (unsigned)tm.tm_sec % 100u);
    const int a = offset_min < 0 ? -offset_min : offset_min;
    snprintf(off, 7, "%c%02u:%02u", offset_min < 0 ? '-' : '+', (unsigned)(a / 60) % 100u, (unsigned)a % 60u);
}
