#include "power_logic.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_awake_until_dim(void)
{
    TEST_ASSERT_EQUAL(POWER_AWAKE, power_state_for_idle(0));
    TEST_ASSERT_EQUAL(POWER_AWAKE, power_state_for_idle(POWER_DIM_MS - 1));
}

static void test_dim_at_30s(void)
{
    TEST_ASSERT_EQUAL(POWER_DIM, power_state_for_idle(30 * 1000));
    TEST_ASSERT_EQUAL(POWER_DIM, power_state_for_idle(POWER_SLEEP_MS - 1));
}

static void test_asleep_at_3min(void)
{
    TEST_ASSERT_EQUAL(POWER_ASLEEP, power_state_for_idle(3 * 60 * 1000));
    TEST_ASSERT_EQUAL(POWER_ASLEEP, power_state_for_idle(UINT32_MAX));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_awake_until_dim);
    RUN_TEST(test_dim_at_30s);
    RUN_TEST(test_asleep_at_3min);
    return UNITY_END();
}
