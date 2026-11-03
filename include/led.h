#ifndef __LED_H
#define __LED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  初始化 PA6 LED
  * @param  none
  * @retval none
  * @note   PA6 -> 10k -> LED -> GND，高电平点亮；PA6 使用 TMR3_CH1。
  *         必须在 buzzer_init() 之前调用，二者共用 TMR3 时基。
  */
void led_init(void);

/**
  * @brief  LED 周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   加热运行：呼吸；待机/休眠：慢闪；HMS/故障：快闪。
  */
void led_task(uint32_t now_ms);

/**
  * @brief  按当前 TMR3 周期刷新 LED 占空比
  * @param  none
  * @retval none
  * @note   蜂鸣器改变 TMR3 ARR 后调用，保持 LED 亮度比例不变。
  */
void led_refresh_pwm(void);

#ifdef __cplusplus
}
#endif

#endif
