#include "fat_image.h"

#include <stdlib.h>
#include <string.h>

/* Layout — deliberately identical to the card's own geometry so no raw card
   metadata (its boot/FAT region) ever reaches the host:
   [0..rsvd-1]        reserved: boot, FSInfo, backup boot, backup FSInfo (RAM)
   [rsvd..]           FAT #1, FAT #2 — generated on demand, never stored
   [data_start]       cluster 2 = synthetic root dir (1 cluster)
                       cluster 3..  synthetic DCIM dir (v clusters)
                       ...          file data clusters, remapped to the card

   The LUN's synthetic cluster count is capped at the FAT32 minimum (65527), which
   fits inside any card's own data area that mkfs.fat -F 32 would produce, so data
   cluster N of the LUN always maps to data cluster N of the card with no offset.
   Everything a FAT32 reader asks for except file bytes is derivable from the file
   table: the image holds boot + directory bytes plus small per-file index arrays
   and a run-length remap of card sectors (photo chains are almost always one
   contiguous run, so a full card's table fits in a few KB). */

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

static uint32_t run_count(uint64_t r)
{
    return (uint32_t)(r & 0xFFFFFFFFu);
}

static uint32_t run_lba(uint64_t r)
{
    return (uint32_t)(r >> 32);
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
    put_u16(e + 16, 0x2821);                         /* 2000-01-01 (date must be nonzero) */
    put_u16(e + 20, (uint16_t)(cluster >> 16));      /* high word of first cluster */
    put_u16(e + 24, 0x2821);                         /* modification date */
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
    put_u16(b + 14, im->rsvd_sectors);
    b[16] = 2; /* FATs */
    b[21] = 0xF8;       /* media descriptor */
    put_u32(b + 32, im->total_sectors);
    put_u32(b + 36, im->fat_sectors);
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

bool fat_image_build(const fat_image_file_t *files, size_t n, uint16_t spc, uint16_t rsvd_sectors,
                     uint32_t fat_sectors, uint32_t vol_sectors, fat_image_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!files || !n || spc == 0 || (spc & (spc - 1)) != 0 || fat_sectors == 0 || vol_sectors == 0) {
        return false;
    }
    const uint32_t cb = (uint32_t)spc * SECTOR;
    size_t used = 0, nruns = 0;
    for (size_t i = 0; i < n; i++) {
        if (files[i].size == 0 || files[i].nruns == 0 || !name_is_83(files[i].name)) {
            return false;
        }
        uint32_t cl = 0;
        for (uint16_t r = 0; r < files[i].nruns; r++) {
            cl += run_count(files[i].runs[r]);
        }
        const uint32_t need = (files[i].size + cb - 1) / cb;
        if (cl != need) {
            return false;
        }
        used += need;
        nruns += files[i].nruns;
    }
    /* Synthetic directories: root (1 cluster) + DCIM entries in v clusters.
       Synthetic data clusters stay within the FAT32 minimum so their card sectors
       exist on any card whose geometry we accepted. */
    const uint32_t dcim_entries = 2 + (uint32_t)n;
    const uint32_t v = align_up(dcim_entries * 32, cb) / cb;
    const uint32_t first_file = DCIM_CLUSTER + v;
    const uint32_t data_clusters = (uint32_t)used + first_file - ROOT_CLUSTER;
    const uint32_t data_start = (uint32_t)rsvd_sectors + 2 * fat_sectors;
    /* Pad the cluster count up to the FAT32 minimum so every OS reads the volume as
       FAT32; clusters past the last file byte read as zeros. The caller's FAT (the
       card's own) must cover every entry, and the volume must back every sector. */
    const uint32_t total_data = data_clusters < FAT32_MIN_CLUSTS ? FAT32_MIN_CLUSTS : data_clusters;
    if ((uint64_t)fat_sectors * (SECTOR / 4) < (uint64_t)total_data + 2) {
        return false; /* FAT cannot index the LUN */
    }
    const uint64_t lun_end = (uint64_t)data_start + (uint64_t)total_data * spc;
    if (vol_sectors < lun_end) {
        return false; /* volume cannot back the LUN */
    }

    uint8_t *boot = calloc(rsvd_sectors, SECTOR);
    const size_t synth_bytes = (size_t)(1 + v) * cb;
    uint8_t *synth = calloc(1, synth_bytes);
    /* One block holds the per-file indexes then the packed runs; pad so the uint64_t
       runs start 8-aligned (the 16-bit run_cnt can end on an odd offset). */
    const size_t idx_bytes = ((n * (sizeof(uint32_t) * 3 + sizeof(uint16_t)) + 7) & ~(size_t)7) +
                             nruns * sizeof(uint64_t);
    void *idx = calloc(1, idx_bytes);
    if (!boot || !synth || !idx) {
        free(boot);
        free(synth);
        free(idx);
        return false;
    }
    uint32_t *v_start = idx;
    uint32_t *v_end = v_start + n;
    uint32_t *run_ofs = v_end + n;
    uint16_t *run_cnt = (uint16_t *)(run_ofs + n);
    uint64_t *runs = (uint64_t *)(((uintptr_t)(run_cnt + n) + 7) & ~(uintptr_t)7);

    out->spc = spc;
    out->rsvd_sectors = rsvd_sectors;
    out->total_sectors = (uint32_t)lun_end; /* the LUN ends with the last used cluster */
    out->fat_sectors = fat_sectors;
    out->data_start_lba = data_start;
    out->synth_clusters = 1 + v;
    out->first_file = first_file;
    out->data_clusters = data_clusters;
    out->boot = boot;
    out->synth = synth;
    out->synth_bytes = synth_bytes;
    out->v_start = v_start;
    out->v_end = v_end;
    out->runs = runs;
    out->run_ofs = run_ofs;
    out->run_cnt = run_cnt;
    out->nfiles = (uint32_t)n;

    build_boot(out, boot);
    /* FSInfo */
    uint8_t *fs = boot + SECTOR;
    put_u32(fs + 0, 0x41615252u);
    put_u32(fs + 484, 0x61417272u);
    put_u32(fs + 488, total_data - data_clusters); /* free clusters: reserved 0/1 are not free */
    put_u32(fs + 492, first_file + (uint32_t)used);              /* next free cluster */
    fs[510] = 0x55;
    fs[511] = 0xAA;
    /* Backup boot + FSInfo */
    memcpy(boot + 6 * SECTOR, boot, SECTOR);
    memcpy(boot + 7 * SECTOR, fs, SECTOR);

    /* Synthetic directory bytes. FAT32 root dirs carry no "." / ".." entries. */
    dir_entry(synth + 0, "DCIM", 0x10, DCIM_CLUSTER, 0);
    uint8_t *dc = synth + cb; /* DCIM dir starts at cluster 3 */
    dir_entry(dc + 0, ".", 0x10, DCIM_CLUSTER, 0);
    dir_entry(dc + 32, "..", 0x10, ROOT_CLUSTER, 0);
    uint32_t c = first_file;
    size_t ro = 0;
    for (size_t i = 0; i < n; i++) {
        v_start[i] = c;
        for (uint16_t r = 0; r < files[i].nruns; r++) {
            runs[ro++] = files[i].runs[r];
        }
        run_ofs[i] = (uint32_t)(ro - files[i].nruns);
        run_cnt[i] = files[i].nruns;
        c += (uint32_t)((files[i].size + cb - 1) / cb);
        v_end[i] = c;
        dir_entry(dc + 64 + i * 32, files[i].name, 0x20, v_start[i], files[i].size);
    }
    return true;
}

/* File index whose synthetic range holds cluster c, or -1. Binary search: v_start
   is sorted, and MSC reads arrive cluster-sequential so the last hit recurs often —
   but 800 entries binary-search in 10 steps, which is cheap enough unconditionally. */
static int file_of(const fat_image_t *im, uint32_t c)
{
    int lo = 0, hi = (int)im->nfiles - 1;
    while (lo <= hi) {
        const int m = (lo + hi) / 2;
        if (c < im->v_start[m]) {
            hi = m - 1;
        } else if (c >= im->v_end[m]) {
            lo = m + 1;
        } else {
            return m;
        }
    }
    return -1;
}

/* FAT value for synthetic cluster c (entry index c in the FAT). */
static uint32_t fat_value(const fat_image_t *im, uint32_t c)
{
    if (c < ROOT_CLUSTER) {
        return c == 0 ? 0x0FFFFFF8u : 0x0FFFFFFFu;
    }
    if (c >= im->data_clusters + ROOT_CLUSTER) {
        return 0x0FFFFFFFu; /* past our used range: free */
    }
    if (c < DCIM_CLUSTER) {
        return 0x0FFFFFFFu; /* root dir: 1 cluster */
    }
    if (c < im->first_file) {
        const uint32_t last = im->first_file - 1;
        return c == last ? 0x0FFFFFFFu : c + 1; /* DCIM dir chain */
    }
    const int f = file_of(im, c);
    if (f < 0) {
        return 0x0FFFFFFFu; /* gap between files: free */
    }
    return c + 1 == im->v_end[f] ? 0x0FFFFFFFu : c + 1;
}

bool fat_image_read_ram(const fat_image_t *im, uint32_t lba, uint32_t count, uint8_t *buf)
{
    bool all_ram = true;
    if (!im->spc) {
        return false;
    }
    for (uint32_t s = 0; s < count; s++) {
        const uint32_t l = lba + s;
        uint8_t *dst = buf + (size_t)s * SECTOR;
        if (l < im->rsvd_sectors) {
            memcpy(dst, im->boot + (size_t)l * SECTOR, SECTOR);
            continue;
        }
        if (l < im->data_start_lba) {
            /* FAT #1 or #2: generate. Both identical. */
            const uint32_t in_fat = (l - im->rsvd_sectors) % im->fat_sectors; /* sector within a FAT */
            const uint32_t base = in_fat * (SECTOR / 4);                      /* first cluster entry */
            memset(dst, 0, SECTOR);
            for (uint32_t e = 0; e < SECTOR / 4; e++) {
                put_u32(dst + e * 4, fat_value(im, base + e));
            }
            continue;
        }
        const uint32_t rel = l - im->data_start_lba;
        const uint32_t cluster = ROOT_CLUSTER + rel / im->spc;
        const uint32_t off = rel % im->spc;
        if (cluster < im->synth_clusters + ROOT_CLUSTER) {
            memcpy(dst, im->synth + (((size_t)cluster - ROOT_CLUSTER) * im->spc + off) * SECTOR, SECTOR);
            continue;
        }
        all_ram = false;
    }
    return all_ram;
}

bool fat_image_is_card_sector(const fat_image_t *im, uint32_t lba)
{
    if (!im->spc || lba < im->data_start_lba || lba >= im->total_sectors) {
        return false;
    }
    const uint32_t rel = lba - im->data_start_lba;
    return file_of(im, ROOT_CLUSTER + rel / im->spc) >= 0;
}

uint32_t fat_image_card_sector(const fat_image_t *im, uint32_t lba)
{
    if (!im->spc || lba < im->data_start_lba || lba >= im->total_sectors) {
        return 0xFFFFFFFFu;
    }
    const uint32_t rel = lba - im->data_start_lba;
    const uint32_t cluster = ROOT_CLUSTER + rel / im->spc;
    const int f = file_of(im, cluster);
    if (f < 0) {
        return 0xFFFFFFFFu;
    }
    uint32_t i = cluster - im->v_start[f];
    const uint64_t *r = im->runs + im->run_ofs[f];
    for (uint16_t k = 0; k < im->run_cnt[f]; k++) {
        const uint32_t rc = run_count(r[k]);
        if (i < rc) {
            return run_lba(r[k]) + i * im->spc + rel % im->spc;
        }
        i -= rc;
    }
    return 0xFFFFFFFFu;
}

void fat_image_free(fat_image_t *im)
{
    free(im->boot);
    free(im->synth);
    free((void *)im->v_start); /* one block for v_start/v_end/run_ofs/run_cnt/runs */
    memset(im, 0, sizeof(*im));
}
