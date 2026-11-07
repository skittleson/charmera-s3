#include <math.h>
#include <string.h>

#include "orient.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define G 1000.0f

/* Gravity for a grip turned by deg from portrait (positive = clockwise). */
static void gravity(float deg, float *down, float *side)
{
    const float r = deg * (float)M_PI / 180.0f;
    *down = G * cosf(r);
    *side = G * sinf(r);
}

static void test_names_roundtrip(void)
{
    const char *n[] = {"portrait", "landscape-right", "landscape-left", "auto"};
    for (int i = 0; i < ORIENT_SETTING_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(n[i], orient_name(i));
        TEST_ASSERT_EQUAL(i, orient_parse(n[i]));
    }
    TEST_ASSERT_EQUAL(-1, orient_parse("sideways"));
    TEST_ASSERT_EQUAL(-1, orient_parse(NULL));
}

static void test_view_size(void)
{
    int w, h;
    orient_view_size(ORIENT_PORTRAIT, 2560, 1920, &w, &h);
    TEST_ASSERT_EQUAL(1920, w);
    TEST_ASSERT_EQUAL(2560, h);
    orient_view_size(ORIENT_LAND_LEFT, 2560, 1920, &w, &h);
    TEST_ASSERT_EQUAL(2560, w);
    TEST_ASSERT_EQUAL(1920, h);
    orient_view_size(ORIENT_LAND_RIGHT, 320, 240, &w, &h);
    TEST_ASSERT_EQUAL(320, w);
    TEST_ASSERT_EQUAL(240, h);
}

static void test_view_to_sensor_corners(void)
{
    const int sw = 320, sh = 240;
    int x, y;
    /* Portrait: today's mapping, (px,py) -> (sw-1-py, px). */
    orient_view_to_sensor(ORIENT_PORTRAIT, 0, 0, sw, sh, &x, &y);
    TEST_ASSERT_EQUAL(sw - 1, x);
    TEST_ASSERT_EQUAL(0, y);
    orient_view_to_sensor(ORIENT_PORTRAIT, 10, 20, sw, sh, &x, &y);
    TEST_ASSERT_EQUAL(sw - 1 - 20, x);
    TEST_ASSERT_EQUAL(10, y);
    /* The native landscape side is the sensor as-is; the other is turned 180. */
    const orient_t other = ORIENT_SENSOR_NATIVE == ORIENT_LAND_LEFT ? ORIENT_LAND_RIGHT : ORIENT_LAND_LEFT;
    orient_view_to_sensor(ORIENT_SENSOR_NATIVE, 10, 20, sw, sh, &x, &y);
    TEST_ASSERT_EQUAL(10, x);
    TEST_ASSERT_EQUAL(20, y);
    orient_view_to_sensor(other, 0, 0, sw, sh, &x, &y);
    TEST_ASSERT_EQUAL(sw - 1, x);
    TEST_ASSERT_EQUAL(sh - 1, y);
}

static void test_view_to_sensor_is_a_bijection(void)
{
    static unsigned char seen[30 * 20];
    for (int o = 0; o < ORIENT_COUNT; o++) {
        int vw, vh;
        orient_view_size((orient_t)o, 30, 20, &vw, &vh);
        memset(seen, 0, sizeof(seen));
        for (int vy = 0; vy < vh; vy++) {
            for (int vx = 0; vx < vw; vx++) {
                int x, y;
                orient_view_to_sensor((orient_t)o, vx, vy, 30, 20, &x, &y);
                TEST_ASSERT_TRUE(x >= 0 && x < 30 && y >= 0 && y < 20);
                TEST_ASSERT_EQUAL(0, seen[y * 30 + x]);
                seen[y * 30 + x] = 1;
            }
        }
    }
}

static void test_exif_values(void)
{
    TEST_ASSERT_EQUAL(8, orient_exif(ORIENT_PORTRAIT));
    TEST_ASSERT_EQUAL(1, orient_exif(ORIENT_SENSOR_NATIVE));
    TEST_ASSERT_EQUAL(3, orient_exif(ORIENT_SENSOR_NATIVE == ORIENT_LAND_LEFT ? ORIENT_LAND_RIGHT : ORIENT_LAND_LEFT));
}

static void test_angles_and_centres(void)
{
    float d, s;
    gravity(0, &d, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, orient_angle_deg(d, s));
    gravity(ORIENT_LEFT_DEG, &d, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, orient_centre_deg(ORIENT_LAND_LEFT), orient_angle_deg(d, s));
    gravity(-ORIENT_LEFT_DEG, &d, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, orient_centre_deg(ORIENT_LAND_RIGHT), orient_angle_deg(d, s));
    gravity(180, &d, &s); /* upside down folds onto portrait */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, orient_angle_deg(d, s));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, orient_centre_deg(ORIENT_PORTRAIT));
}

static void test_roll_relative(void)
{
    float d, s;
    gravity(12, &d, &s); /* portrait, tilted 12 deg clockwise */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 12.0f, orient_roll_deg(ORIENT_PORTRAIT, d, s));
    /* Portrait matches today's formula atan2(side, |down|). */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, atan2f(s, fabsf(d)) * 180.0f / (float)M_PI, orient_roll_deg(ORIENT_PORTRAIT, d, s));
    gravity(ORIENT_LEFT_DEG + 7, &d, &s); /* USB-C left, tilted 7 deg further */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, ORIENT_LEFT_DEG > 0 ? 7.0f : -7.0f, orient_roll_deg(ORIENT_LAND_LEFT, d, s));
    gravity(-ORIENT_LEFT_DEG - 5, &d, &s);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, ORIENT_LEFT_DEG > 0 ? -5.0f : 5.0f, orient_roll_deg(ORIENT_LAND_RIGHT, d, s));
}

/* Feed a constant grip from t0 to t1 (ms) every 10 ms; returns the time of the first switch or -1. */
static int64_t hold(orient_auto_t *a, float deg, float face, bool locked, int64_t t0, int64_t t1)
{
    float d, s;
    gravity(deg, &d, &s);
    for (int64_t t = t0; t <= t1; t += 10) {
        if (orient_auto_update(a, d, s, face, locked, t)) {
            return t;
        }
    }
    return -1;
}

static void test_auto_switches_after_dwell(void)
{
    orient_auto_t a;
    orient_auto_init(&a, ORIENT_PORTRAIT);
    TEST_ASSERT_EQUAL(-1, hold(&a, 0, 0, false, 0, 1000));
    TEST_ASSERT_EQUAL(-1, hold(&a, ORIENT_LEFT_DEG, 0, false, 1010, 1600));
    const int64_t t = hold(&a, ORIENT_LEFT_DEG, 0, false, 1610, 3000);
    TEST_ASSERT_TRUE(t >= 1010 + ORIENT_AUTO_DWELL_MS && t <= 1010 + ORIENT_AUTO_DWELL_MS + 20);
    TEST_ASSERT_EQUAL(ORIENT_LAND_LEFT, a.current);
    TEST_ASSERT_TRUE(hold(&a, -ORIENT_LEFT_DEG, 0, false, 3010, 5000) > 0);
    TEST_ASSERT_EQUAL(ORIENT_LAND_RIGHT, a.current);
    TEST_ASSERT_TRUE(hold(&a, 5, 0, false, 5010, 7000) > 0);
    TEST_ASSERT_EQUAL(ORIENT_PORTRAIT, a.current);
}

static void test_auto_hysteresis_no_flip_flop(void)
{
    orient_auto_t a;
    orient_auto_init(&a, ORIENT_PORTRAIT);
    float d1, s1, d2, s2;
    gravity(40, &d1, &s1);
    gravity(50, &d2, &s2);
    for (int64_t t = 0; t < 5000; t += 10) {
        const bool odd = (t / 100) % 2;
        TEST_ASSERT_FALSE(orient_auto_update(&a, odd ? d1 : d2, odd ? s1 : s2, 0, false, t));
    }
    TEST_ASSERT_EQUAL(ORIENT_PORTRAIT, a.current);
    /* A camera framed at a 25 deg angle in landscape stays landscape. */
    orient_auto_init(&a, ORIENT_LAND_LEFT);
    TEST_ASSERT_EQUAL(-1, hold(&a, ORIENT_LEFT_DEG > 0 ? 65 : -65, 0, false, 0, 3000));
    TEST_ASSERT_EQUAL(-1, hold(&a, ORIENT_LEFT_DEG > 0 ? 45 : -45, 0, false, 3010, 6000));
    TEST_ASSERT_EQUAL(ORIENT_LAND_LEFT, a.current);
}

static void test_auto_ignores_flat(void)
{
    orient_auto_t a;
    orient_auto_init(&a, ORIENT_PORTRAIT);
    float d, s;
    gravity(ORIENT_LEFT_DEG, &d, &s);
    for (int64_t t = 0; t < 3000; t += 10) {
        /* 300 mg in the screen plane, 950 mg through the face: lying (nearly) flat. */
        TEST_ASSERT_FALSE(orient_auto_update(&a, d * 0.3f, s * 0.3f, 950.0f, false, t));
    }
    TEST_ASSERT_EQUAL(ORIENT_PORTRAIT, a.current);
}

static void test_auto_locked(void)
{
    orient_auto_t a;
    orient_auto_init(&a, ORIENT_PORTRAIT);
    TEST_ASSERT_EQUAL(-1, hold(&a, ORIENT_LEFT_DEG, 0, true, 0, 2000));
    const int64_t t = hold(&a, ORIENT_LEFT_DEG, 0, false, 2010, 4000);
    TEST_ASSERT_TRUE(t >= 2010 + ORIENT_AUTO_DWELL_MS); /* a fresh dwell after the unlock */
    TEST_ASSERT_EQUAL(ORIENT_LAND_LEFT, a.current);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_names_roundtrip);
    RUN_TEST(test_view_size);
    RUN_TEST(test_view_to_sensor_corners);
    RUN_TEST(test_view_to_sensor_is_a_bijection);
    RUN_TEST(test_exif_values);
    RUN_TEST(test_angles_and_centres);
    RUN_TEST(test_roll_relative);
    RUN_TEST(test_auto_switches_after_dwell);
    RUN_TEST(test_auto_hysteresis_no_flip_flop);
    RUN_TEST(test_auto_ignores_flat);
    RUN_TEST(test_auto_locked);
    return UNITY_END();
}
