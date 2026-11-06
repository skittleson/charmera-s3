#include "input_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Drive the button with a pressed/released timeline sampled every 10 ms. Returns the
   first non-NONE event and when it happened. */
typedef struct {
    input_event_t ev;
    uint32_t at;
} hit_t;

static hit_t run_button(btn_state_t *s, uint32_t from, uint32_t to, bool pressed)
{
    for (uint32_t t = from; t < to; t += 10) {
        input_event_t e = btn_update(s, pressed, t);
        if (e != INPUT_NONE) {
            return (hit_t){e, t};
        }
    }
    return (hit_t){INPUT_NONE, 0};
}

static void test_short_press(void)
{
    btn_state_t s;
    btn_init(&s);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 0, 100, true).ev);
    hit_t h = run_button(&s, 100, 1000, false);
    TEST_ASSERT_EQUAL(INPUT_SHORT, h.ev);
    TEST_ASSERT_EQUAL(100 + BTN_DOUBLE_MS, h.at);
}

static void test_double_press(void)
{
    btn_state_t s;
    btn_init(&s);
    run_button(&s, 0, 100, true);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 100, 200, false).ev);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 200, 300, true).ev);
    hit_t h = run_button(&s, 300, 1000, false);
    TEST_ASSERT_EQUAL(INPUT_DOUBLE, h.ev);
    TEST_ASSERT_EQUAL(300, h.at);
    /* nothing trails the double */
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 310, 2000, false).ev);
}

static void test_long_press_fires_once(void)
{
    btn_state_t s;
    btn_init(&s);
    hit_t h = run_button(&s, 0, 3000, true);
    TEST_ASSERT_EQUAL(INPUT_LONG, h.ev);
    TEST_ASSERT_EQUAL(BTN_LONG_MS, h.at);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, h.at + 10, 3000, true).ev);
}

static void test_long_then_release_no_short(void)
{
    btn_state_t s;
    btn_init(&s);
    run_button(&s, 0, 1000, true);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 1000, 3000, false).ev);
}

static void test_suppress_drops_pending_short(void)
{
    /* Pressed and released just before a save ends: the short press is still pending
       when the UI flushes input. It must not fire afterwards. */
    btn_state_t s;
    btn_init(&s);
    run_button(&s, 0, 100, true);
    run_button(&s, 100, 200, false);
    btn_suppress(&s);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 200, 2000, false).ev);
}

static void test_suppress_ignores_held_until_release(void)
{
    /* Held through the end of a save: no long press, and nothing on release. */
    btn_state_t s;
    btn_init(&s);
    run_button(&s, 0, 300, true);
    btn_suppress(&s);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 300, 3000, true).ev);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_button(&s, 3000, 4000, false).ev);
    /* the next real press works */
    run_button(&s, 4000, 4100, true);
    TEST_ASSERT_EQUAL(INPUT_SHORT, run_button(&s, 4100, 5000, false).ev);
}

static hit_t run_tilt(tilt_state_t *s, uint32_t from, uint32_t to, float roll)
{
    for (uint32_t t = from; t < to; t += 10) {
        input_event_t e = tilt_update(s, roll, t);
        if (e != INPUT_NONE) {
            return (hit_t){e, t};
        }
    }
    return (hit_t){INPUT_NONE, 0};
}

static void test_tilt_needs_hold(void)
{
    tilt_state_t s;
    tilt_init(&s);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_tilt(&s, 0, 100, 0.0f).ev);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_tilt(&s, 100, 300, 25.0f).ev);
    hit_t h = run_tilt(&s, 300, 400, 25.0f);
    TEST_ASSERT_EQUAL(INPUT_NEXT, h.ev);
    TEST_ASSERT_EQUAL(100 + TILT_HOLD_MS, h.at);
}

static void test_tilt_auto_repeat(void)
{
    tilt_state_t s;
    tilt_init(&s);
    uint32_t steps[4];
    int n = 0;
    for (uint32_t t = 0; t < 1500 && n < 4; t += 10) {
        if (tilt_update(&s, 25.0f, t) == INPUT_NEXT) {
            steps[n++] = t;
        }
    }
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL(250, steps[0]);
    TEST_ASSERT_EQUAL(850, steps[1]);
    TEST_ASSERT_EQUAL(1450, steps[2]);
}

static void test_tilt_rearm_requires_level(void)
{
    tilt_state_t s;
    tilt_init(&s);
    TEST_ASSERT_EQUAL(INPUT_NEXT, run_tilt(&s, 0, 300, 25.0f).ev);
    run_tilt(&s, 300, 400, 12.0f);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_tilt(&s, 400, 2000, 25.0f).ev);
    run_tilt(&s, 2000, 2100, 5.0f);
    TEST_ASSERT_EQUAL(INPUT_NEXT, run_tilt(&s, 2100, 2500, 25.0f).ev);
}

static void test_tilt_left_is_prev(void)
{
    tilt_state_t s;
    tilt_init(&s);
    TEST_ASSERT_EQUAL(INPUT_PREV, run_tilt(&s, 0, 400, -25.0f).ev);
}

static void test_wobble_ignored(void)
{
    tilt_state_t s;
    tilt_init(&s);
    for (uint32_t t = 0; t < 5000; t += 10) {
        float roll = ((t / 100) % 2) ? 14.0f : -14.0f;
        TEST_ASSERT_EQUAL(INPUT_NONE, tilt_update(&s, roll, t));
    }
}

static void test_tilt_brief_spike_ignored(void)
{
    tilt_state_t s;
    tilt_init(&s);
    run_tilt(&s, 0, 100, 0.0f);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_tilt(&s, 100, 300, 25.0f).ev);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_tilt(&s, 300, 400, 14.0f).ev);
    TEST_ASSERT_EQUAL(INPUT_NONE, run_tilt(&s, 400, 600, 25.0f).ev);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_press);
    RUN_TEST(test_double_press);
    RUN_TEST(test_long_press_fires_once);
    RUN_TEST(test_long_then_release_no_short);
    RUN_TEST(test_suppress_drops_pending_short);
    RUN_TEST(test_suppress_ignores_held_until_release);
    RUN_TEST(test_tilt_needs_hold);
    RUN_TEST(test_tilt_auto_repeat);
    RUN_TEST(test_tilt_rearm_requires_level);
    RUN_TEST(test_tilt_left_is_prev);
    RUN_TEST(test_wobble_ignored);
    RUN_TEST(test_tilt_brief_spike_ignored);
    return UNITY_END();
}
