#ifndef __VERIFYHEATER_H
#define __VERIFYHEATER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "adc.h"
#include "temp.h"

/**
  * @brief 加热器保护故障码
  */
typedef enum
{
  VERIFY_HEATER_FAULT_NONE = 0,       /* 无故障 */
  VERIFY_HEATER_FAULT_TC_OPEN,        /* 热电偶开路 */
  VERIFY_HEATER_FAULT_TC_SHORT,       /* 热电偶短路 */
  VERIFY_HEATER_FAULT_TEMP_INVALID,   /* 温度数据无效（NTC 开路/短路、ADC 异常） */
  VERIFY_HEATER_FAULT_NO_RISE,        /* 30s 内温度无法上升 */
  VERIFY_HEATER_FAULT_TEMP_JUMP,      /* 温度突变太快 */
  VERIFY_HEATER_FAULT_TEMP_DROP,      /* 严重失温 */
  VERIFY_HEATER_FAULT_OVERTEMP,       /* 温度超过 500°C */
  VERIFY_HEATER_FAULT_FULL_POWER      /* 持续 100% 满功率运行 30s */
} verifyheater_fault_t;

/**
  * @brief  可配置的加热器保护阈值
  */
typedef struct
{
  float overtemp_c;
  float temp_jump_delta_c;
  float temp_jump_rate_c_per_s;
  uint32_t no_rise_timeout_ms;
  float no_rise_delta_c;
  uint32_t full_power_timeout_ms;
  float temp_drop_delta_c;
} verifyheater_config_t;

/**
  * @brief  初始化加热器保护模块
  * @param  none
  * @retval none
  */
void verifyheater_init(void);

/**
  * @brief  注册/更新当前加热器输出功率
  * @param  percent: 0~100，单位 %
  * @retval none
  * @note   后续 heater.c 计算完功率后每个控制周期调用一次；
  *         请传 PID 输出的平均功率，不要传 PWM 的瞬时通断状态。
  *         20 kHz 架构下由 statemachine 以 1 kHz 调用。
  *         发生故障锁存后，本函数会把功率强制保持为 0。
  */
void verifyheater_set_heater_power(uint8_t percent);

/**
  * @brief  获取当前登记的加热器功率
  * @param  none
  * @retval 0~100，单位 %
  */
uint8_t verifyheater_get_heater_power(void);

/**
  * @brief  周期性运行加热器异常检测
  * @param  temp: 温度换算结果
  * @param  adc_raw: ADC 原始数据（用于热电偶开路/短路判断）
  * @param  now_ms: 毫秒时间戳，需要单调递增，允许 32 位回绕
  * @retval none
  * @note   故障一旦触发会锁存，直到调用 verifyheater_reset()。
  *         建议以 1 kHz 调用；20 kHz 中断只负责在锁存后立即切断 PWM。
  */
void verifyheater_update(const temp_data_t *temp,
                         const adc_raw_data_t *adc_raw,
                         uint32_t now_ms);

/**
  * @brief  查询是否处于故障锁存状态
  * @param  none
  * @retval 1：有故障；0：正常
  */
uint8_t verifyheater_is_fault(void);

/**
  * @brief  获取当前故障码
  * @param  none
  * @retval verifyheater_fault_t
  */
verifyheater_fault_t verifyheater_get_fault(void);

/**
  * @brief  清除故障并重新开始检测
  * @param  none
  * @retval none
  * @note   必须在故障原因排除后调用，否则会再次触发。
  */
void verifyheater_reset(void);

/**
  * @brief  读取保护阈值
  * @param  config: 输出配置
  * @retval none
  */
void verifyheater_get_config(verifyheater_config_t *config);

/**
  * @brief  写入保护阈值
  * @param  config: 配置
  * @retval 1 成功；0 参数无效
  */
uint8_t verifyheater_set_config(const verifyheater_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
