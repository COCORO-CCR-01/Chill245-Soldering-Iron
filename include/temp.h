#ifndef __TEMP_H
#define __TEMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "adc.h"

#define TEMP_TC_CAL_POINTS              (10U)

typedef struct
{
  uint16_t adc;      /* PA1 / T-ADC 原始值 */
  float temp_c;      /* 对应烙铁头实际温度 */
} temp_tc_cal_point_t;

/**
  * @brief 温度计算结果
  * @note  room_c 由 PA0 的 NTC 计算，作为热电偶冷端温度；
  *        tip_c 由 PA1 的热电偶信号 + 冷端补偿计算。
  */
typedef struct
{
  float room_c;    /* 室温 / 冷端温度，单位 °C */
  float tip_c;     /* 烙铁头温度，单位 °C */
  uint8_t valid;   /* 1：数据有效；0：ADC 无效或 NTC 开路/短路 */
} temp_data_t;

/**
  * @brief  NTC ADC 原始值转摄氏度
  * @param  adc: PA0 / T-NTC 的 12 位 ADC 原始值
  * @retval 温度，单位 °C；超出查表范围时返回 -20 或 100
  */
float temp_ntc_adc_to_celsius(uint16_t adc);

/**
  * @brief  热电偶 ADC 原始值转烙铁头温度
  * @param  adc: PA1 / T-ADC 的 12 位 ADC 原始值
  * @param  cold_junction_c: NTC 测得的冷端温度，单位 °C
  * @retval 烙铁头温度，单位 °C
  * @note   默认使用二次拟合 T=-1.1948e-6*N^2+0.19073*N+23.78，
  *         并按冷端与 23.78°C 的差值补偿；
  *         填入标定表后改为查表插值 + 冷端补偿。
  */
float temp_thermocouple_adc_to_celsius(uint16_t adc, float cold_junction_c);

/**
  * @brief  热电偶初始拟合公式（不依赖标定表）
  * @param  adc: PA1 原始 ADC
  * @param  cold_junction_c: 当前冷端温度
  * @retval 烙铁头温度，单位 °C
  */
float temp_thermocouple_default_celsius(uint16_t adc, float cold_junction_c);

/**
  * @brief  获取热电偶放大器零点对应的 ADC 原始值
  * @param  none
  * @retval 零点 ADC 值
  * @note   用于加热器保护模块判断热电偶短路。
  */
uint16_t temp_get_thermocouple_zero_adc(void);

/**
  * @brief  读取 10 点热电偶标定曲线
  * @param  points: 输出 10 个点
  * @param  enabled: 输出是否启用标定
  * @param  ref_c: 输出标定时的冷端温度
  * @retval none
  */
void temp_get_cal_curve(temp_tc_cal_point_t *points,
                        uint8_t *enabled,
                        float *ref_c);

/**
  * @brief  写入 10 点热电偶标定曲线
  * @param  points: 10 个点，ADC 必须严格递增
  * @param  enabled: 1 启用，0 使用理论线性换算
  * @param  ref_c: 标定时的冷端温度
  * @retval 1 成功；0 参数无效
  */
uint8_t temp_set_cal_curve(const temp_tc_cal_point_t *points,
                           uint8_t enabled,
                           float ref_c);

/**
  * @brief  根据三路 ADC 原始值更新温度数据
  * @param  adc_raw: ADC 原始数据
  * @param  temp: 温度输出
  * @retval none
  */
void temp_update(const adc_raw_data_t *adc_raw, temp_data_t *temp);

/**
  * @brief  20 kHz 快速温度更新
  * @param  adc_raw: ADC 原始数据
  * @param  temp: 温度输出（保留上次 room_c）
  * @param  update_room: 1 重新计算 NTC 冷端温度；0 复用上次值
  * @retval none
  * @note   热电偶烙铁头温度每次都更新，并做一阶 IIR/EMA 平滑：
  *         y_n = alpha*x_n + (1-alpha)*y_{n-1}，约 20 Hz；
  *         NTC 冷端每 10 ms 更新一次，EMA 约 1.6 Hz。
  *         ADC 无效或 NTC 故障时滤波器重新初始化，避免恢复时拖尾。
  */
void temp_update_fast(const adc_raw_data_t *adc_raw,
                      temp_data_t *temp,
                      uint8_t update_room);

#ifdef __cplusplus
}
#endif

#endif
