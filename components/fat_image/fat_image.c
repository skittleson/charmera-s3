#include "fat_image.h"

#include <stdlib.h>
#include <string.h>

/* Layout (all sectors 512 B):
   [0..31]        reserved: boot, FSInfo, backup boot, backup FSInfo
   [32..]         FAT #1, FAT #2 (each fat_sectors long)
   [data_start]   cluster 2 = synthetic root dir (1 cluster)
                  cluster 3..  synthetic DCIM dir (v clusters)
                  ...          file data clusters, remapped to the card
   Cluster count is padded to the FAT32 minimum (65525) so every OS agrees the volume
   is FAT32; padding clusters read as zeros. */

#define RSVD_SECTORS     32
#define FAT32_MIN_CLUSTS 65527
#define ROOT_CLUSTER     2
#define DCIM_CLUSTER     3
#define SECTOR           512

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

static bool name_is_83(const char *n)
{
    size_t len = strlen(n);
    const char *dot = strchr(n, '.');
    if (len > 12 || len < 3) {
        return false;
    }
    if (dot) {
        size_t b = (size_t)(dot - n), e = len - b - 1;
        if (b < 1 || b > 8 || e > 3) {
            return false;
        }
    } else if (len > 8) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c == '.') {
            continue; /* the dot was validated above */
        }
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80 || c == '_')) {
            return false;
        }
    }
    return true;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void dir_entry(uint8_t *e, const char *name83, uint8_t attr, uint32_t cluster, uint32_t size)
{
    memset(e, 0, 32);
    memset(e, ' ', 11); /* name+ext are space padded; everything else stays zero */
    if (name83[0] == '.') { /* "." and "..": literal dots in the name field, no extension */
        e[0] = '.';
        if (name83[1] == '.') {
            e[1] = '.';
        }
    } else {
        const char *dot = strchr(name83, '.');
        const size_t base = dot ? (size_t)(dot - name83) : strlen(name83);
        memcpy(e, name83, base);
        if (dot) {
            memcpy(e + 8, dot + 1, strlen(dot + 1));
        }
    }
    e[11] = attr;
    put_u16(e + 14, 0x0000);       /* creation time */
    put_u16(e + 16, 0x2821);       /* 2000-01-01 (FAT date must be nonzero) */
    put_u16(e + 24, 0x2821);       /* modification date */
    put_u16(e + 26, (uint16_t)cluster);
    put_u32(e + 28, size);
}

static void build_boot(const fat_image_t *im, uint8_t *b)
{
    memset(b, 0, SECTOR);
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "CHARMERA", 8);
    put_u16(b + 11, SECTOR);
    b[13] = (uint8_t)im->spc;
    put_u16(b + 14, RSVD_SECTORS);
    b[16] = 2; /* FATs */
    put_u16(b + 19, 0); /* TotSec16: FAT32 uses TotSec32 */
    b[21] = 0xF8;       /* media descriptor */
    put_u16(b + 22, 0); /* FATSz16: zero for FAT32 */
    put_u16(b + 24, 63);
    put_u16(b + 26, 255);
    put_u32(b + 32, im->total_sectors);
    put_u32(b + 36, (im->image_bytes / SECTOR - RSVD_SECTORS) / 2); /* BPB_FATSz32 */
    put_u32(b + 44, ROOT_CLUSTER); /* BPB_RootClus */
    put_u16(b + 48, 1);            /* BPB_FSInfo */
    put_u16(b + 50, 6);            /* BPB_BackBootSec */
    b[66] = 0x29;                  /* extended boot signature */
    put_u32(b + 67, 0x43484152);
    memcpy(b + 71, "CHARMERA     ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xAA;
}

bool fat_image_build(const fat_image_file_t *files, size_t n, uint16_t spc, fat_image_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!files || spc == 0 || (spc & (spc - 1)) != 0) {
        return false;
    }
    const uint32_t cb = (uint32_t)spc * SECTOR;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (files[i].size == 0 || files[i].size >= 0xFFFFFFFFu || !name_is_83(files[i].name)) {
            return false;
        }
        const uint32_t need = (files[i].size + cb - 1) / cb;
        if (need != files[i].nclusters || need > 0xFFF8u) {
            return false;
        }
        used += need;
    }
    /* Synthetic directories: root (1 cluster) + DCIM entries in v clusters. */
    const uint32_t dcim_entries = 2 + (uint32_t)n;
    const uint32_t v = align_up(dcim_entries * 32, cb) / cb;
    const uint32_t first_file_cluster = DCIM_CLUSTER + v;
    const uint32_t data_clusters = used + first_file_cluster - ROOT_CLUSTER;
    const uint32_t total_data = data_clusters < FAT32_MIN_CLUSTS ? FAT32_MIN_CLUSTS : data_clusters;
    const uint32_t fat_sectors = align_up((total_data + 2) * 4, SECTOR) / SECTOR;
    const uint32_t data_start = RSVD_SECTORS + 2 * fat_sectors;
    const uint64_t total = (uint64_t)data_start + (uint64_t)total_data * spc;
    if (total > 0xFFFFFFFFu) {
        return false;
    }

    out->spc = spc;
    out->total_sectors = (uint32_t)total;
    out->data_start_lba = data_start;
    out->synth_clusters = 1 + v;
    out->image_bytes = (size_t)data_start * SECTOR;
    out->synth_bytes = (size_t)out->synth_clusters * cb;
    out->remap_clusters = used;

    uint8_t *image = malloc(out->image_bytes);
    uint8_t *synth = calloc(1, out->synth_bytes);
    uint32_t *remap = used ? malloc(used * sizeof(uint32_t)) : NULL;
    if (!image || !synth || (used && !remap)) {
        free(image);
        free(synth);
        free(remap);
        return false;
    }
    memset(image, 0, out->image_bytes);
    build_boot(out, image);
    /* FSInfo */
    uint8_t *fs = image + SECTOR;
    put_u32(fs + 0, 0x41615252u);
    put_u32(fs + 484, 0x61417272u);
    put_u32(fs + 488, total_data - (first_file_cluster + (uint32_t)used - ROOT_CLUSTER));
    put_u32(fs + 492, first_file_cluster + (uint32_t)used); /* next cluster */
    fs[510] = 0x55;
    fs[511] = 0xAA;
    /* Backup boot + FSInfo */
    memcpy(image + 6 * SECTOR, image, SECTOR);
    memcpy(image + 7 * SECTOR, fs, SECTOR);

    /* FATs. Entry c describes cluster c (entries 0-1 are reserved media bytes). */
    uint8_t *fat = image + RSVD_SECTORS * SECTOR;
    const uint32_t fat_entries = fat_sectors * (SECTOR / 4);
    for (uint32_t e = 2; e < fat_entries; e++) {
        put_u32(fat + e * 4, 0x0FFFFFFFu); /* free */
    }
    put_u32(fat + 0, 0x0FFFFFF8u);
    put_u32(fat + 4, 0x0FFFFFFFu);
    put_u32(fat + ROOT_CLUSTER * 4, 0x0FFFFFFFu); /* root dir, 1 cluster */
    uint32_t c = first_file_cluster;
    for (size_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < files[i].nclusters; j++) {
            const uint32_t next = (j + 1 == files[i].nclusters) ? 0x0FFFFFFFu : c + 1;
            if (c < fat_entries) {
                put_u32(fat + c * 4, next);
            }
            remap[c - first_file_cluster] = files[i].clusters[j];
            c++;
        }
    }
    /* DCIM dir: "." ".." then files. Its chain is one run starting at DCIM_CLUSTER. */
    for (uint32_t k = 0; k < v; k++) {
        const uint32_t e = DCIM_CLUSTER + k;
        if (e < fat_entries) {
            put_u32(fat + e * 4, (k + 1 == v) ? 0x0FFFFFFFu : DCIM_CLUSTER + k + 1);
        }
    }
    memcpy(fat + fat_sectors * SECTOR, fat, (size_t)fat_sectors * SECTOR); /* FAT #2 */

    /* Synthetic directory bytes. FAT32 root dirs carry no "." / ".." entries. */
    uint8_t *root = synth;
    dir_entry(root + 0, "DCIM", 0x10, DCIM_CLUSTER, 0);
    uint8_t *dc = synth + cb; /* DCIM dir starts at cluster 3 */
    dir_entry(dc + 0, ".", 0x10, DCIM_CLUSTER, 0);
    dir_entry(dc + 32, "..", 0x10, ROOT_CLUSTER, 0);
    c = first_file_cluster;
    for (size_t i = 0; i < n; i++) {
        dir_entry(dc + 64 + i * 32, files[i].name, 0x20, c, files[i].size);
        c += files[i].nclusters;
    }

    out->image = image;
    out->synth = synth;
    out->remap = remap;
    return true;
}

bool fat_image_read_ram(const fat_image_t *im, uint32_t lba, uint32_t count, uint8_t *buf)
{
    bool all_ram = true;
    if (!im->spc) {
        return false;
    }
    for (uint32_t s = 0; s < count; s++) {
        const uint32_t l = lba + s;
        if (l < im->data_start_lba) {
            memcpy(buf + (size_t)s * SECTOR, im->image + (size_t)l * SECTOR, SECTOR);
            continue;
        }
        const uint32_t rel = l - im->data_start_lba;
        const uint32_t cluster = rel / im->spc; /* cluster number minus ROOT_CLUSTER */
        const uint32_t off = rel % im->spc;
        if (cluster < im->synth_clusters) {
            memcpy(buf + (size_t)s * SECTOR, im->synth + ((size_t)cluster * im->spc + off) * SECTOR, SECTOR);
            continue;
        }
        all_ram = false;
    }
    return all_ram;
}

static bool data_cluster_of(const fat_image_t *im, uint32_t lba, uint32_t *cluster, uint32_t *off)
{
    if (!im->spc || lba < im->data_start_lba || lba >= im->total_sectors) {
        return false;
    }
    const uint32_t rel = lba - im->data_start_lba;
    *cluster = rel / im->spc + ROOT_CLUSTER;
    *off = rel % im->spc;
    return true;
}

bool fat_image_is_card_sector(const fat_image_t *im, uint32_t lba)
{
    uint32_t c, off;
    (void)off;
    if (!data_cluster_of(im, lba, &c, &off) || c < ROOT_CLUSTER + im->synth_clusters) {
        return false;
    }
    const uint32_t i = c - ROOT_CLUSTER - im->synth_clusters;
    return i < im->remap_clusters && im->remap[i] != 0;
}

uint32_t fat_image_card_sector(const fat_image_t *im, uint32_t lba)
{
    uint32_t c, off;
    if (!data_cluster_of(im, lba, &c, &off)) {
        return 0xFFFFFFFFu;
    }
    const uint32_t i = c - ROOT_CLUSTER - im->synth_clusters;
    if (i >= im->remap_clusters) {
        return 0xFFFFFFFFu;
    }
    return im->remap[i] + off;
}

void fat_image_free(fat_image_t *im)
{
    free(im->image);
    free(im->synth);
    free(im->remap);
    memset(im, 0, sizeof(*im));
}
