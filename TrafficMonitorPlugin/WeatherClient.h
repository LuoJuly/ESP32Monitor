#pragma once

#include <stdint.h>

struct WeatherSnap {
    int temp;
    int code;
    int hum;
    char city[24];
    /* Minutes after local midnight at the weather fix. -1 if unknown. */
    int rise_min;
    int set_min;
    /* Last network UTC sample and that fix's UTC offset. time_ok is 0
     * until both have arrived. time_tick is GetTickCount64 at the sample. */
    int time_ok;
    int offset_sec;
    int64_t utc_unix;
    uint64_t time_tick;
};

void weather_start();
void weather_stop();
void weather_copy(WeatherSnap* out);

/* Civil time at the weather fix, advanced from the last network sample.
 * False until that sample exists. */
bool weather_network_time(const WeatherSnap* snap,
                          int* year, int* month, int* day,
                          int* hour, int* minute, int* second);
