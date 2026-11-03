#ifndef __TEMPCAL_H
#define __TEMPCAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  初始化热端校准：注册按键和调试命令
  * @param  none
  * @retval none
  * @note   必须在 buttons_init()、debug_init()、sm_init() 之后调用。
  */
void tempcal_init(void);

/**
  * @brief  启动 100/150/200/250/300/350/400°C 七点热端校准
  * @param  none
  * @retval 1 启动成功；0 当前有故障/整定或已在校准
  */
uint8_t tempcal_start(void);

/**
  * @brief  取消校准并恢复原设定
  * @param  none
  * @retval none
  */
void tempcal_cancel(void);

/**
  * @brief  校准周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   主循环调用；负责稳定判定、消隐 ADC 平均和校准屏绘制。
  */
void tempcal_task(uint32_t now_ms);

/**
  * @brief  是否正在占用校准屏
  * @param  none
  * @retval 1 是；0 否
  */
uint8_t tempcal_is_active(void);

/**
  * @brief  提交外部实测温度
  * @param  actual_c: 实测热端温度，°C
  * @retval 1 成功；0 当前不在等待输入
  */
uint8_t tempcal_submit_actual(float actual_c);

/**
  * @brief  调整等待输入的实测温度
  * @param  delta_c: 增量
  * @retval none
  */
void tempcal_adjust_actual(float delta_c);

#ifdef __cplusplus
}
#endif

#endif
