#include "avi_writer.h"

#include <string.h>

/* Offsets of the fields avi_writer_close() patches, and where frame data begins.
   Layout (little-endian):
     0  "RIFF"        4  RIFF size (patched)      8  "AVI "
    12 "LIST"         16 hdrl size=164            20 "hdrl"
    24 "avih"         28 avih size=44             32..75 avih content
    48   dwTotalFrames (patched)                  68 rcFrame.left=w  72 rcFrame.top=h
    76 "LIST"         80 strl size=104            84 "strl"
    88 "strh"         92 strh size=48             96 "vids"  100 "MJPG"
   128   dwLength (patched) = frame count
   144 "strf"        148 strf size=40            152 BITMAPINFOHEADER
   192 "LIST"        196 movi size (patched)     200 "movi"
   204 first frame chunk */
#define OFF_RIFF_SIZE    4
#define OFF_TOTAL_FRAMES 48
#define OFF_C_SAMPLES    128
#define OFF_MOVI_SIZE    196
#define OFF_FRAMES       204

#define FPS      8
#define US_PER_FRAME (1000000 / FPS)

static bool write_all(FILE *f, const void *data, size_t len)
{
    return fwrite(data, 1, len, f) == len;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

bool avi_writer_open(avi_writer_t *w, FILE *f, uint32_t width, uint32_t height)
{
    if (!f || width == 0 || height == 0) {
        return false;
    }
    memset(w, 0, sizeof(*w));
    w->f = f;
    w->width = width;
    w->height = height;

    uint8_t h[AVI_HEADER_SIZE];
    memset(h, 0, sizeof(h));

    memcpy(h + 0, "RIFF", 4);
    put_u32(h + OFF_RIFF_SIZE, 0); /* patched on close */
    memcpy(h + 8, "AVI ", 4);

    /* LIST hdrl */
    memcpy(h + 12, "LIST", 4);
    put_u32(h + 16, 168); /* avih(52) + strl(116) */
    memcpy(h + 20, "hdrl", 4);

    /* avih (44 bytes of content) */
    memcpy(h + 24, "avih", 4);
    put_u32(h + 28, 44);
    put_u32(h + 32, US_PER_FRAME);
    put_u32(h + 36, width * height * 3 * FPS); /* max bytes/sec estimate */
    put_u32(h + 40, 0);                        /* padding */
    put_u32(h + 44, 0);                        /* flags: no index chunk is written */
    put_u32(h + OFF_TOTAL_FRAMES, 0);          /* patched on close */
    put_u32(h + 52, 0);                        /* initial frames */
    put_u32(h + 56, width * height * 3);       /* max buffer size */
    put_u32(h + 60, 0);                        /* quality */
    put_u32(h + 64, 0);                        /* sample size */
    put_u32(h + 68, width);                    /* rcFrame.right */
    put_u32(h + 72, height);                   /* rcFrame.bottom */

    /* LIST strl */
    memcpy(h + 76, "LIST", 4);
    put_u32(h + 80, 104);
    memcpy(h + 84, "strl", 4);

    /* strh (48 bytes of content) */
    memcpy(h + 88, "strh", 4);
    put_u32(h + 92, 48);
    memcpy(h + 96, "vids", 4);
    memcpy(h + 100, "MJPG", 4);
    put_u32(h + 104, 0); /* flags */
    put_u16(h + 108, 0); /* priority */
    put_u16(h + 110, 0); /* language */
    put_u32(h + 112, 0); /* initial frames */
    put_u32(h + 116, US_PER_FRAME); /* scale: ms-per-frame = scale/rate, matching avih */
    put_u32(h + 120, 1000000);      /* rate */
    put_u32(h + 124, 0); /* start */
    put_u32(h + OFF_C_SAMPLES, 0); /* patched on close */
    put_u32(h + 132, width * height * 3); /* suggested buffer */
    put_u32(h + 136, 0); /* quality */
    put_u32(h + 140, 0); /* sample size */

    /* strf: BITMAPINFOHEADER (40 bytes) */
    memcpy(h + 144, "strf", 4);
    put_u32(h + 148, 40);
    put_u32(h + 152, 40); /* biSize */
    put_u32(h + 156, width);
    put_u32(h + 160, height);
    put_u16(h + 164, 1); /* biPlanes */
    put_u16(h + 166, 24); /* biBitCount */
    put_u32(h + 168, 0x47504A4D); /* biCompression 'MJPG' */
    put_u32(h + 172, width * height * 3); /* biSizeImage */

    /* LIST movi (size patched on close) */
    memcpy(h + 192, "LIST", 4);
    put_u32(h + OFF_MOVI_SIZE, 0);
    memcpy(h + 200, "movi", 4);

    return write_all(f, h, sizeof(h));
}

bool avi_writer_frame(avi_writer_t *w, const uint8_t *jpeg, size_t len)
{
    if (!w->f || !jpeg || len == 0) {
        return false;
    }
    uint8_t hdr[8];
    memcpy(hdr, "00dc", 4);
    put_u32(hdr + 4, (uint32_t)len);
    if (!write_all(w->f, hdr, 8) || !write_all(w->f, jpeg, len)) {
        return false;
    }
    if (len & 1) {
        const uint8_t pad = 0;
        if (!write_all(w->f, &pad, 1)) {
            return false;
        }
    }
    w->frame_count++;
    w->data_bytes += (uint32_t)len + (len & 1);
    return true;
}

bool avi_writer_close(avi_writer_t *w)
{
    if (!w->f) {
        return false;
    }
    if (fflush(w->f) != 0) {
        fclose(w->f);
        w->f = NULL;
        return false;
    }
    const long size = ftell(w->f);
    if (size < AVI_HEADER_SIZE) {
        fclose(w->f);
        w->f = NULL;
        return false;
    }
    uint8_t v[4];
    bool ok = true;
    put_u32(v, (uint32_t)(size - 8));
    ok = ok && fseek(w->f, OFF_RIFF_SIZE, SEEK_SET) == 0 && write_all(w->f, v, 4);
    put_u32(v, w->frame_count);
    ok = ok && fseek(w->f, OFF_TOTAL_FRAMES, SEEK_SET) == 0 && write_all(w->f, v, 4);
    ok = ok && fseek(w->f, OFF_C_SAMPLES, SEEK_SET) == 0 && write_all(w->f, v, 4);
    put_u32(v, w->data_bytes);
    ok = ok && fseek(w->f, OFF_MOVI_SIZE, SEEK_SET) == 0 && write_all(w->f, v, 4);
    if (fclose(w->f) != 0) {
        ok = false;
    }
    w->f = NULL;
    return ok;
}

int avi_writer_parse(const char *path, uint8_t *buf, size_t cap, avi_frame_cb cb, void *ctx, bool *truncated)
{
    if (truncated) {
        *truncated = false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    uint8_t hdr[4];
    if (fread(hdr, 1, 4, f) != 4 || memcmp(hdr, "RIFF", 4) != 0) {
        fclose(f);
        return -1;
    }
    int frames = 0;
    bool cut = false;
    if (fseek(f, OFF_FRAMES, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) {
            cut = (feof(f) == 0); /* ran out mid-chunk-header */
            break;
        }
        if (memcmp(c, "00dc", 4) != 0) {
            break; /* not a frame chunk: end of stream or foreign data */
        }
        const uint32_t len = (uint32_t)c[4] | ((uint32_t)c[5] << 8) | ((uint32_t)c[6] << 16) | ((uint32_t)c[7] << 24);
        if (len > cap) {
            cut = true; /* frame bigger than the scratch buffer */
            break;
        }
        if (fread(buf, 1, len, f) != len) {
            cut = true; /* file ends mid-frame */
            break;
        }
        if (len & 1) {
            uint8_t pad;
            if (fread(&pad, 1, 1, f) != 1) {
                cut = true;
                break;
            }
        }
        if (cb && !cb(ctx, buf, len)) {
            break; /* caller stopped the walk */
        }
        frames++;
    }
    fclose(f);
    if (truncated) {
        *truncated = cut;
    }
    return frames;
}
