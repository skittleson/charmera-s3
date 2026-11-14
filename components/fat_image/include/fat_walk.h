#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads `count` 512-byte sectors at `lba` into `buf`. */
typedef bool (*fat_read_fn)(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf);

/* Streaming walk of one directory: begin() is called per file; return false there to
   stop (fat_walk still reports success — what was delivered is complete). Each accepted
   file is followed by its card backing as runs of consecutive clusters
   (volume-relative first data sector of each cluster + how many), then no further call.
   Photo chains are almost always one contiguous run, so this needs no allocation. */
typedef bool (*fat_walk_begin_fn)(void *ctx, const char *name, uint32_t size);
typedef void (*fat_walk_run_fn)(void *ctx, uint32_t volume_lba, uint32_t clusters);

/* Walks `path` (e.g. "DCIM/100CHARM") on a FAT16/FAT32 volume, calling begin/run per
   non-directory, non-label entry. LBAs derive from the volume's own BPB. */
bool fat_walk(fat_read_fn read, void *read_ctx, const char *path, fat_walk_begin_fn begin, fat_walk_run_fn run,
              void *cb_ctx);
