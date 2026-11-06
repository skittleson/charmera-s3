#include "power_logic.h"

power_state_t power_state_for_idle(uint32_t idle_ms)
{
    if (idle_ms >= POWER_SLEEP_MS) {
        return POWER_ASLEEP;
    }
    return idle_ms >= POWER_DIM_MS ? POWER_DIM : POWER_AWAKE;
}
