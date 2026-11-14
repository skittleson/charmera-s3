#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reads `count` 512-byte sectors at `lba` into `buf`. */
typedef bool (*fat_read_fn)(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf);
typedef void *(*fat_alloc_fn)(size_t size);

typedef struct {
    char name[13];            /* 8.3 display name, e.g. "IMG_0001.JPG" */
    uint32_t size;
    const uint32_t *clusters; /* card absolute sector of each cluster; valid during cb only */
    uint16_t nclusters;
} fat_walk_entry_t;

typedef bool (*fat_walk_cb)(void *ctx, const fat_walk_entry_t *e);

/* Walks `path` (e.g. "DCIM/100CHARM") on a FAT16/FAT32 volume and calls cb for every
   non-directory, non-label entry. Clusters are absolute card sectors, derived from the
   volume's own BPB so they line up with the card 1:1 for a synthetic FAT.
   alloc backs the cluster arrays; it is never called for entries cb rejects. */
bool fat_walk(fat_read_fn read, void *read_ctx, fat_alloc_fn alloc, const char *path, fat_walk_cb cb, void *cb_ctx);
