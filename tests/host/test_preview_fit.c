#include <stdbool.h>

#include "preview_fit.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define MAX (240 * 320)

typedef struct {
    int w, h;
    bool turned;
    int out_w, out_h;
} case_t;

static void check(case_t c)
{
    int dw, dh, sw, sh;
    TEST_ASSERT_TRUE(preview_fit(c.w, c.h, c.turned, c.out_w, c.out_h, MAX, &dw, &dh, &sw, &sh));
    TEST_ASSERT_EQUAL(0, dw % 8);
    TEST_ASSERT_EQUAL(0, dh % 8);
    TEST_ASSERT_TRUE(dw * 8 >= c.w && dh * 8 >= c.h); /* decoder can shrink at most 8x */
    TEST_ASSERT_TRUE(dw <= c.w && dh <= c.h);
    TEST_ASSERT_TRUE(dw * dh <= MAX);
    TEST_ASSERT_TRUE(sw <= c.out_w && sh <= c.out_h);
    TEST_ASSERT_TRUE(sw == c.out_w || sh == c.out_h); /* fills one side */
    /* Aspect kept (as shown) within a pixel. */
    const int aw = c.turned ? c.h : c.w, ah = c.turned ? c.w : c.h;
    const int err = sw * ah - sh * aw;
    TEST_ASSERT_TRUE(err < aw && -err < aw && err < ah + aw && -err < ah + aw);
}

static void test_every_photo_on_every_screen(void)
{
    const int photos[][3] = {
        {2560, 1920, 1}, /* OV5640 portrait (EXIF 8) */
        {2560, 1920, 0}, /* OV5640 landscape (EXIF 1/3) */
        {1200, 1600, 0}, /* OV2640 portrait */
        {1600, 1200, 0}, /* OV2640 landscape */
    };
    const int screens[][2] = {{240, 320}, {320, 240}};
    for (int p = 0; p < 4; p++) {
        for (int s = 0; s < 2; s++) {
            check((case_t){photos[p][0], photos[p][1], photos[p][2], screens[s][0], screens[s][1]});
        }
    }
}

static void test_too_big_for_buffer_fails(void)
{
    int a, b, c, d;
    /* 1/8 of 8000x8000 is 1000x1000: more than the buffer holds. */
    TEST_ASSERT_FALSE(preview_fit(8000, 8000, false, 240, 320, MAX, &a, &b, &c, &d));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_photo_on_every_screen);
    RUN_TEST(test_too_big_for_buffer_fails);
    return UNITY_END();
}
