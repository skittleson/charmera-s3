#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "looks.h"
#include "orient.h"
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
    /* Same number of cells (80) across the viewfinder and the photo. */
    TEST_ASSERT_EQUAL(3, look_pixel_block(240));
    TEST_ASSERT_EQUAL(15, look_pixel_block(1200));
    TEST_ASSERT_EQUAL(240 / look_pixel_block(240), 1200 / look_pixel_block(1200));
    TEST_ASSERT_EQUAL(2, look_pixel_block(100));
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
    const int b = look_pixel_block(h); /* cells come from the short edge */
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

static int distinct_colours(const uint8_t *img, int n)
{
    uint32_t seen[8];
    int k = 0;
    for (int i = 0; i < n && k < 8; i++) {
        const uint32_t c = (img[i * 3] << 16) | (img[i * 3 + 1] << 8) | img[i * 3 + 2];
        int found = 0;
        for (int j = 0; j < k; j++) {
            found |= seen[j] == c;
        }
        if (!found) {
            seen[k++] = c;
        }
    }
    return k;
}

static void test_pixel_filter_dark_scene_keeps_two_tones(void)
{
    /* A dim scene (all luma < 100) must still show both tones, using the threshold
       taken from the scene. */
    const int w = 320, h = 240;
    uint8_t *img = malloc(w * h * 3);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t v = (uint8_t)(10 + x * 90 / (w - 1));
            uint8_t *p = img + (y * w + x) * 3;
            p[0] = p[1] = p[2] = v;
        }
    }
    look_set_pixel_threshold(look_mean_luma_rgb888(img, w * h));
    look_apply_rgb888(LOOK_PIXEL_RED, img, w, h, 0, w, h);
    TEST_ASSERT_EQUAL(2, distinct_colours(img, w * h));
    free(img);
    look_set_pixel_threshold(128);
}

static void test_rgb565_dark_scene_keeps_two_tones(void)
{
    const int w = 240, h = 320;
    uint16_t *px = malloc(w * h * 2);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t v = (uint8_t)(10 + x * 90 / (w - 1));
            px[y * w + x] = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
        }
    }
    look_apply_rgb565(LOOK_PIXEL_BLUE, px, w, h);
    int k = 0;
    uint16_t seen[4];
    for (int i = 0; i < w * h && k < 4; i++) {
        int f = 0;
        for (int j = 0; j < k; j++) {
            f |= seen[j] == px[i];
        }
        if (!f) {
            seen[k++] = px[i];
        }
    }
    TEST_ASSERT_EQUAL(2, k);
    free(px);
    look_set_pixel_threshold(128);
}

static void test_strip_equals_whole_for_every_software_look(void)
{
    const int w = 640, h = 480;
    const int strip = look_pixel_block(h) * 2; /* cells come from the short edge */
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
    const int strip = look_pixel_block(h); /* cells come from the short edge */
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

/* Sensor-orientation strips (landscape sw x sh) must give exactly the photo you'd get
   by rotating to portrait first (portrait (px,py) = sensor (sw-1-py, px)) and applying
   the portrait look. */
static void rotate_to_portrait(const uint8_t *src, int sw, int sh, uint8_t *dst)
{
    const int pw = sh, ph = sw;
    for (int py = 0; py < ph; py++) {
        for (int px = 0; px < pw; px++) {
            memcpy(dst + ((size_t)py * pw + px) * 3, src + ((size_t)px * sw + (sw - 1 - py)) * 3, 3);
        }
    }
}

static void test_sensor_strips_match_portrait_for_every_look(void)
{
    const int sw = 480, sh = 360; /* portrait 360 x 480 */
    const int b = look_pixel_block(sh);
    const int strip = b * 4;
    for (look_t l = LOOK_NORMAL; l < LOOK_COUNT; l++) {
        uint8_t *sensor = random_image(sw, sh, 21);
        uint8_t *portrait = malloc((size_t)sw * sh * 3);
        uint8_t *rotated = malloc((size_t)sw * sh * 3);
        rotate_to_portrait(sensor, sw, sh, portrait);
        look_set_pixel_threshold(100);
        look_apply_rgb888(l, portrait, sh, sw, 0, sh, sw);
        for (int y0 = 0; y0 < sh; y0 += strip) {
            const int rows = (sh - y0) < strip ? (sh - y0) : strip;
            look_apply_rgb888_sensor(l, sensor + (size_t)y0 * sw * 3, sw, rows, y0, sw, sh, ORIENT_PORTRAIT);
        }
        rotate_to_portrait(sensor, sw, sh, rotated);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(portrait, rotated, (size_t)sw * sh * 3, look_name(l));
        free(sensor);
        free(portrait);
        free(rotated);
    }
    look_set_pixel_threshold(128);
}

/* Sensor image -> view image for orientation o. */
static void to_view(const uint8_t *sensor, int sw, int sh, orient_t o, uint8_t *out)
{
    int vw, vh;
    orient_view_size(o, sw, sh, &vw, &vh);
    for (int vy = 0; vy < vh; vy++) {
        for (int vx = 0; vx < vw; vx++) {
            int sx, sy;
            orient_view_to_sensor(o, vx, vy, sw, sh, &sx, &sy);
            memcpy(out + ((size_t)vy * vw + vx) * 3, sensor + ((size_t)sy * sw + sx) * 3, 3);
        }
    }
}

static void test_sensor_strips_match_view_every_orientation(void)
{
    /* 480x360 (b 4) and 400x300 (b 3, odd), both with sensor height a multiple of b; the
       second uses a strip count that leaves a short last strip. */
    const int sizes[2][2] = {{480, 360}, {400, 300}};
    for (int k = 0; k < 2; k++) {
        const int sw = sizes[k][0], sh = sizes[k][1];
        const int b = look_pixel_block(sh < sw ? sh : sw);
        const int strip = b * (k ? 7 : 4);
        for (int o = 0; o < ORIENT_COUNT; o++) {
            int vw, vh;
            orient_view_size((orient_t)o, sw, sh, &vw, &vh);
            for (look_t l = LOOK_NORMAL; l < LOOK_COUNT; l++) {
                uint8_t *sensor = random_image(sw, sh, 21 + k);
                uint8_t *view = malloc((size_t)sw * sh * 3);
                uint8_t *mapped = malloc((size_t)sw * sh * 3);
                to_view(sensor, sw, sh, (orient_t)o, view);
                look_set_pixel_threshold(100);
                look_apply_rgb888(l, view, vw, vh, 0, vw, vh);
                for (int y0 = 0; y0 < sh; y0 += strip) {
                    const int rows = (sh - y0) < strip ? (sh - y0) : strip;
                    look_apply_rgb888_sensor(l, sensor + (size_t)y0 * sw * 3, sw, rows, y0, sw, sh, (orient_t)o);
                }
                to_view(sensor, sw, sh, (orient_t)o, mapped);
                char msg[64];
                snprintf(msg, sizeof(msg), "%dx%d %s %s", sw, sh, orient_name(o), look_name(l));
                TEST_ASSERT_EQUAL_MEMORY_MESSAGE(view, mapped, (size_t)sw * sh * 3, msg);
                free(sensor);
                free(view);
                free(mapped);
            }
        }
    }
    look_set_pixel_threshold(128);
}

static void transpose(const uint8_t *in, int w, int h, uint8_t *out)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            memcpy(out + ((size_t)x * h + y) * 3, in + ((size_t)y * w + x) * 3, 3);
        }
    }
}

static void test_landscape_view_uses_short_edge(void)
{
    /* Pixel cells come from the short edge, so a landscape picture gets exactly the cells
       of the same picture in portrait (the grid is anchored at the top-left corner). */
    for (look_t l = LOOK_PIXEL_RED; l <= LOOK_PIXEL_GRAY; l++) {
        uint8_t *p = random_image(240, 320, 5);
        uint8_t *land = malloc(240 * 320 * 3);
        uint8_t *back = malloc(240 * 320 * 3);
        transpose(p, 240, 320, land); /* 320 x 240 */
        look_set_pixel_threshold(100);
        look_apply_rgb888(l, p, 240, 320, 0, 240, 320);
        look_apply_rgb888(l, land, 320, 240, 0, 320, 240);
        transpose(land, 320, 240, back);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(p, back, 240 * 320 * 3, look_name(l));
        free(p);
        free(land);
        free(back);
    }
    look_set_pixel_threshold(128);
}

/* Film Frame: 35 mm proportions. Rails with sprocket holes run along the photo's LONG
   edges and are 5.5/35 of the short edge wide; the picture between them is untouched. */
static void film_check(int w, int h)
{
    uint8_t *img = malloc((size_t)w * h * 3);
    memset(img, 0x80, (size_t)w * h * 3); /* mid grey, so frame pixels stand out */
    look_apply_rgb888(LOOK_FRAME_FILM, img, w, h, 0, w, h);
    const bool tall = h >= w;
    const int S = tall ? w : h, L = tall ? h : w;
    const int rail = S * 55 / 350;
    int holes = 0, prev_cream = 0;
    for (int u = 0; u < L; u++) {
        /* across = distance from one long edge; u = along the film */
        int changed_rail = 0, cream = 0;
        for (int a = 0; a < rail - 1; a++) {
            const int x = tall ? a : u, y = tall ? u : a;
            const uint8_t *p = img + ((size_t)y * w + x) * 3;
            changed_rail += p[0] != 0x80 || p[1] != 0x80 || p[2] != 0x80;
            cream |= p[0] > 200 && p[1] > 200 && p[2] > 180; /* a backlit sprocket hole */
        }
        TEST_ASSERT_EQUAL_MESSAGE(rail - 1, changed_rail, "rail fully covered");
        /* just inside the rail on both sides: the picture */
        for (int a = rail + 2; a < rail + 6; a++) {
            const int x1 = tall ? a : u, y1 = tall ? u : a;
            const int x2 = tall ? S - 1 - a : u, y2 = tall ? u : S - 1 - a;
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, img[((size_t)y1 * w + x1) * 3], "picture left untouched");
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, img[((size_t)y2 * w + x2) * 3], "picture left untouched");
        }
        holes += cream && !prev_cream;
        prev_cream = cream;
    }
    /* 4.75 mm pitch along the film */
    const float pitch_mm = 4.75f, film_mm = 35.0f;
    const int want = (int)(L / (S * pitch_mm / film_mm));
    TEST_ASSERT_INT_WITHIN_MESSAGE(2, want, holes, "sprocket hole count along the long edge");
    free(img);
}

static void test_film_frame_portrait(void)
{
    film_check(240, 320);
    film_check(1200, 1600);
}

static void test_film_frame_landscape(void)
{
    film_check(320, 240);
    film_check(1600, 1200);
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
    RUN_TEST(test_pixel_filter_dark_scene_keeps_two_tones);
    RUN_TEST(test_rgb565_dark_scene_keeps_two_tones);
    RUN_TEST(test_strip_equals_whole_for_every_software_look);
    RUN_TEST(test_strip_with_partial_last_strip);
    RUN_TEST(test_frame_leaves_centre_untouched);
    RUN_TEST(test_rgb565_matches_rgb888);
    RUN_TEST(test_sensor_strips_match_portrait_for_every_look);
    RUN_TEST(test_sensor_strips_match_view_every_orientation);
    RUN_TEST(test_landscape_view_uses_short_edge);
    RUN_TEST(test_film_frame_portrait);
    RUN_TEST(test_film_frame_landscape);
    return UNITY_END();
}
