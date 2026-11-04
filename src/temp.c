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
/* R25、B 值仅用于离线生成下方查表，运行时直接查表插值 */
#define TEMP_NTC_R25_OHM                (100000.0f)
#define TEMP_NTC_BETA                   (3950.0f)
#define TEMP_NTC_SERIES_OHM             (10000.0f)
#define TEMP_ADC_FULL_SCALE             (4095.0f)

/*
 * NTC 分压拓扑：
 *   1 = 10kΩ 上拉到 VREF，NTC 下拉到 GND（默认，常见接法）
 *   0 = NTC 上拉到 VREF，10kΩ 下拉到 GND
 * 如果实际电路与默认相反，只需把这里改成 0。
 */
#define TEMP_NTC_CONNECT_TO_GND         (1)

/*
 * NTC 温度-电阻表：
 *   由 Beta 方程 R(T) = R25 * exp(B * (1/T - 1/T25)) 离线生成，
 *   温度范围 -20~100°C，步进 2°C，运行时做线性插值。
 *   表项为 NTC 电阻值，单位 Ω。
 */
#define TEMP_NTC_TABLE_MIN_C            (-20.0f)
#define TEMP_NTC_TABLE_STEP_C           (2.0f)
#define TEMP_NTC_TABLE_POINTS           (61U)

static const float temp_ntc_table[TEMP_NTC_TABLE_POINTS] =
{
  1053846.90f,   /* -20 C */
  932523.94f,   /* -18 C */
  826739.54f,   /* -16 C */
  734318.62f,   /* -14 C */
  653414.78f,   /* -12 C */
  582457.13f,   /* -10 C */
  520106.32f,   /*  -8 C */
  465218.03f,   /*  -6 C */
  416812.55f,   /*  -4 C */
  374049.37f,   /*  -2 C */
  336206.04f,   /*   0 C */
  302660.34f,   /*   2 C */
  272875.40f,   /*   4 C */
  246387.08f,   /*   6 C */
  222793.44f,   /*   8 C */
  201745.77f,   /*  10 C */
  182941.00f,   /*  12 C */
  166115.28f,   /*  14 C */
  151038.53f,   /*  16 C */
  137509.80f,   /*  18 C */
  125353.26f,   /*  20 C */
  114414.85f,   /*  22 C */
  104559.36f,   /*  24 C */
   95667.96f,   /*  26 C */
   87636.05f,   /*  28 C */
   80371.41f,   /*  30 C */
   73792.63f,   /*  32 C */
   67827.74f,   /*  34 C */
   62413.05f,   /*  36 C */
   57492.07f,   /*  38 C */
   53014.67f,   /*  40 C */
   48936.30f,   /*  42 C */
   45217.30f,   /*  44 C */
   41822.34f,   /*  46 C */
   38719.90f,   /*  48 C */
   35881.83f,   /*  50 C */
   33282.93f,   /*  52 C */
   30900.66f,   /*  54 C */
   28714.81f,   /*  56 C */
   26707.24f,   /*  58 C */
   24861.65f,   /*  60 C */
   23163.39f,   /*  62 C */
   21599.25f,   /*  64 C */
   20157.36f,   /*  66 C */
   18826.96f,   /*  68 C */
   17598.37f,   /*  70 C */
   16462.83f,   /*  72 C */
   15412.39f,   /*  74 C */
   14439.89f,   /*  76 C */
   13538.80f,   /*  78 C */
   12703.20f,   /*  80 C */
   11927.74f,   /*  82 C */
   11207.51f,   /*  84 C */
   10538.08f,   /*  86 C */
    9915.40f,   /*  88 C */
    9335.77f,   /*  90 C */
    8795.83f,   /*  92 C */
    8292.49f,   /*  94 C */
    7822.96f,   /*  96 C */
    7384.64f,   /*  98 C */
    6975.20f,   /* 100 C */
};

_Static_assert((sizeof(temp_ntc_table) / sizeof(temp_ntc_table[0])) == TEMP_NTC_TABLE_POINTS,
               "temp_ntc_table size mismatch");

/* NTC 开路/短路判定：ADC 接近 0 或 4095 时认为传感器异常 */
#define TEMP_NTC_ADC_FAULT_LOW          (2U)
#define TEMP_NTC_ADC_FAULT_HIGH         (4093U)

/* NTC 冷端实测比实际高约 10°C，直接在换算结果上做一阶修正 */
#define TEMP_NTC_CAL_OFFSET_C           (-10.0f)

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
#define TEMP_TC_POLY_A                  (-1.1948e-6f)
#define TEMP_TC_POLY_B                  (0.19073f)
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
  float tip_c;

  tip_c = (TEMP_TC_POLY_A * n * n) +
          (TEMP_TC_POLY_B * n) +
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

#if TEMP_NTC_CONNECT_TO_GND
  /* V_adc = VREF * R_ntc / (10k + R_ntc) */
  resistance = TEMP_NTC_SERIES_OHM * adc_f / (TEMP_ADC_FULL_SCALE - adc_f);
#else
  /* V_adc = VREF * 10k / (R_ntc + 10k) */
  resistance = TEMP_NTC_SERIES_OHM * (TEMP_ADC_FULL_SCALE - adc_f) / adc_f;
#endif

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
