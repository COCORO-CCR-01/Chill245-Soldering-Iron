#ifndef __PRESET_H
#define __PRESET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * 冷端 NTC 预设。
 *
 * 修改这里后重新编译即可，运行时不从 EEPROM 覆盖。
 * 默认值对应当前板上的 100kΩ B3950 NTC。
 *
 * PRESET_NTC_CONNECT_TO_GND:
 *   1 = 10k 上拉到 VREF，NTC 下拉到 GND
 *   0 = NTC 上拉到 VREF，10k 下拉到 GND
 */
#define PRESET_NTC_R25_OHM            (100000.0f)
#define PRESET_NTC_BETA_K             (3950.0f)
#define PRESET_NTC_SERIES_OHM         (10000.0f)
#define PRESET_NTC_CONNECT_TO_GND     (1)
#define PRESET_NTC_CAL_OFFSET_C       (-10.0f)

#define PRESET_NTC_TABLE_MIN_C        (-20.0f)
#define PRESET_NTC_TABLE_STEP_C       (2.0f)
#define PRESET_NTC_TABLE_POINTS       (61U)

typedef struct
{
  float r25_ohm;
  float beta_k;
  float series_ohm;
  uint8_t connect_to_gnd;
  float cal_offset_c;
  float table_min_c;
  float table_step_c;
  uint8_t table_points;
} preset_ntc_config_t;

extern const preset_ntc_config_t g_preset_ntc;

#ifdef __cplusplus
}
#endif

#endif
