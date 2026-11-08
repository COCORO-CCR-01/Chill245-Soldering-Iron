#include "preset.h"

const preset_tc_config_t g_preset_tc =
{
  PRESET_TC_RESISTANCE_KOHM
};

const preset_ntc_config_t g_preset_ntc =
{
  PRESET_NTC_R25_OHM,
  PRESET_NTC_BETA_K,
  PRESET_NTC_SERIES_OHM,
  (uint8_t)PRESET_NTC_CONNECT_TO_GND,
  PRESET_NTC_CAL_OFFSET_C,
  PRESET_NTC_TABLE_MIN_C,
  PRESET_NTC_TABLE_STEP_C,
  (uint8_t)PRESET_NTC_TABLE_POINTS
};
