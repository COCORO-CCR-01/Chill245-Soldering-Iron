#ifndef __MAINDASH_H
#define __MAINDASH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "statemachine.h"

/**
  * @brief  初始化主界面
  * @param  none
  * @retval none
  */
void maindash_init(void);

/**
  * @brief  绘制主界面
  * @param  status: 控制快照
  * @retval none
  */
void maindash_draw(const sm_status_t *status);

/**
  * @brief  主界面周期刷新
  * @param  now_ms: 当前毫秒时间戳
  * @retval 1：本次执行了重绘；0：未到刷新时间
  * @note   内部限制约 5 Hz，避免频繁全屏刷新。
  */
uint8_t maindash_update(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
