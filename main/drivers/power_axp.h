#pragma once

#include <stdbool.h>

typedef struct {
    bool present;
    bool charging;
    int percent; /* 0–100, or -1 when no battery is connected */
} power_batt_t;

/** Read the AXP2101 fuel gauge. False leaves *out unchanged. */
bool power_axp_read(power_batt_t *out);
