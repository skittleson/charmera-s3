#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Builds a read-only FAT32 image that shows a host exactly one folder, DCIM, holding
   the gallery files. File bytes are never copied: the image only carries boot and
   directory bytes; FAT sectors are computed on demand, and a file's data clusters are
   remapped onto the SD card's own sectors in run-length form, so a read of any
   synthetic sector resolves either to a small RAM buffer or to real card sectors.

   The image mirrors the card's own geometry (rsvd, FAT size, sectors per cluster) so
   the synthetic volume's data area lines up with the card's data area sector for
   sector: card cluster N backs synthetic cluster N with no offset, and no raw card
   metadata ever reaches the host. */

typedef struct {
    char name[13];      /* 8.3 display name, e.g. "IMG_0001.JPG" */
    uint32_t size;      /* file length in bytes */
    /* Card backing: each run = (card_lba << 32 | cluster_count), cluster-absolute
       sectors; total clusters must equal ceil(size / (spc*512)). */
    const uint64_t *runs;
    uint16_t nruns;
} fat_image_file_t;

typedef struct {
    uint16_t spc;              /* sectors per cluster (mirrors the card) */
    uint16_t rsvd_sectors;     /* reserved region size (mirrors the card) */
    uint32_t total_sectors;    /* LUN size in 512-byte sectors */
    uint32_t fat_sectors;      /* one FAT (mirrors the card) */
    uint32_t data_start_lba;   /* first data-cluster sector */
    uint32_t synth_clusters;   /* leading data clusters served from synth (root dir + DCIM dir) */
    uint32_t first_file;       /* first data cluster holding file bytes */
    uint32_t data_clusters;    /* synthetic clusters in use (root dir .. last file byte) */
    uint8_t *boot;             /* reserved region (boot, FSInfo, backups) */
    uint8_t *synth;            /* synth_clusters clusters of directory bytes */
    size_t synth_bytes;
    /* File table, ordered by synthetic start cluster. */
    uint32_t nfiles;
    uint32_t *v_start;         /* synthetic cluster where each file begins */
    uint32_t *v_end;           /* one past each file's last synthetic cluster */
    const uint64_t *runs;      /* all files' runs, packed */
    uint32_t *run_ofs;
    uint16_t *run_cnt;
} fat_image_t;

/* spc/rsvd_sectors/fat_sectors come from the card volume's BPB; vol_sectors is the
   volume's total sectors. Returns false on bad input or if the gallery does not fit
   inside the card's data area. */
bool fat_image_build(const fat_image_file_t *files, size_t n, uint16_t spc, uint16_t rsvd_sectors,
                     uint32_t fat_sectors, uint32_t vol_sectors, fat_image_t *out);

/* Copies `count` sectors starting at `lba` into `buf` from the image's RAM regions
   (boot, generated FAT, dirs). Card-backed sectors are left untouched — the caller
   fills those via fat_image_is_card_sector + fat_image_card_sector. Returns true if
   all sectors were RAM. */
bool fat_image_read_ram(const fat_image_t *im, uint32_t lba, uint32_t count, uint8_t *buf);

/* True if synthetic LBA `lba` lives on the card rather than in the RAM image. */
bool fat_image_is_card_sector(const fat_image_t *im, uint32_t lba);

/* Absolute card sector backing synthetic LBA `lba` (valid only when fat_image_is_card_sector). */
uint32_t fat_image_card_sector(const fat_image_t *im, uint32_t lba);

void fat_image_free(fat_image_t *im);
