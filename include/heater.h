#ifndef __HEATER_H
#define __HEATER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  初始化 20 kHz 硬件 PWM 加热器
  * @param  none
  * @retval none
  * @note   PA8 复用为 TMR1_CH1，低电平导通、高电平关断。
  *         原理图功率管 Q6 为 P 沟道 S-LP7413DT3WG；
  *         Q8 2N7002W 只是栅极驱动 NMOS，不是功率管。
  *         PWM 频率 20 kHz，占空比 0~100%，初始化后默认 0%。
  */
void heater_init(void);

/**
  * @brief  设置加热器 PWM 占空比
  * @param  percent: 0~100，单位 %
  * @retval none
  * @note   直接写入 TMR1 CH1 比较值，立即生效。
  *         0% 输出高电平关断，100% 输出低电平持续导通。
  */
void heater_set_power(uint8_t percent);

/**
  * @brief  获取当前设置的加热器功率
  * @param  none
  * @retval 0~100，单位 %
  */
uint8_t heater_get_power(void);

/**
  * @brief  立即强制关闭加热器
  * @param  none
  * @retval none
  */
void heater_force_off(void);

/**
  * @brief  查询加热器当前是否允许导通
  * @param  none
  * @retval 1：占空比大于 0；0：关断
  */
uint8_t heater_is_on(void);

#ifdef __cplusplus
}
#endif

#endif
