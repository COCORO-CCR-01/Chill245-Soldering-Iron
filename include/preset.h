#ifndef __PRESET_H
#define __PRESET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * ============================ 修改方法 ============================
 *
 * 1. 冷端 NTC：
 *    - R25_OHM   填 25°C 标称阻值，单位 Ω。
 *    - BETA_K    填 NTC 的 B 值，单位 K。
 *    - SERIES_OHM填分压电阻，单位 Ω。
 *    - CONNECT_TO_GND：
 *        1 = NTC 下拉到 GND（10k 上拉到 VREF）
 *        0 = NTC 上拉到 VREF（10k 下拉到 GND）
 *    - CAL_OFFSET_C：冷端整体偏移修正，单位 °C。
 *
 * 2. 热端：
 *    - TC_RESISTANCE_KOHM 填热端电阻，单位 kΩ，默认 150.0f。
 *    - 公式：x = 240*N/(1+a)，T = -4.73e-7*x^2 + 0.12*x + 23.78
 *    - 这里 a 的单位是 kΩ，所以默认填 150.0f，不能填 150000。
 *
 * 3. 修改后必须重新编译并烧录：
 *      pio run
 *    这些是编译期常量，不会从 EEPROM 覆盖。
 *
 * 默认值对应当前板：冷端 100kΩ B3950、10k 上拉、热端 150kΩ。
 * ==================================================================
 */
#define PRESET_NTC_R25_OHM            (100000.0f)
#define PRESET_NTC_BETA_K             (3950.0f)
#define PRESET_NTC_SERIES_OHM         (10000.0f)
#define PRESET_NTC_CONNECT_TO_GND     (1)
#define PRESET_NTC_CAL_OFFSET_C       (-10.0f)

#define PRESET_NTC_TABLE_MIN_C        (-20.0f)
#define PRESET_NTC_TABLE_STEP_C       (2.0f)
#define PRESET_NTC_TABLE_POINTS       (61U)

/*
 * 热端电阻，单位 kΩ。
 * 默认 150 kΩ，对应公式：
 *   x = 240*N/(1+a)
 *   T = -4.73e-7*x^2 + 0.12*x + 23.78
 * 修改后重新编译。
 */
#define PRESET_TC_RESISTANCE_KOHM     (150.0f)

typedef struct
{
  float resistance_kohm;
} preset_tc_config_t;

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
extern const preset_tc_config_t g_preset_tc;

#ifdef __cplusplus
}
#endif

#endif
