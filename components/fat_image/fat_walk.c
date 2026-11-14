#include "fat_walk.h"

#include <string.h>

#define SECTOR 512
#define MAX_CLUSTERS 4096

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    uint8_t spc;
    uint16_t rsvd;
    uint8_t nfat;
    uint32_t fatsz;      /* sectors per FAT (16- or 32-bit variant, whichever is set) */
    uint32_t root_dir_lba;
    uint32_t root_clus;  /* FAT32 root cluster */
    uint32_t data_lba;   /* first sector of cluster 2 */
    bool fat32;
} vol_t;

static bool parse_boot(fat_read_fn read, void *ctx, vol_t *v)
{
    uint8_t b[SECTOR];
    if (!read(ctx, 0, 1, b) || b[510] != 0x55 || b[511] != 0xAA) {
        return false;
    }
    v->spc = b[13];
    v->rsvd = rd16(b + 14);
    v->nfat = b[16];
    if (!v->spc || (v->spc & (v->spc - 1)) || v->nfat == 0) {
        return false;
    }
    const uint32_t fatsz32 = rd32(b + 36);
    const uint16_t fatsz16 = rd16(b + 22);
    if (rd16(b + 17) == 0) { /* FAT32: no fixed root entries (offset 36 is garbage on FAT16) */
        if (!fatsz32) {
            return false;
        }
        v->fat32 = true;
        v->fatsz = fatsz32;
        v->root_clus = rd32(b + 44);
        v->root_dir_lba = 0; /* not used: FAT32 root is a cluster chain */
        v->data_lba = v->rsvd + (uint32_t)v->nfat * v->fatsz;
    } else if (fatsz16) {
        v->fat32 = false;
        v->fatsz = fatsz16;
        v->data_lba = v->rsvd + (uint32_t)v->nfat * v->fatsz;
        v->root_dir_lba = v->data_lba; /* FAT16 fixed root dir occupies the first sectors */
        v->data_lba += (rd16(b + 17) * 32 + SECTOR - 1) / SECTOR;
        v->root_clus = 0;
    } else {
        return false;
    }
    return true;
}

/* Next cluster after c (0 = bad). *eof true at end of chain. */
static uint32_t fat_next(const vol_t *v, fat_read_fn read, void *ctx, uint32_t c, bool *eof)
{
    const uint32_t off = v->fat32 ? c * 4 : c * 2;
    uint8_t s[SECTOR];
    if (!read(ctx, v->rsvd + off / SECTOR, 1, s)) {
        return 0;
    }
    const uint32_t e = off % SECTOR;
    const uint32_t n = v->fat32 ? (rd32(s + e) & 0x0FFFFFFFu) : rd16(s + e);
    *eof = v->fat32 ? (n >= 0x0FFFFFF8u) : (n >= 0xFFF8u);
    return n;
}

static uint32_t cluster_lba(const vol_t *v, uint32_t c)
{
    return v->data_lba + (c - 2) * v->spc;
}

/* Entry name as "BASE.EXT" (no dot when no extension); fields are space padded. */
static void entry_name(const uint8_t *e, char out[13])
{
    size_t bl = 8;
    while (bl > 0 && (e[bl - 1] == ' ' || e[bl - 1] == '\0')) {
        bl--;
    }
    size_t el = 3;
    while (el > 0 && (e[8 + el - 1] == ' ' || e[8 + el - 1] == '\0')) {
        el--;
    }
    memset(out, 0, 13);
    memcpy(out, e, bl);
    if (el) {
        out[bl] = '.';
        memcpy(out + bl + 1, e + 8, el);
    }
}

/* Scans the fixed FAT16 root area (512 entries) for a directory named `comp`. */
static bool find_in_fixed_root(const vol_t *v, fat_read_fn read, void *ctx, const char *comp, uint32_t *out_clus)
{
    for (uint32_t pos = 0; pos < 16 * SECTOR; pos += 32) {
        uint8_t s[SECTOR];
        if (!read(ctx, v->root_dir_lba + pos / SECTOR, 1, s)) {
            return false;
        }
        const uint8_t *e = s + pos % SECTOR;
        if (e[0] == 0x00) {
            return false;
        }
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08) || !(e[11] & 0x10)) {
            continue;
        }
        char nm[13];
        entry_name(e, nm);
        if (strcmp(nm, comp) == 0) {
            *out_clus = rd16(e + 26);
            return true;
        }
    }
    return false;
}

/* Scans one directory's cluster chain for a subdirectory named `comp`. */
static bool find_subdir(const vol_t *v, fat_read_fn read, void *ctx, uint32_t start_clus, const char *comp,
                        uint32_t *out_clus)
{
    uint32_t c = start_clus;
    bool eof = false;
    while (!eof) {
        for (uint32_t s = 0; s < v->spc; s++) {
            uint8_t sec[SECTOR];
            if (!read(ctx, cluster_lba(v, c) + s, 1, sec)) {
                return false;
            }
            for (uint32_t i = 0; i < SECTOR / 32; i++) {
                const uint8_t *e = sec + i * 32;
                if (e[0] == 0x00) {
                    return false;
                }
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08) || !(e[11] & 0x10)) {
                    continue;
                }
                char nm[13];
                entry_name(e, nm);
                if (nm[0] == '.') {
                    continue; /* "." and ".." */
                }
                if (strcmp(nm, comp) == 0) {
                    *out_clus = rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
                    return true;
                }
            }
        }
        c = fat_next(v, read, ctx, c, &eof);
    }
    return false;
}

bool fat_walk(fat_read_fn read, void *read_ctx, fat_alloc_fn alloc, const char *path, fat_walk_cb cb, void *cb_ctx)
{
    vol_t v;
    if (!parse_boot(read, read_ctx, &v) || !path || !*path) {
        return false;
    }
    /* Resolve each path component. Start at the root: FAT32 root cluster, FAT16 fixed dir. */
    uint32_t dir_clus = v.fat32 ? v.root_clus : 0;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        const size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char comp[13];
        if (n == 0 || n > 12) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            comp[i] = (p[i] >= 'a' && p[i] <= 'z') ? (char)(p[i] - 32) : p[i];
        }
        comp[n] = '\0';
        bool found;
        if (!v.fat32 && dir_clus == 0) {
            found = find_in_fixed_root(&v, read, read_ctx, comp, &dir_clus);
        } else {
            found = find_subdir(&v, read, read_ctx, dir_clus, comp, &dir_clus);
        }
        if (!found) {
            return false;
        }
        p += n;
        if (*p == '/') {
            p++;
        }
    }

    /* List files in the resolved directory. */
    uint32_t c = dir_clus;
    bool eof = false;
    while (!eof) {
        for (uint32_t s = 0; s < v.spc; s++) {
            uint8_t sec[SECTOR];
            if (!read(read_ctx, cluster_lba(&v, c) + s, 1, sec)) {
                return false;
            }
            for (uint32_t i = 0; i < SECTOR / 32; i++) {
                const uint8_t *e = sec + i * 32;
                if (e[0] == 0x00) {
                    return true;
                }
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08) || (e[11] & 0x10)) {
                    continue; /* freed, LFN, label, directory */
                }
                fat_walk_entry_t out;
                entry_name(e, out.name);
                out.size = rd32(e + 28);
                const uint32_t first = rd16(e + 26) | ((uint32_t)rd16(e + 20) << 16);
                if (out.size == 0 || first < 2) {
                    continue;
                }
                uint32_t *arr = alloc(MAX_CLUSTERS * sizeof(uint32_t));
                if (!arr) {
                    return false;
                }
                uint16_t nc = 0;
                uint32_t cc = first;
                bool ceof = false;
                while (nc < MAX_CLUSTERS) {
                    arr[nc++] = cluster_lba(&v, cc);
                    const uint32_t nx = fat_next(&v, read, read_ctx, cc, &ceof);
                    if (ceof) {
                        break;
                    }
                    cc = nx;
                }
                out.clusters = arr;
                out.nclusters = nc;
                if (!cb(cb_ctx, &out)) {
                    return true;
                }
            }
        }
        c = fat_next(&v, read, read_ctx, c, &eof);
    }
    return true;
}
