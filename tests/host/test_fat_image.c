#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fat_image.h"
#include "unity.h"

/* Verification bar from the spec: a real FAT reader sees DCIM/IMG_*.JPG with byte-exact
   contents. No root here, so mtools stands in for a loop-mount; skipped when absent. */

static uint8_t card[70000 * 512]; /* pretend SD card: 512-byte sectors at arbitrary spots */
#define CARD_SECTORS (sizeof(card) / 512)

#define OUT "/tmp/opencode/fat_image_lun.bin"

static void fill_pattern(uint8_t *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed * 31 + i * 7 + (i >> 8));
    }
}

/* Writes the whole synthetic LUN to OUT the way the MSC callback would: every LBA goes
   through read_ram, and card-backed LBAs are pulled from the pretend card via remap. */
static void write_lun(const fat_image_t *im)
{
    FILE *f = fopen(OUT, "wb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t sec[512];
    for (uint32_t lba = 0; lba < im->total_sectors; lba++) {
        memset(sec, 0, sizeof(sec));
        fat_image_read_ram(im, lba, 1, sec);
        if (fat_image_is_card_sector(im, lba)) {
            uint32_t cs = fat_image_card_sector(im, lba);
            TEST_ASSERT_TRUE((size_t)(cs + 1) * 512 <= sizeof(card));
            memcpy(sec, card + (size_t)cs * 512, 512);
        }
        fwrite(sec, 1, 512, f);
    }
    fclose(f);
}

static bool tool_exists(const char *name)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", name);
    return system(cmd) == 0;
}

void setUp(void) {}
void tearDown(void) {}

static void test_build_and_read_back(void)
{
    /* Pretend files on the card, clusters deliberately scattered and out of order.
       spc = 1 so each cluster is one sector; runs of 1 mark scattered placement. */
    fill_pattern(card + 1000 * 512, 1500, 1);
    fill_pattern(card + 5000 * 512, 512, 2);
    fill_pattern(card + 2000 * 512, 512, 3);
    fill_pattern(card + 2001 * 512, 476, 4); /* contiguous second cluster of file 2 */
    fill_pattern(card + 9000 * 512, 100, 5);

    /* run = (card_lba << 32 | clusters) */
    static const uint64_t r1[] = {1000ull << 32 | 3};
    static const uint64_t r2[] = {5000ull << 32 | 1, 2000ull << 32 | 2};
    static const uint64_t r3[] = {9000ull << 32 | 1};
    fat_image_file_t files[3] = {
        {"IMG_0001.JPG", 1500, r1, 1},
        {"IMG_0002.JPG", 1500, r2, 2},
        {"IMG_0002.THM", 100, r3, 1},
    };
    fat_image_t im;
    TEST_ASSERT_TRUE(fat_image_build(files, 3, 1, 32, 512, CARD_SECTORS, &im));
    TEST_ASSERT_EQUAL_UINT16(1, im.spc);
    TEST_ASSERT_NOT_NULL(im.boot);
    TEST_ASSERT_NOT_NULL(im.synth);
    write_lun(&im);

    /* Boot sector sanity via read_ram. */
    uint8_t b[512];
    TEST_ASSERT_TRUE(fat_image_read_ram(&im, 0, 1, b));
    TEST_ASSERT_EQUAL_HEX8(0x29, b[66]);             /* extended boot signature */
    TEST_ASSERT_EQUAL_MEMORY("FAT32   ", b + 82, 8); /* system ID */
    TEST_ASSERT_EQUAL_UINT16(512, b[11] | (b[12] << 8));
    TEST_ASSERT_EQUAL_UINT16(1, b[13]);    /* sectors per cluster */
    TEST_ASSERT_EQUAL_UINT16(2, b[16]);    /* two FATs */
    TEST_ASSERT_EQUAL_UINT32(im.total_sectors, b[32] | (b[33] << 8) | (b[34] << 16) | ((uint32_t)b[35] << 24));
    TEST_ASSERT_EQUAL_HEX8(0x55, b[510]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, b[511]);

    /* FAT sectors beyond the first must hold their own entries (by sector*128),
       not entries re-derived from sector 0. Clusters 128..255 are all padding
       (free): every entry must be 0x0FFFFFFF. FAT #2 must match FAT #1. */
    uint8_t b1[512], b2[512];
    TEST_ASSERT_TRUE(im.fat_sectors > 1);
    TEST_ASSERT_TRUE(fat_image_read_ram(&im, im.rsvd_sectors + 1, 1, b1));
    for (int e = 0; e < 512 / 4; e++) {
        uint32_t v = b1[e * 4] | (b1[e * 4 + 1] << 8) | (b1[e * 4 + 2] << 16) | ((uint32_t)b1[e * 4 + 3] << 24);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x0FFFFFFFu, v, "FAT sector 1 must be all-free entries");
    }
    TEST_ASSERT_TRUE(fat_image_read_ram(&im, im.rsvd_sectors + im.fat_sectors + 1, 1, b2));
    TEST_ASSERT_EQUAL_MEMORY(b1, b2, 512);
    fat_image_free(&im);
}

static void test_mtools_reads_gallery(void)
{
    if (!tool_exists("mdir") || !tool_exists("mcopy")) {
        TEST_IGNORE_MESSAGE("mtools not installed");
    }
    /* LUN from the previous test is on disk; a fresh reader must see exactly the gallery. */
    int rc = system("MTOOLS_SKIP_CHECK=1 mdir -i " OUT " ::/DCIM > /tmp/opencode/fat_image_mdir.txt 2>&1");
    TEST_ASSERT_EQUAL_INT(0, rc);
    FILE *f = fopen("/tmp/opencode/fat_image_mdir.txt", "r");
    TEST_ASSERT_NOT_NULL(f);
    char txt[4096] = {0};
    size_t tn = fread(txt, 1, sizeof(txt) - 1, f);
    TEST_ASSERT_TRUE(tn > 0);
    fclose(f);
    TEST_ASSERT_NOT_NULL(strstr(txt, "IMG_0001 JPG"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "IMG_0002 JPG"));
    TEST_ASSERT_NOT_NULL(strstr(txt, "IMG_0002 THM"));

    unlink("/tmp/opencode/fat_image_x1.jpg");
    rc = system("MTOOLS_SKIP_CHECK=1 mcopy -o -i " OUT " ::/DCIM/IMG_0001.JPG /tmp/opencode/fat_image_x1.jpg");
    TEST_ASSERT_EQUAL_INT(0, rc);
    uint8_t got[4096];
    f = fopen("/tmp/opencode/fat_image_x1.jpg", "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(got, 1, sizeof(got), f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT(1500, n);
    uint8_t want[1500];
    fill_pattern(want, sizeof(want), 1);
    TEST_ASSERT_EQUAL_MEMORY(want, got, 1500);

    /* Second file spans a run break: order must still be right. */
    unlink("/tmp/opencode/fat_image_x2.jpg");
    rc = system("MTOOLS_SKIP_CHECK=1 mcopy -o -i " OUT " ::/DCIM/IMG_0002.JPG /tmp/opencode/fat_image_x2.jpg");
    TEST_ASSERT_EQUAL_INT(0, rc);
    f = fopen("/tmp/opencode/fat_image_x2.jpg", "rb");
    TEST_ASSERT_NOT_NULL(f);
    n = fread(got, 1, sizeof(got), f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT(1500, n);
    uint8_t want2[1500];
    memcpy(want2, card + 5000 * 512, 512);
    memcpy(want2 + 512, card + 2000 * 512, 988);
    TEST_ASSERT_EQUAL_MEMORY(want2, got, 1500);
}

static void test_build_rejects_bad_input(void)
{
    static const uint64_t r[] = {1000ull << 32 | 1};
    fat_image_file_t f = {"IMG_0001.JPG", 0, r, 1};
    fat_image_t im;
    TEST_ASSERT_FALSE(fat_image_build(&f, 1, 1, 32, 512, CARD_SECTORS, &im)); /* empty file */
    fat_image_file_t g = {"IMG_00012.JPG", 100, r, 1};
    TEST_ASSERT_FALSE(fat_image_build(&g, 1, 1, 32, 512, CARD_SECTORS, &im)); /* base longer than 8 */
    fat_image_file_t ok = {"IMG_0001.JPG", 100, r, 1};
    TEST_ASSERT_FALSE(fat_image_build(&ok, 1, 3, 32, 512, CARD_SECTORS, &im)); /* spc not power of two */
    fat_image_file_t big = {"IMG_0001.JPG", 100u * 1024 * 1024, r, 1};
    TEST_ASSERT_FALSE(fat_image_build(&big, 1, 1, 32, 512, CARD_SECTORS, &im)); /* runs shorter than size */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_and_read_back);
    RUN_TEST(test_mtools_reads_gallery);
    RUN_TEST(test_build_rejects_bad_input);
    return UNITY_END();
}
