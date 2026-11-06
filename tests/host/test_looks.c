#include <stdlib.h>
#include <string.h>

#include "looks.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t *random_image(int w, int h, unsigned seed)
{
    uint8_t *p = malloc((size_t)w * h * 3);
    srand(seed);
    for (int i = 0; i < w * h * 3; i++) {
        p[i] = (uint8_t)(rand() & 0xff);
    }
    return p;
}

static void test_names_and_count(void)
{
    TEST_ASSERT_EQUAL(12, LOOK_COUNT);
    TEST_ASSERT_EQUAL_STRING("Normal", look_name(LOOK_NORMAL));
    TEST_ASSERT_EQUAL_STRING("Pixel Blue", look_name(LOOK_PIXEL_BLUE));
    TEST_ASSERT_EQUAL_STRING("Charmera Frame", look_name(LOOK_FRAME_CHARMERA));
    TEST_ASSERT_EQUAL(LOOK_NORMAL, look_next(LOOK_FRAME_CHARMERA));
    TEST_ASSERT_EQUAL(LOOK_FRAME_CHARMERA, look_prev(LOOK_NORMAL));
    TEST_ASSERT_EQUAL(LOOK_COOL, look_next(LOOK_NORMAL));
}

static void test_is_software(void)
{
    TEST_ASSERT_FALSE(look_is_software(LOOK_NORMAL));
    for (look_t l = LOOK_COOL; l < LOOK_COUNT; l++) {
        TEST_ASSERT_TRUE(look_is_software(l));
    }
}

static void test_normal_untouched(void)
{
    uint8_t *img = random_image(64, 64, 1);
    uint8_t *ref = random_image(64, 64, 1);
    look_apply_rgb888(LOOK_NORMAL, img, 64, 64, 0, 64, 64);
    TEST_ASSERT_EQUAL_MEMORY(ref, img, 64 * 64 * 3);
    free(img);
    free(ref);
}

static void test_bw_is_gray(void)
{
    uint8_t *img = random_image(64, 64, 2);
    look_apply_rgb888(LOOK_BW, img, 64, 64, 0, 64, 64);
    for (int i = 0; i < 64 * 64; i++) {
        TEST_ASSERT_EQUAL(img[i * 3], img[i * 3 + 1]);
        TEST_ASSERT_EQUAL(img[i * 3], img[i * 3 + 2]);
    }
    free(img);
}

static void test_cool_and_warm_shift(void)
{
    uint8_t px[2][3] = {{128, 128, 128}, {128, 128, 128}};
    look_apply_rgb888(LOOK_COOL, px[0], 1, 1, 0, 1, 1);
    look_apply_rgb888(LOOK_WARM, px[1], 1, 1, 0, 1, 1);
    TEST_ASSERT_GREATER_THAN(px[0][0], px[0][2]); /* cool: blue > red */
    TEST_ASSERT_GREATER_THAN(px[1][2], px[1][0]); /* warm: red > blue */
}

static void test_pixel_block_sizes(void)
{
    TEST_ASSERT_EQUAL(4, look_pixel_block(320));
    TEST_ASSERT_EQUAL(32, look_pixel_block(2592));
    TEST_ASSERT_EQUAL(4, look_pixel_block(100));
}

static void test_pixel_filter_two_tones(void)
{
    const int w = 320, h = 240;
    uint8_t *img = malloc(w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t v = (uint8_t)(x * 255 / (w - 1));
            uint8_t *p = img + (y * w + x) * 3;
            p[0] = p[1] = p[2] = v;
        }
    }
    look_apply_rgb888(LOOK_PIXEL_RED, img, w, h, 0, w, h);

    uint32_t colours[2];
    int ncol = 0;
    const int b = look_pixel_block(w);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = img + (y * w + x) * 3;
            uint32_t c = (p[0] << 16) | (p[1] << 8) | p[2];
            int seen = 0;
            for (int i = 0; i < ncol; i++) {
                seen |= colours[i] == c;
            }
            if (!seen) {
                TEST_ASSERT_LESS_THAN(2, ncol);
                colours[ncol++] = c;
            }
            const uint8_t *cell = img + ((y / b * b) * w + (x / b * b)) * 3;
            TEST_ASSERT_EQUAL_MEMORY(cell, p, 3);
        }
    }
    TEST_ASSERT_EQUAL(2, ncol);
    free(img);
}

static void test_strip_equals_whole_for_every_software_look(void)
{
    const int w = 640, h = 480;
    const int strip = look_pixel_block(w) * 2;
    for (look_t l = LOOK_COOL; l < LOOK_COUNT; l++) {
        uint8_t *whole = random_image(w, h, 7);
        uint8_t *strips = random_image(w, h, 7);
        look_apply_rgb888(l, whole, w, h, 0, w, h);
        for (int y0 = 0; y0 < h; y0 += strip) {
            int rows = (h - y0) < strip ? (h - y0) : strip;
            look_apply_rgb888(l, strips + (size_t)y0 * w * 3, w, rows, y0, w, h);
        }
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole, strips, (size_t)w * h * 3, look_name(l));
        free(whole);
        free(strips);
    }
}

static void test_strip_with_partial_last_strip(void)
{
    /* 1944 isn't a multiple of 32: the last strip is short. Use a scaled-down
       analogue: 648x486 with block 8 -> strips of 8 rows, last one 6 rows. */
    const int w = 648, h = 486;
    const int strip = look_pixel_block(w);
    uint8_t *whole = random_image(w, h, 3);
    uint8_t *strips = random_image(w, h, 3);
    look_apply_rgb888(LOOK_PIXEL_GRAY, whole, w, h, 0, w, h);
    for (int y0 = 0; y0 < h; y0 += strip) {
        int rows = (h - y0) < strip ? (h - y0) : strip;
        look_apply_rgb888(LOOK_PIXEL_GRAY, strips + (size_t)y0 * w * 3, w, rows, y0, w, h);
    }
    TEST_ASSERT_EQUAL_MEMORY(whole, strips, (size_t)w * h * 3);
    free(whole);
    free(strips);
}

static void test_frame_leaves_centre_untouched(void)
{
    const int w = 640, h = 480;
    for (look_t l = LOOK_FRAME_STICKER; l < LOOK_COUNT; l++) {
        uint8_t *img = random_image(w, h, 5);
        uint8_t *ref = random_image(w, h, 5);
        look_apply_rgb888(l, img, w, h, 0, w, h);
        for (int y = h / 4; y < 3 * h / 4; y++) {
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(ref + ((size_t)y * w + w / 4) * 3,
                                             img + ((size_t)y * w + w / 4) * 3, (w / 2) * 3,
                                             look_name(l));
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, memcmp(ref, img, (size_t)w * 3), look_name(l));
        free(img);
        free(ref);
    }
}

static void test_rgb565_matches_rgb888(void)
{
    /* The viewfinder path must produce the same look as the photo path. */
    const int w = 320, h = 240;
    uint16_t *px = malloc(w * h * 2);
    uint8_t *rgb = malloc(w * h * 3);
    srand(11);
    for (int i = 0; i < w * h; i++) {
        uint16_t v = (uint16_t)(rand() & 0xffff);
        px[i] = v;
        rgb[i * 3 + 0] = (uint8_t)(((v >> 11) & 0x1f) << 3);
        rgb[i * 3 + 1] = (uint8_t)(((v >> 5) & 0x3f) << 2);
        rgb[i * 3 + 2] = (uint8_t)((v & 0x1f) << 3);
    }
    look_apply_rgb565(LOOK_PIXEL_BLUE, px, w, h);
    look_apply_rgb888(LOOK_PIXEL_BLUE, rgb, w, h, 0, w, h);
    for (int i = 0; i < w * h; i++) {
        uint16_t want = (uint16_t)(((rgb[i * 3] >> 3) << 11) | ((rgb[i * 3 + 1] >> 2) << 5) |
                                   (rgb[i * 3 + 2] >> 3));
        TEST_ASSERT_EQUAL_HEX16(want, px[i]);
    }
    free(px);
    free(rgb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_names_and_count);
    RUN_TEST(test_is_software);
    RUN_TEST(test_normal_untouched);
    RUN_TEST(test_bw_is_gray);
    RUN_TEST(test_cool_and_warm_shift);
    RUN_TEST(test_pixel_block_sizes);
    RUN_TEST(test_pixel_filter_two_tones);
    RUN_TEST(test_strip_equals_whole_for_every_software_look);
    RUN_TEST(test_strip_with_partial_last_strip);
    RUN_TEST(test_frame_leaves_centre_untouched);
    RUN_TEST(test_rgb565_matches_rgb888);
    return UNITY_END();
}
