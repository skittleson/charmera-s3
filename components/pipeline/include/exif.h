#pragma once

/* EXIF APP1 segment builder: pure C, host-tested. Big-endian TIFF, IFD0 + Exif IFD. */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *make, *model, *software; /* required */
    const char *serial;                  /* BodySerialNumber, NULL = omit */
    const char *description;             /* ImageDescription, NULL = omit */
    const char *comment;                 /* UserComment (ASCII), NULL = omit */
    const char *datetime;                /* "YYYY:MM:DD HH:MM:SS", NULL = clock not set */
    const char *offset;                  /* "+HH:MM", used only with datetime */
    uint16_t orientation;                /* 1 = upright, 8 = turn 90 deg CCW */
    uint32_t width, height;              /* stored pixel size */
} exif_info_t;

/* Writes FF E1 <len> "Exif\0\0" <TIFF>. Returns the segment's total size, or 0 if it
   doesn't fit in cap (or exceeds the 64 KB APP1 limit). */
size_t exif_build(const exif_info_t *in, uint8_t *out, size_t cap);

/* EXIF Orientation (1..8) of a JPEG file in memory, from the APP1 right after SOI.
   Big- or little-endian TIFF. 1 if there is none or it can't be read. */
int exif_read_orientation(const uint8_t *jpeg, size_t len);

/* Local time for an epoch and UTC offset in minutes: dt "YYYY:MM:DD HH:MM:SS",
   off "+HH:MM". */
void exif_format_datetime(int64_t epoch, int offset_min, char dt[20], char off[7]);
