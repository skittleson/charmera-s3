#include <string.h>

#include "lapse_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_intervals(void)
{
    TEST_ASSERT_EQUAL_UINT32(5000, lapse_interval_ms(0));
    TEST_ASSERT_EQUAL_UINT32(10000, lapse_interval_ms(1));
    TEST_ASSERT_EQUAL_UINT32(30000, lapse_interval_ms(2));
    TEST_ASSERT_EQUAL_UINT32(60000, lapse_interval_ms(3));
    TEST_ASSERT_EQUAL_UINT32(300000, lapse_interval_ms(4));
    TEST_ASSERT_EQUAL_STRING("1 min", lapse_interval_name(3));
    TEST_ASSERT_EQUAL_STRING("5 s", lapse_interval_name(0));
}

static void test_bad_index_uses_default(void)
{
    TEST_ASSERT_EQUAL_UINT32(lapse_interval_ms(LAPSE_INTERVAL_DEFAULT), lapse_interval_ms(-1));
    TEST_ASSERT_EQUAL_UINT32(lapse_interval_ms(LAPSE_INTERVAL_DEFAULT), lapse_interval_ms(LAPSE_INTERVAL_COUNT));
    TEST_ASSERT_EQUAL_STRING(lapse_interval_name(LAPSE_INTERVAL_DEFAULT), lapse_interval_name(99));
}

static void test_next_due_on_schedule(void)
{
    /* Frame due at 0 finished at 4 s: next stays on the 10 s grid. */
    TEST_ASSERT_EQUAL_INT64(10000, lapse_next_due(0, 10000, 4000));
    TEST_ASSERT_EQUAL_INT64(20000, lapse_next_due(10000, 10000, 14500));
}

static void test_next_due_skips_missed_slots(void)
{
    /* A 5 s interval with a 12 s save: shoot again right away, no burst. */
    TEST_ASSERT_EQUAL_INT64(12000, lapse_next_due(0, 5000, 12000));
    TEST_ASSERT_EQUAL_INT64(12000, lapse_next_due(0, 5000, 12000));
}

static void test_format_duration(void)
{
    char b[16];
    lapse_format_duration(0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("0:00", b);
    lapse_format_duration(75, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1:15", b);
    lapse_format_duration(3725, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("1:02:05", b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_intervals);
    RUN_TEST(test_bad_index_uses_default);
    RUN_TEST(test_next_due_on_schedule);
    RUN_TEST(test_next_due_skips_missed_slots);
    RUN_TEST(test_format_duration);
    return UNITY_END();
}
