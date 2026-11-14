#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fat_walk.h"
#include "unity.h"

/* Walks real FAT16 and FAT32 images made by mkfs.fat (files planted with mcopy) and
   checks names, sizes and cluster-run backing round-trip. */

#define LUN16 "/tmp/opencode/walk16.img"
#define LUN32 "/tmp/opencode/walk32.img"

static FILE *lun;

static bool read_cb(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf)
{
    (void)ctx;
    return fseek(lun, (long)lba * 512, SEEK_SET) == 0 && fread(buf, 512, count, lun) == count;
}

typedef struct {
    char names[16][13];
    uint32_t sizes[16];
    uint32_t first_lbas[16];
    uint32_t total_clusters[16];
    int n;
} acc_t;

static bool sh(const char *cmd)
{
    return system(cmd) == 0;
}

void setUp(void) {}
void tearDown(void) {}

static void prepare_images(void)
{
    TEST_ASSERT_TRUE(sh("dd if=/dev/zero of=" LUN16 " bs=1M count=16 2>/dev/null"));
    TEST_ASSERT_TRUE(sh("mkfs.fat -F 16 " LUN16 " >/dev/null 2>&1"));
    TEST_ASSERT_TRUE(sh("dd if=/dev/zero of=" LUN32 " bs=1M count=16 2>/dev/null"));
    TEST_ASSERT_TRUE(sh("mkfs.fat -F 32 " LUN32 " >/dev/null 2>&1"));
    TEST_ASSERT_TRUE(sh("head -c 5000 /dev/urandom > /tmp/opencode/w_a.bin"));
    TEST_ASSERT_TRUE(sh("head -c 70000 /dev/urandom > /tmp/opencode/w_b.bin")); /* spans clusters */
    const char *imgs[] = {LUN16, LUN32};
    for (int i = 0; i < 2; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "MTOOLS_SKIP_CHECK=1 mmd -i %s ::/DCIM && MTOOLS_SKIP_CHECK=1 mmd -i %s ::/DCIM/100CHARM",
                 imgs[i], imgs[i]);
        TEST_ASSERT_TRUE(sh(cmd));
        snprintf(cmd, sizeof(cmd),
                 "MTOOLS_SKIP_CHECK=1 mcopy -o -i %s /tmp/opencode/w_a.bin ::/DCIM/100CHARM/IMG_0001.JPG"
                 " && MTOOLS_SKIP_CHECK=1 mcopy -o -i %s /tmp/opencode/w_b.bin ::/DCIM/100CHARM/IMG_0002.JPG",
                 imgs[i], imgs[i]);
        TEST_ASSERT_TRUE(sh(cmd));
    }
}

/* Accumulator where begin() advances the cursor: run_cb appends to the current file. */
static bool beg2(void *ctx, const char *name, uint32_t size)
{
    acc_t *a = (acc_t *)ctx;
    if (a->n == 16) {
        return false;
    }
    a->n++;
    memcpy(a->names[a->n - 1], name, 13);
    a->sizes[a->n - 1] = size;
    a->first_lbas[a->n - 1] = 0xFFFFFFFFu;
    a->total_clusters[a->n - 1] = 0;
    return true;
}

static void run2(void *ctx, uint32_t volume_lba, uint32_t clusters)
{
    acc_t *a = (acc_t *)ctx;
    if (a->first_lbas[a->n - 1] == 0xFFFFFFFFu) {
        a->first_lbas[a->n - 1] = volume_lba;
    }
    a->total_clusters[a->n - 1] += clusters;
}

static void walk_one(const char *img)
{
    lun = fopen(img, "rb");
    TEST_ASSERT_NOT_NULL(lun);
    acc_t a; memset(&a, 0, sizeof(a));
    TEST_ASSERT_TRUE(fat_walk(read_cb, NULL, "DCIM/100CHARM", beg2, run2, &a));
    TEST_ASSERT_EQUAL_INT(2, a.n);
    TEST_ASSERT_EQUAL_STRING("IMG_0001.JPG", a.names[0]);
    TEST_ASSERT_EQUAL_STRING("IMG_0002.JPG", a.names[1]);
    TEST_ASSERT_EQUAL_UINT32(5000, a.sizes[0]);
    TEST_ASSERT_EQUAL_UINT32(70000, a.sizes[1]);
    TEST_ASSERT_TRUE(a.total_clusters[0] >= 1);   /* 5000 B over any cluster size */
    TEST_ASSERT_TRUE(a.total_clusters[1] >= 7);

    /* The reported first cluster sector must actually hold the file's first bytes. */
    uint8_t sec[512];
    fseek(lun, (long)a.first_lbas[0] * 512, SEEK_SET);
    TEST_ASSERT_EQUAL_INT(512, (int)fread(sec, 1, 512, lun));
    uint8_t want[5000];
    FILE *f = fopen("/tmp/opencode/w_a.bin", "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(5000, (int)fread(want, 1, 5000, f));
    fclose(f);
    TEST_ASSERT_EQUAL_MEMORY(want, sec, 512);
    fclose(lun);
}

static void test_walk_fat16(void)
{
    walk_one(LUN16);
}

static void test_walk_fat32(void)
{
    walk_one(LUN32);
}

static void test_walk_missing_path(void)
{
    lun = fopen(LUN32, "rb");
    acc_t a; memset(&a, 0, sizeof(a));
    TEST_ASSERT_FALSE(fat_walk(read_cb, NULL, "DCIM/NOPE", beg2, run2, &a));
    TEST_ASSERT_FALSE(fat_walk(read_cb, NULL, "NOSUCH", beg2, run2, &a));
    fclose(lun);
}

static void test_walk_stops_when_begin_refuses(void)
{
    /* First begin() returns false: walk reports success with zero files. */
    lun = fopen(LUN32, "rb");
    acc_t a; memset(&a, 0, sizeof(a));
    a.n = 16; /* begin refuses immediately */
    TEST_ASSERT_TRUE(fat_walk(read_cb, NULL, "DCIM/100CHARM", beg2, run2, &a));
    TEST_ASSERT_EQUAL_INT(16, a.n);
    fclose(lun);
}

int main(void)
{
    if (!sh("command -v mkfs.fat >/dev/null 2>&1") || !sh("command -v mcopy >/dev/null 2>&1")) {
        printf("mkfs.fat or mtools missing; skipping\n");
        return 0;
    }
    prepare_images();
    UNITY_BEGIN();
    RUN_TEST(test_walk_fat16);
    RUN_TEST(test_walk_fat32);
    RUN_TEST(test_walk_missing_path);
    RUN_TEST(test_walk_stops_when_begin_refuses);
    return UNITY_END();
}
