#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Builds a read-only FAT32 image that shows a host exactly one folder, DCIM, holding
   the gallery files. File bytes are never copied: the image only carries the boot
   sector, FATs and directory entries; a file's data clusters are remapped 1:1 onto the
   SD card's own clusters (same bytes-per-cluster), so a read of any synthetic sector
   resolves either to a small RAM buffer (boot/FAT/dirs) or to a real card sector. */

typedef struct {
    char name[13];             /* 8.3 display name, e.g. "IMG_0001.JPG" */
    uint32_t size;             /* file length in bytes */
    const uint32_t *clusters;  /* card absolute sector of each cluster, ceil(size/cluster) entries */
    uint16_t nclusters;
} fat_image_file_t;

typedef struct {
    uint16_t spc;              /* sectors per cluster (mirrors the card) */
    uint32_t total_sectors;    /* LUN size in 512-byte sectors */
    uint32_t data_start_lba;   /* first data-cluster sector */
    uint32_t synth_clusters;   /* leading data clusters served from synth (root dir + DCIM dir) */
    uint8_t *image;            /* reserved+FAT region, image_bytes long (PSRAM) */
    size_t image_bytes;
    uint8_t *synth;            /* synth_clusters clusters of directory bytes (PSRAM) */
    size_t synth_bytes;
    uint32_t *remap;           /* one card start-sector per data cluster (PSRAM); unused for synth clusters */
    size_t remap_clusters;
} fat_image_t;

/* cluster_bytes = spc * 512. Returns false on bad input or if sizes overflow FAT32. */
bool fat_image_build(const fat_image_file_t *files, size_t n, uint16_t spc, fat_image_t *out);

/* Copies `count` sectors starting at `lba` into `buf` from the image's RAM regions
   (boot/FAT/dirs). Card-backed sectors are left untouched — the caller fills those via
   fat_image_is_card_sector + fat_image_card_sector. Returns true if all sectors were RAM. */
bool fat_image_read_ram(const fat_image_t *im, uint32_t lba, uint32_t count, uint8_t *buf);

/* True if synthetic LBA `lba` lives on the card rather than in the RAM image. */
bool fat_image_is_card_sector(const fat_image_t *im, uint32_t lba);

/* Absolute card sector backing synthetic LBA `lba` (valid only when fat_image_is_card_sector). */
uint32_t fat_image_card_sector(const fat_image_t *im, uint32_t lba);

void fat_image_free(fat_image_t *im);
