#ifndef __STATEMACHINE_H
#define __STATEMACHINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "pid.h"
#include "verifyheater.h"

/* 控制环：20 kHz，50 µs */
#define SM_FAST_RATE_HZ                 (20000U)
#define SM_FAST_PERIOD_US                (50U)
/* 后台调试/慢任务：1 kHz */
#define SM_SLOW_PERIOD_MS                (1U)

/**
  * @brief 系统状态
  */
typedef enum
{
  SM_STATE_INIT = 0,     /* 初始化 */
  SM_STATE_HEATING,      /* 正常 PID 加热 */
  SM_STATE_AUTOTUNE,     /* PID 自动整定 */
  SM_STATE_DISABLED,     /* 人为禁用加热输出 */
  SM_STATE_FAULT         /* 保护故障锁存 */
} sm_state_t;

/**
  * @brief 供 UI/图表读取的控制快照
  */
typedef struct
{
  float tip_c;                  /* 烙铁头温度，°C */
  float room_c;                 /* NTC 冷端温度，°C */
  float setpoint_c;             /* 目标温度，°C */
  uint8_t temp_valid;           /* 温度有效 */
  uint8_t adc_valid;            /* ADC 采样有效 */
  uint8_t power;                /* 当前加热功率，0~100% */
  uint8_t heater_on;            /* PWM 占空比 > 0 */
  uint16_t vin_raw;             /* PA2 V-ADC 原始值 */
  uint16_t tc_raw;              /* PA1 热电偶原始 ADC（消隐采样） */
  uint16_t ntc_raw;             /* PA0 NTC 原始 ADC */
  sm_state_t state;             /* 系统状态 */
  pid_mode_t mode;              /* PID 曲线 */
  verifyheater_fault_t fault;   /* 保护故障码 */
  pid_autotune_state_t autotune_state; /* 自动整定状态 */
  uint32_t now_ms;              /* 控制时间戳 */
} sm_status_t;

/**
  * @brief  初始化所有模块并启动 20 kHz 控制定时器
  * @param  none
  * @retval none
  * @note   会初始化 ADC、温度、保护、PID、20 kHz PWM、调试串口，
  *         注册串口命令，并使能 TMR1 更新中断作为 20 kHz 控制节拍。
  *         调用前需完成系统时钟、NVIC 和时基配置。
  */
void sm_init(void);

/**
  * @brief  初始化加热独立看门狗
  * @param  none
  * @retval none
  * @note   必须在主循环开始前调用。硬件 WDT 由主循环喂狗，
  *         控制中断序列停滞后由 sm_heater_watchdog_task() 触发 HMS。
  */
void sm_heater_watchdog_init(void);

/**
  * @brief  加热独立看门狗任务
  * @param  none
  * @retval none
  * @note   每个主循环调用一次。若 20 kHz 控制序列停止，强制关加热并进入 HMS。
  */
void sm_heater_watchdog_task(void);

/**
  * @brief  后台慢任务（调试控制台）
  * @param  none
  * @retval none
  * @note   建议在主循环中每 SM_SLOW_PERIOD_MS 调用一次。
  *         内部读取控制环快照，不直接操作 20 kHz 控制数据。
  */
void sm_slow_update(void);

/**
  * @brief  获取当前状态
  * @param  none
  * @retval sm_state_t
  */
sm_state_t sm_get_state(void);

/**
  * @brief  获取 UI/图表使用的控制快照
  * @param  status: 输出
  * @retval none
  */
void sm_get_status(sm_status_t *status);

/**
  * @brief  获取控制时间戳
  * @param  none
  * @retval ms
  */
uint32_t sm_get_uptime_ms(void);

/**
  * @brief  获取状态名称
  * @param  state: 状态
  * @retval 静态字符串
  */
const char *sm_state_name(sm_state_t state);

/**
  * @brief  启用/禁用加热输出
  * @param  enabled: 1 启用，0 禁用
  * @retval 1 成功；0 失败（故障锁存时无法启用）
  * @note   禁用会立即关闭 PWM 并复位 PID；启用会从安全状态重新开始。
  */
uint8_t sm_set_heater_enabled(uint8_t enabled);

/**
  * @brief  查询加热输出是否允许
  * @param  none
  * @retval 1 允许；0 禁用或故障
  */
uint8_t sm_is_heater_enabled(void);

/**
  * @brief  清除保护故障并进入禁用状态
  * @param  none
  * @retval 1 成功
  * @note   清除后不会自动恢复加热，需再调用 sm_set_heater_enabled(1)。
  */
uint8_t sm_clear_fault(void);

/**
  * @brief  启动自动整定
  * @param  setpoint_c: 整定目标温度
  * @retval 1 成功；0 拒绝（故障/禁用/温度无效/已在整定）
  */
uint8_t sm_start_autotune(float setpoint_c);

/**
  * @brief  取消自动整定
  * @param  none
  * @retval none
  */
void sm_cancel_autotune(void);

/**
  * @brief  设置目标温度
  * @param  setpoint_c: 目标温度
  * @retval 1 成功；0 自动整定期间拒绝
  */
uint8_t sm_set_setpoint(float setpoint_c);

/**
  * @brief  获取目标温度
  * @param  none
  * @retval °C
  */
float sm_get_setpoint(void);

/**
  * @brief  设置 PID 曲线模式
  * @param  mode: PID_MODE_STANDARD / BOOST / CUSTOM
  * @retval 1 成功；0 自动整定期间或参数无效
  */
uint8_t sm_set_pid_mode(pid_mode_t mode);

/**
  * @brief  获取 PID 曲线模式
  * @param  none
  * @retval pid_mode_t
  */
pid_mode_t sm_get_pid_mode(void);

/**
  * @brief  复位 PID 运行状态
  * @param  none
  * @retval none
  * @note   若正在自动整定，会取消整定并回到 HEATING。
  */
void sm_reset_pid(void);

/**
  * @brief  获取当前加热功率
  * @param  none
  * @retval 0~100
  */
uint8_t sm_get_heater_power(void);

/**
  * @brief  获取保护故障码
  * @param  none
  * @retval verifyheater_fault_t
  */
verifyheater_fault_t sm_get_fault(void);

#ifdef __cplusplus
}
#endif

#endif
