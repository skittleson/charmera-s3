#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Minimal MJPEG-in-AVI writer and reader. The only AVI knowledge in the project.
   Pure C, no ESP-IDF headers: host-tested and reused on-device.

   Layout (little-endian): RIFF/AVI header with one 'MJPG' video stream, sizes left as
   placeholders and patched on close. Frames are '00dc' chunks, zero-padded to even. */

#define AVI_HEADER_SIZE 204 /* bytes written by avi_writer_open() before the first frame */

typedef struct {
    FILE *f;
    uint32_t width, height;
    uint32_t frame_count;
    uint32_t data_bytes; /* movi data written so far (frames + padding) */
} avi_writer_t;

/* Write the RIFF/AVI header for one MJPEG stream at `width` x `height`, 8 fps. The
   RIFF, movi and frame-count sizes are placeholders patched by avi_writer_close(). */
bool avi_writer_open(avi_writer_t *w, FILE *f, uint32_t width, uint32_t height);

/* Append one JPEG frame as a '00dc' chunk (zero-padded to even). */
bool avi_writer_frame(avi_writer_t *w, const uint8_t *jpeg, size_t len);

/* Patch the header sizes and close the file. Returns false on write failure (the file
   is then left truncated; the rescan deletes leftover .TMP files). */
bool avi_writer_close(avi_writer_t *w);

/* Walk the '00dc' chunks of a clip, calling cb(ctx, buf, len) for each frame. `buf` is a
   caller-provided scratch buffer (PSRAM on-device) that must hold the largest frame;
   frames larger than `cap` stop the walk and set *truncated. A clean end-of-file stops
   with *truncated false; a cut file stops at the first incomplete chunk with it true.
   `start_frame` skips that many complete frames before the first callback (for sequential
   playback). Returns the total number of complete frames in the file, or -1 if the file is
   not an AVI clip. */
typedef bool (*avi_frame_cb)(void *ctx, const uint8_t *jpeg, size_t len);
int avi_writer_parse(const char *path, uint8_t *buf, size_t cap, int start_frame, avi_frame_cb cb, void *ctx,
                     bool *truncated);

/* Streaming reader for playback: keeps the file open and yields one frame per call,
   in order, without re-reading earlier frames. NULL if the file is not an AVI clip. */
typedef struct {
    FILE *f;
} avi_reader_t;
avi_reader_t *avi_reader_open(const char *path);
/* Read the next frame into buf (cap bytes). Returns its length, or 0 at end of stream. */
size_t avi_reader_next(avi_reader_t *r, uint8_t *buf, size_t cap);
void avi_reader_close(avi_reader_t *r);
