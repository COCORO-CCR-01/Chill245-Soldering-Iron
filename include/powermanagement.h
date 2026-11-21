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

/* CH224A
 *
 * 注意：i2c_7bit_address_send() 内部只做 (addr & 0xFE)，不会帮你左移，
 * 所以传进去的必须是"预移位 8 位地址"。原来直接传 0x22 会让总线上发出
 * 7 位地址 0x11，CH224A 永远不应答 —— 这是本固件通讯不通的根因。
 * 实测：总线上 0x44(7 位 0x22) 和 0x46(7 位 0x23) 都会应答。 */
#define POWER_CH224A_ADDR_7BIT_PRIMARY          (0x22U)
#define POWER_CH224A_ADDR_7BIT_SECONDARY        (0x23U)
#define POWER_CH224A_I2C_ADDR_PRIMARY           (POWER_CH224A_ADDR_7BIT_PRIMARY << 1)   /* 0x44 */
#define POWER_CH224A_I2C_ADDR_SECONDARY         (POWER_CH224A_ADDR_7BIT_SECONDARY << 1) /* 0x46 */

#define POWER_CH224A_REG_STATUS                 (0x09U)
#define POWER_CH224A_REG_VOLTAGE                (0x0AU)
#define POWER_CH224A_REG_CURRENT_LIMIT          (0x50U)   /* 单位 50 mA */

/* 0x09 状态位（实测：未插 PD = 0x00；插入 = 0x48；档位切换成功 = 0xC8） */
#define POWER_CH224A_STATUS_PD                  (0x08U)   /* 已建立 PD 合同 */
#define POWER_CH224A_STATUS_CONTRACT_READY      (0x80U)   /* 合同已确认，此时 0x0A 请求才生效 */

/* 档位索引 */
#define POWER_CH224A_INDEX_5V                   (0U)
#define POWER_CH224A_INDEX_9V                   (1U)
#define POWER_CH224A_INDEX_12V                  (2U)
#define POWER_CH224A_INDEX_15V                  (3U)
#define POWER_CH224A_INDEX_20V                  (4U)
#define POWER_CH224A_INDEX_28V                  (5U)
/* 上电第一次写 0x0A 会被 CH224A 吞掉，用这一档把合同 commit 掉。
 * 选 9V 是因为 CFG1=205k 单电阻模式的默认档就是 9V，来回切换最安全。 */
#define POWER_CH224A_INDEX_COMMIT               POWER_CH224A_INDEX_9V

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

/* 最大加热占空比（功率限制） 10~100% */
uint8_t power_get_max_duty(void);
void power_set_max_duty(uint8_t percent);

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

/**
  * @brief  协商到指定档位（带 commit + 输入电压回读校验）
  * @param  index: 0..5 = 5/9/12/15/20/28 V
  * @retval 1 已确认到位；0 未到位（供电方没有这一档，或 CH224A 不在线）
  * @note   阻塞，最坏约 3.2 s，只适合在上电/菜单里调用；周期纠偏走 power_update。
  *         CH224A 的 0x09 有两个状态：0x48 = 收到 Source_Capabilities 但合同未确认，
  *         0xC8(bit7) = 合同已确认。上电后第一次写 0x0A 会被吞掉，所以这里先写
  *         一档把合同 commit，再打真正的目标档，最后用 PA2 实测 VBUS 确认。
  *         实测切换在写寄存器后 400 ms 内完成：20V = 19.86~19.92 V，
  *         15V = 14.89~14.95 V，9V = 8.90~8.97 V。
  */
uint8_t power_ch224a_negotiate_index(uint8_t index);

/**
  * @brief  协商到指定电压（浮点接口，内部换算档位）
  */
uint8_t power_ch224a_negotiate(float volts);

/**
  * @brief  读 CH224A 寄存器（单字节；该芯片不支持一次多字节流式读）
  * @retval 1 成功；0 失败（器件不应答或总线异常）
  */
uint8_t power_ch224a_read_reg(uint8_t reg, uint8_t *value);

/**
  * @brief  最近一次读到的 0x09 状态字节
  */
uint8_t power_ch224a_get_status(void);

/**
  * @brief  CH224A 是否在总线上（0x44 / 0x46 至少一个应答）
  */
uint8_t power_ch224a_is_present(void);

/**
  * @brief  当前正在维持的目标档位
  */
uint8_t power_ch224a_get_target_index(void);

#ifdef __cplusplus
}
#endif

#endif
