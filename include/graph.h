#ifndef __GRAPH_H
#define __GRAPH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define GRAPH_POINTS                    (80U)
#define GRAPH_SAMPLE_MS                 (250U)

/**
  * @brief  初始化曲线历史
  * @param  none
  * @retval none
  */
void graph_init(void);

/**
  * @brief  周期采样曲线数据
  * @param  now_ms: 当前毫秒时间戳
  * @retval 1：本次产生了新样本；0：未到采样时间
  * @note   每 GRAPH_SAMPLE_MS 采样一次，保存 80 点（约 20 秒）。
  */
uint8_t graph_task(uint32_t now_ms);

/**
  * @brief  绘制热端温度 + 功率占比图（同一图表）
  * @param  none
  * @retval none
  */
void graph_draw_temp_power(void);

/**
  * @brief  绘制输入电压曲线
  * @param  none
  * @retval none
  */
void graph_draw_voltage(void);

/**
  * @brief  绘制冷端温度曲线
  * @param  none
  * @retval none
  */
void graph_draw_cold(void);

#ifdef __cplusplus
}
#endif

#endif
