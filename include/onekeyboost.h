#ifndef __ONEKEYBOOST_H
#define __ONEKEYBOOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  初始化 OneKeyBoost：注册 NEXT 长按 / PREV 短按
  * @param  none
  * @retval none
  */
void onekeyboost_init(void);

/**
  * @brief  OneKeyBoost 周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   负责掉温检测、300 s 超时和异常退出。
  */
void onekeyboost_task(uint32_t now_ms);

/**
  * @brief  查询 Boost 是否激活
  * @param  none
  * @retval 1 激活；0 未激活
  */
uint8_t onekeyboost_is_active(void);

/**
  * @brief  手动启动/重启 Boost
  * @param  none
  * @retval 1 成功；0 当前不允许
  */
uint8_t onekeyboost_start_manual(void);

/**
  * @brief  退出 Boost 并恢复原设定
  * @param  none
  * @retval none
  */
void onekeyboost_cancel(void);

/**
  * @brief  获取设置步进（°C）
  * @param  none
  * @retval 1~100
  */
float onekeyboost_get_setpoint_step(void);

#ifdef __cplusplus
}
#endif

#endif
