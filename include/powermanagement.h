#ifndef __POWERMANAGEMENT_H
#define __POWERMANAGEMENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* V-ADC = VCC * R2 / (R1 + R2)，原理图 R1=10k、R2=1k */
#define POWER_VIN_DIVIDER_R1            (10U)
#define POWER_VIN_DIVIDER_R2            (1U)
#define POWER_VIN_VREF_MV               (3300U)
#define POWER_ADC_FULL_SCALE            (4095U)

/* 默认休眠参数（可用 setter 修改） */
#define POWER_DEFAULT_STATIONARY_TIMEOUT_MS     (300000U)   /* 5 min */
#define POWER_DEFAULT_REDUCED_TEMP_C            (150.0f)
#define POWER_DEFAULT_REDUCED_OFF_TIMEOUT_MS    (600000U)   /* 10 min */
#define POWER_DEFAULT_BACKLIGHT_OFF_TIMEOUT_MS  (300000U)   /* 5 min */

/* 默认电压保护参数 */
#define POWER_DEFAULT_LOW_VOLTAGE_MV            (18000U)
#define POWER_DEFAULT_HIGH_VOLTAGE_MV           (30000U)
#define POWER_DEFAULT_VOLTAGE_HYSTERESIS_MV     (1000U)
#define POWER_DEFAULT_VOLTAGE_TRIP_MS           (100U)
#define POWER_DEFAULT_VOLTAGE_RECOVERY_MS       (2000U)
#define POWER_VOLTAGE_STARTUP_GRACE_MS          (2000U)

/* CH224A */
#define POWER_CH224A_REG_VOLTAGE                (0x0AU)
#define POWER_CH224A_I2C_ADDR_PRIMARY           (0x22U)
#define POWER_CH224A_I2C_ADDR_SECONDARY         (0x23U)
#define POWER_CH224A_DEFAULT_VOLTAGE_V          (20.0f)

typedef enum
{
  POWER_STATE_ACTIVE = 0,     /* 正常工作 */
  POWER_STATE_REDUCED,        /* 长时间静止，已降低目标温度 */
  POWER_STATE_HEATER_OFF,     /* 已关闭加热器 */
  POWER_STATE_BACKLIGHT_OFF   /* 已关闭背光，仅按键可唤醒 */
} power_state_t;

typedef enum
{
  POWER_FAULT_NONE = 0,
  POWER_FAULT_UNDERVOLTAGE,
  POWER_FAULT_OVERVOLTAGE
} power_fault_t;

typedef void (*power_fault_callback_t)(power_fault_t fault, uint16_t voltage_mv);

/**
  * @brief  初始化电源管理
  * @param  none
  * @retval none
  * @note   初始化 PB1 震动开关、CH224A I2C、休眠状态和按键唤醒回调。
  *         必须在 buttons_init() 之后调用。
  */
void power_init(void);

/**
  * @brief  电源管理周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   建议每 1 ms 调用一次。
  */
void power_update(uint32_t now_ms);

/**
  * @brief  V-ADC 原始值转输入电压
  * @param  raw: PA2 ADC 原始值
  * @retval mV
  */
uint16_t power_vin_raw_to_mv(uint16_t raw);

/**
  * @brief  获取最近一次输入电压
  * @param  none
  * @retval mV
  */
uint16_t power_get_voltage_mv(void);

/**
  * @brief  获取休眠状态
  * @param  none
  * @retval power_state_t
  */
power_state_t power_get_state(void);

/**
  * @brief  获取电源保护故障
  * @param  none
  * @retval power_fault_t
  */
power_fault_t power_get_fault(void);

/**
  * @brief  强制唤醒并恢复加热
  * @param  none
  * @retval none
  * @note   按键按下时自动调用；也可由 UI 调用。
  */
void power_wake(void);

/**
  * @brief  手动进入休眠：关闭加热和背光，任意按键唤醒
  */
void power_sleep(void);

/**
  * @brief  只关闭加热器，保持屏幕；唤醒后加热仍保持关闭
  */
void power_heater_off(void);

/**
  * @brief  深度休眠：关闭加热器和背光；唤醒后加热仍保持关闭
  */
void power_sleep_deep(void);

/**
  * @brief  是否处于已关加热/背光的休眠状态
  */
uint8_t power_is_sleeping(void);

/**
  * @brief  震动开关当前/最近触发状态
  * @retval 1：震动触发；0：静止
  */
uint8_t power_get_vibration_state(void);

/**
  * @brief  距离下一次自动休眠/关屏的剩余毫秒
  */
uint32_t power_get_sleep_remaining_ms(void);

/**
  * @brief  取走一次“NEXT 唤醒点击”标记，避免唤醒同时翻页
  * @retval 1：本次 NEXT 点击用于唤醒；0：普通点击
  */
uint8_t power_take_wake_click(void);

/**
  * @brief  注册电源故障回调
  * @param  callback: 回调，可为 NULL
  * @retval none
  * @note   欠压/过压锁存时调用，用于蜂鸣器、UI 报错。
  */
void power_register_fault_callback(power_fault_callback_t callback);

/**
  * @brief  清除电源故障
  * @param  none
  * @retval 1 成功；0 当前电压仍不在安全范围
  */
uint8_t power_clear_fault(void);

/**
  * @brief  菜单/UI 活动通知
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   刷新静止计时，避免在菜单操作时进入休眠。
  */
void power_notify_activity(uint32_t now_ms);

/* 休眠参数读取/设置 */
uint32_t power_get_stationary_timeout_ms(void);
uint8_t power_get_low_power_threshold(void);
float power_get_reduced_temperature(void);
uint32_t power_get_reduced_off_timeout_ms(void);
uint32_t power_get_backlight_off_timeout_ms(void);

void power_set_stationary_timeout_ms(uint32_t ms);
void power_set_low_power_threshold(uint8_t percent);
void power_set_reduced_temperature(float celsius);
void power_set_reduced_off_timeout_ms(uint32_t ms);
void power_set_backlight_off_timeout_ms(uint32_t ms);

/* 电压保护参数读取/设置 */
void power_get_voltage_limits(float *low_v, float *high_v);
float power_get_voltage_hysteresis(void);
uint32_t power_get_voltage_trip_time_ms(void);
uint32_t power_get_voltage_recovery_time_ms(void);

void power_set_voltage_limits(float low_v, float high_v);
void power_set_voltage_hysteresis(float volts);
void power_set_voltage_trip_time_ms(uint32_t ms);
void power_set_voltage_recovery_time_ms(uint32_t ms);

/* CH224A 电压诱骗 */
float power_get_ch224a_voltage(void);
uint8_t power_ch224a_set_voltage(float volts);
uint8_t power_ch224a_set_voltage_index(uint8_t index);

#ifdef __cplusplus
}
#endif

#endif
