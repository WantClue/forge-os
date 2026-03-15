#ifndef POWER_MANAGEMENT_TASK_H_
#define POWER_MANAGEMENT_TASK_H_

// DEBUG FLAG
// #define POWER_DEBUG

typedef struct
{
    uint16_t fan_perc;
    uint16_t fan_rpm[2];
    float chip_temp[6];
    float chip_temp_avg;
    float vr_temp;
    float voltage;
    float frequency_multiplier;
    float frequency_value;
    float power;
    float current;
    float thermal_freq;  // current thermally-adjusted running frequency (MHz)
} PowerManagementModule;

void POWER_MANAGEMENT_task(void * pvParameters);

#endif
