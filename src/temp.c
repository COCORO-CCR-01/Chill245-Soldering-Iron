#pragma GCC optimize("O2")
/**
  **************************************************************************
  * @file     temp.c
  * @brief    NTC 查表与热电偶温度换算
  **************************************************************************
  * NTC: 100kΩ B3950 (25°C)
  *      R25 = 100kΩ, B = 3950K
  *      与 10kΩ 电阻构成分压
  *
  * 热电偶: PA1 为放大后的热电偶信号
  *         默认按“运放增益 × 热电偶灵敏度”做线性换算，
  *         标定后使用 10 点 ADC->温度曲线 + 冷端补偿。
  **************************************************************************
  */

#include "temp.h"
#include "at32f421.h"
#include <stddef.h>

/* ============================== NTC 参数 ============================== */
/*
 * 冷端 NTC 参数来自 preset.h。
 * 开机时用 Beta 方程动态生成温度-电阻表，替代写死的常量表：
 *   R(T) = R25 * exp(B * (1/T - 1/T25))
 * 修改 NTC 后在 preset.h 中填写并重新编译。
 */
#include "preset.h"

#define TEMP_ADC_FULL_SCALE             (4095.0f)

#define TEMP_NTC_TABLE_MIN_C            PRESET_NTC_TABLE_MIN_C
#define TEMP_NTC_TABLE_STEP_C           PRESET_NTC_TABLE_STEP_C
#define TEMP_NTC_TABLE_POINTS           PRESET_NTC_TABLE_POINTS

static float temp_ntc_table[TEMP_NTC_TABLE_POINTS];
static uint8_t temp_ntc_table_ready;

_Static_assert(PRESET_NTC_TABLE_POINTS >= 2U, "NTC table too short");

static float temp_expf(float x)
{
  int32_t k;
  float r;
  float p;
  float scale;
  int32_t i;

  /* e^x = 2^k * e^r, r in [0, ln2) */
  k = (int32_t)(x * 1.44269504f);
  r = x - ((float)k * 0.69314718f);
  if(r < 0.0f)
  {
    r += 0.69314718f;
    k--;
  }

  p = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f +
      r * (0.04166667f + r * (0.00833333f + r * 0.00138889f)))));

  scale = 1.0f;
  if(k >= 0)
  {
    for(i = 0; i < k; i++)
    {
      scale *= 2.0f;
    }
  }
  else
  {
    for(i = 0; i > k; i--)
    {
      scale *= 0.5f;
    }
  }

  return p * scale;
}

void temp_init(void)
{
  uint32_t i;
  float t_c;
  float t_k;
  float inv_t;
  float inv_t25 = 1.0f / 298.15f;

  for(i = 0U; i < PRESET_NTC_TABLE_POINTS; i++)
  {
    t_c = PRESET_NTC_TABLE_MIN_C +
          ((float)i * PRESET_NTC_TABLE_STEP_C);
    t_k = t_c + 273.15f;
    inv_t = 1.0f / t_k;
    temp_ntc_table[i] = PRESET_NTC_R25_OHM *
      temp_expf(PRESET_NTC_BETA_K * (inv_t - inv_t25));
  }

  temp_ntc_table_ready = 1U;
}

/* NTC 开路/短路判定：ADC 接近 0 或 4095 时认为传感器异常 */
#define TEMP_NTC_ADC_FAULT_LOW          (2U)
#define TEMP_NTC_ADC_FAULT_HIGH         (4093U)

/* NTC 冷端实测比实际高约 10°C，直接在换算结果上做一阶修正 */
#define TEMP_NTC_CAL_OFFSET_C           (g_preset_ntc.cal_offset_c)

/* 热电偶默认曲线的一阶增益/偏移微调；默认不改，精修建议用 7 点热端标定 */
#define TEMP_TC_TRIM_GAIN               (1.0f)
#define TEMP_TC_TRIM_OFFSET_C           (0.0f)

/* ========================== 热电偶参数 ========================== */
/*
 * 初始拟合公式（N = PA1 原始 ADC 值，0~4095）：
 *   T = -1.1948e-6 * N^2 + 0.19073 * N + 23.78
 *
 * 该式在冷端约 23.78°C 时拟合得到；运行时按当前冷端与
 * TEMP_TC_POLY_REF_C 的差值做一阶冷端补偿。
 */
#define TEMP_TC_SCALE                   (240.0f)
#define TEMP_TC_QUAD                    (-4.73e-7f)
#define TEMP_TC_LIN                     (0.12f)
#define TEMP_TC_POLY_C                  (23.78f)
#define TEMP_TC_POLY_REF_C              (23.78f)
#define TEMP_TC_ZERO_ADC                (0.0f)

/*
 * 温度一阶 IIR/EMA 平滑：
 *   y_n = alpha * x_n + (1 - alpha) * y_{n-1}
 *
 * 采样改为 5 ms（200 Hz）消隐采样后：
 *   TEMP_TC_FILTER_ALPHA  0.06 @ 200 Hz -> 约 2 Hz
 *   TEMP_NTC_FILTER_ALPHA 0.05 @ 200 Hz -> 约 1.6 Hz
 *
 * 烙铁头热时间常数远大于温度滤波时间常数，平滑主要压制
 * ADC 噪声和加热器耦合残余。
 */
#define TEMP_TC_FILTER_ALPHA            (0.06f)
#define TEMP_NTC_FILTER_ALPHA           (0.05f)

/* ====================== 烙铁头 10 点标定曲线 ====================== */
/*
 * 标定曲线给出“标定冷端温度下”的 ADC -> 实际温度。
 * 运行时再补偿当前冷端与标定冷端的差异。
 * 默认曲线是理论线性曲线的 10 个采样点，启用后与线性换算基本一致。
 */
#define TEMP_TC_CAL_REF_C               (25.0f)

static temp_tc_cal_point_t temp_tc_cal_points[TEMP_TC_CAL_POINTS];
static uint8_t temp_tc_cal_enabled;
static float temp_tc_cal_ref_c;
static uint8_t temp_tc_cal_initialized;

float temp_thermocouple_default_celsius(uint16_t adc, float cold_junction_c)
{
  float n = (float)adc;
  float x;
  float tip_c;

  x = (TEMP_TC_SCALE * n) / (1.0f + g_preset_tc.resistance_kohm);

  tip_c = (TEMP_TC_QUAD * x * x) +
          (TEMP_TC_LIN * x) +
          TEMP_TC_POLY_C;

  /* 冷端补偿 + 可选的增益/偏移微调（默认 1.0 / 0.0） */
  tip_c = cold_junction_c +
          (((tip_c - TEMP_TC_POLY_REF_C) * TEMP_TC_TRIM_GAIN) +
           TEMP_TC_TRIM_OFFSET_C);

  return tip_c;
}

static void temp_tc_cal_init_defaults(void)
{
  uint32_t i;

  if(temp_tc_cal_initialized != 0U)
  {
    return;
  }

  for(i = 0U; i < TEMP_TC_CAL_POINTS; i++)
  {
    uint16_t adc = (uint16_t)(((uint32_t)i * TEMP_ADC_FULL_SCALE) /
                              (TEMP_TC_CAL_POINTS - 1U));
    float temp_c = temp_thermocouple_default_celsius(adc, TEMP_TC_CAL_REF_C);

    if(temp_c > 600.0f)
    {
      temp_c = 600.0f;
    }

    temp_tc_cal_points[i].adc = adc;
    temp_tc_cal_points[i].temp_c = temp_c;
  }

  temp_tc_cal_enabled = 0U;
  temp_tc_cal_ref_c = TEMP_TC_CAL_REF_C;
  temp_tc_cal_initialized = 1U;
}

/* ============================ 内部函数 ============================ */

/**
  * @brief  NTC 电阻值转摄氏度（查表 + 线性插值）
  * @param  resistance: NTC 电阻，单位 Ω
  * @retval 温度，单位 °C
  */
static float temp_ntc_resistance_to_celsius(float resistance)
{
  uint32_t low = 0U;
  uint32_t high = TEMP_NTC_TABLE_POINTS - 1U;
  uint32_t mid;
  float r_high;
  float r_low;
  float ratio;

  /* 表按温度升高、电阻减小排列 */
  if(temp_ntc_table_ready == 0U)
  {
    temp_init();
  }

  if(resistance >= temp_ntc_table[0])
  {
    return TEMP_NTC_TABLE_MIN_C;
  }

  if(resistance <= temp_ntc_table[TEMP_NTC_TABLE_POINTS - 1U])
  {
    return TEMP_NTC_TABLE_MIN_C + ((float)(TEMP_NTC_TABLE_POINTS - 1U) * TEMP_NTC_TABLE_STEP_C);
  }

  /* 二分查找电阻所在的温度区间 */
  while((low + 1U) < high)
  {
    mid = (low + high) / 2U;

    if(temp_ntc_table[mid] > resistance)
    {
      low = mid;
    }
    else
    {
      high = mid;
    }
  }

  r_high = temp_ntc_table[low];
  r_low = temp_ntc_table[high];

  if(r_high <= r_low)
  {
    return TEMP_NTC_TABLE_MIN_C + ((float)low * TEMP_NTC_TABLE_STEP_C);
  }

  ratio = (r_high - resistance) / (r_high - r_low);

  return TEMP_NTC_TABLE_MIN_C + (((float)low + ratio) * TEMP_NTC_TABLE_STEP_C);
}

static float temp_tc_cal_lookup(uint16_t adc)
{
  uint32_t i;

  temp_tc_cal_init_defaults();

  if(adc <= temp_tc_cal_points[0].adc)
  {
    return temp_tc_cal_points[0].temp_c;
  }

  if(adc >= temp_tc_cal_points[TEMP_TC_CAL_POINTS - 1U].adc)
  {
    return temp_tc_cal_points[TEMP_TC_CAL_POINTS - 1U].temp_c;
  }

  for(i = 0U; i < (TEMP_TC_CAL_POINTS - 1U); i++)
  {
    if(adc <= temp_tc_cal_points[i + 1U].adc)
    {
      uint16_t adc0 = temp_tc_cal_points[i].adc;
      uint16_t adc1 = temp_tc_cal_points[i + 1U].adc;
      float temp0 = temp_tc_cal_points[i].temp_c;
      float temp1 = temp_tc_cal_points[i + 1U].temp_c;

      if(adc1 == adc0)
      {
        return temp1;
      }

      return temp0 + (((float)(adc - adc0) / (float)(adc1 - adc0)) *
                      (temp1 - temp0));
    }
  }

  return temp_tc_cal_points[TEMP_TC_CAL_POINTS - 1U].temp_c;
}

/* ============================ 对外函数 ============================ */

float temp_ntc_adc_to_celsius(uint16_t adc)
{
  float adc_f = (float)adc;
  float resistance;

  /* 限幅，避免分压公式除零 */
  if(adc_f < 1.0f)
  {
    adc_f = 1.0f;
  }

  if(adc_f > (TEMP_ADC_FULL_SCALE - 1.0f))
  {
    adc_f = TEMP_ADC_FULL_SCALE - 1.0f;
  }

  if(temp_ntc_table_ready == 0U)
  {
    temp_init();
  }

  if(g_preset_ntc.connect_to_gnd != 0U)
  {
    /* V_adc = VREF * R_ntc / (Rs + R_ntc) */
    resistance = g_preset_ntc.series_ohm * adc_f /
                 (TEMP_ADC_FULL_SCALE - adc_f);
  }
  else
  {
    /* V_adc = VREF * Rs / (R_ntc + Rs) */
    resistance = g_preset_ntc.series_ohm *
                 (TEMP_ADC_FULL_SCALE - adc_f) / adc_f;
  }

  return temp_ntc_resistance_to_celsius(resistance) +
         TEMP_NTC_CAL_OFFSET_C;
}

float temp_thermocouple_adc_to_celsius(uint16_t adc, float cold_junction_c)
{
  temp_tc_cal_init_defaults();

  if(temp_tc_cal_enabled != 0U)
  {
    /* 标定表给出的是标定冷端温度下的温度，运行时补偿冷端差异 */
    return temp_tc_cal_lookup(adc) + (cold_junction_c - temp_tc_cal_ref_c);
  }

  /* 未标定：使用初始二次拟合 + 冷端补偿 */
  return temp_thermocouple_default_celsius(adc, cold_junction_c);
}

uint16_t temp_get_thermocouple_zero_adc(void)
{
  return (uint16_t)(TEMP_TC_ZERO_ADC + 0.5f);
}

void temp_get_cal_curve(temp_tc_cal_point_t *points,
                        uint8_t *enabled,
                        float *ref_c)
{
  uint32_t primask;

  if(points == NULL)
  {
    return;
  }

  primask = __get_PRIMASK();
  __disable_irq();

  temp_tc_cal_init_defaults();

  for(uint32_t i = 0U; i < TEMP_TC_CAL_POINTS; i++)
  {
    points[i] = temp_tc_cal_points[i];
  }

  if(enabled != NULL)
  {
    *enabled = temp_tc_cal_enabled;
  }

  if(ref_c != NULL)
  {
    *ref_c = temp_tc_cal_ref_c;
  }

  if(primask == 0U)
  {
    __enable_irq();
  }
}

uint8_t temp_set_cal_curve(const temp_tc_cal_point_t *points,
                           uint8_t enabled,
                           float ref_c)
{
  uint32_t primask;

  if((points == NULL) || (ref_c < -20.0f) || (ref_c > 60.0f))
  {
    return 0U;
  }

  for(uint32_t i = 0U; i < TEMP_TC_CAL_POINTS; i++)
  {
    if((points[i].adc > TEMP_ADC_FULL_SCALE) ||
       (points[i].temp_c < -50.0f) ||
       (points[i].temp_c > 600.0f))
    {
      return 0U;
    }

    if((i > 0U) && (points[i].adc <= points[i - 1U].adc))
    {
      return 0U;
    }
  }

  primask = __get_PRIMASK();
  __disable_irq();

  for(uint32_t i = 0U; i < TEMP_TC_CAL_POINTS; i++)
  {
    temp_tc_cal_points[i] = points[i];
  }

  temp_tc_cal_enabled = (enabled != 0U) ? 1U : 0U;
  temp_tc_cal_ref_c = ref_c;
  temp_tc_cal_initialized = 1U;

  if(primask == 0U)
  {
    __enable_irq();
  }

  return 1U;
}

static uint8_t temp_room_valid;
static uint8_t temp_room_filter_init;
static uint8_t temp_tip_filter_init;
static float temp_room_filtered_c;
static float temp_tip_filtered_c;
static float temp_tip_hist[3];
static uint8_t temp_tip_hist_index;
static uint8_t temp_tip_hist_count;

/* 三点中值：抑制单次 ADC 尖峰，再进入 EMA */
static float temp_median3(float a, float b, float c)
{
  if(a > b)
  {
    float t = a;
    a = b;
    b = t;
  }

  if(b > c)
  {
    float t = b;
    b = c;
    c = t;
  }

  if(a > b)
  {
    b = a;
  }

  return b;
}

/**
  * @brief  一阶 IIR/EMA 温度平滑
  * @param  x_n: 本次周期采样值
  * @param  y_last: 上次平滑输出 y_{n-1}
  * @param  alpha: 平滑系数，要求 0 < alpha < 1
  * @retval 本次平滑输出 y_n
  * @note   y_n = alpha * x_n + (1 - alpha) * y_{n-1}
  *         alpha 越大跟随越快、平滑越弱；alpha 越小噪声抑制越强、滞后越大。
  */
static float temp_ema_step(float x_n, float y_last, float alpha)
{
  if(alpha <= 0.0f)
  {
    return y_last;
  }

  if(alpha >= 1.0f)
  {
    return x_n;
  }

  return (alpha * x_n) + ((1.0f - alpha) * y_last);
}

void temp_update_fast(const adc_raw_data_t *adc_raw,
                      temp_data_t *temp,
                      uint8_t update_room)
{
  float raw_room_c;
  float raw_tip_c;

  if((adc_raw == NULL) || (temp == NULL))
  {
    return;
  }

  temp->valid = 0U;

  if(adc_raw->valid == 0U)
  {
    /* ADC 本次失败：重新初始化滤波器，恢复时直接跟随原始值 */
    temp_room_filter_init = 0U;
    temp_tip_filter_init = 0U;
    return;
  }

  if(update_room != 0U)
  {
    raw_room_c = temp_ntc_adc_to_celsius(adc_raw->ntc);

    if((adc_raw->ntc <= TEMP_NTC_ADC_FAULT_LOW) ||
       (adc_raw->ntc >= TEMP_NTC_ADC_FAULT_HIGH))
    {
      temp_room_valid = 0U;
      temp_room_filter_init = 0U;
      temp_tip_filter_init = 0U;
      return;
    }

    temp_room_valid = 1U;

    if(temp_room_filter_init == 0U)
    {
      temp_room_filtered_c = raw_room_c;
      temp_room_filter_init = 1U;
    }
    else
    {
      temp_room_filtered_c = temp_ema_step(raw_room_c,
                                           temp_room_filtered_c,
                                           TEMP_NTC_FILTER_ALPHA);
    }

    temp->room_c = temp_room_filtered_c;
  }

  if(temp_room_valid == 0U)
  {
    temp_tip_filter_init = 0U;
    return;
  }

  /*
   * 初始热端温度直接继承冷端温度，避免上电/ADC 恢复瞬间
   * 热电偶通道尚未稳定时出现 0°C 或负温跳变。
   */
  if(temp_tip_filter_init == 0U)
  {
    temp_tip_filtered_c = temp_room_filtered_c;
    temp_tip_hist[0] = temp_room_filtered_c;
    temp_tip_hist[1] = temp_room_filtered_c;
    temp_tip_hist[2] = temp_room_filtered_c;
    temp_tip_hist_index = 0U;
    temp_tip_hist_count = 3U;
    temp_tip_filter_init = 1U;

    if(update_room == 0U)
    {
      temp->room_c = temp_room_filtered_c;
    }

    temp->tip_c = temp_tip_filtered_c;
    temp->valid = 1U;
    return;
  }

  /* PA1 / T-ADC：热电偶 + 冷端补偿，每个 20 kHz 周期更新 */
  raw_tip_c = temp_thermocouple_adc_to_celsius(adc_raw->thermocouple, temp->room_c);

  temp_tip_hist[temp_tip_hist_index] = raw_tip_c;
  temp_tip_hist_index = (uint8_t)((temp_tip_hist_index + 1U) % 3U);
  if(temp_tip_hist_count < 3U)
  {
    temp_tip_hist_count++;
  }

  if(temp_tip_hist_count >= 3U)
  {
    raw_tip_c = temp_median3(temp_tip_hist[0], temp_tip_hist[1], temp_tip_hist[2]);
  }

  temp_tip_filtered_c = temp_ema_step(raw_tip_c,
                                      temp_tip_filtered_c,
                                      TEMP_TC_FILTER_ALPHA);

  temp->tip_c = temp_tip_filtered_c;
  temp->valid = 1U;
}

void temp_update(const adc_raw_data_t *adc_raw, temp_data_t *temp)
{
  temp_update_fast(adc_raw, temp, 1U);
}
