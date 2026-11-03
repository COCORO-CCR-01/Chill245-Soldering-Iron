#ifndef __PID_H
#define __PID_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief PID 曲线模式
  */
typedef enum
{
  PID_MODE_STANDARD = 0,   /* 标准曲线：自动整定得到，偏稳健 */
  PID_MODE_BOOST,          /* boost 加强曲线：由标准曲线自动放大得到 */
  PID_MODE_CUSTOM,         /* 用户自定义曲线 */
  PID_MODE_COUNT
} pid_mode_t;

/**
  * @brief 一组 PID 曲线参数
  * @note  输出为 0~100% 加热功率；
  *        kp 单位 %/°C，ki 单位 %/(°C·s)，kd 单位 %·s/°C。
  */
typedef struct
{
  float kp;
  float ki;
  float kd;
} pid_curve_t;

#define PID_CUSTOM_POINTS               (10U)

/**
  * @brief  自定义 PID 曲线的一个温度点
  * @note   在 temp_c 处使用 gain；点之间线性插值。
  */
typedef struct
{
  float temp_c;
  pid_curve_t gain;
} pid_custom_point_t;

/**
  * @brief 自动整定状态
  */
typedef enum
{
  PID_AUTOTUNE_IDLE = 0,   /* 未运行 */
  PID_AUTOTUNE_RUNNING,    /* 正在 PWM 双电平整定 */
  PID_AUTOTUNE_DONE,       /* 整定成功 */
  PID_AUTOTUNE_FAILED      /* 整定失败 */
} pid_autotune_state_t;

/**
  * @brief 自动整定失败原因
  */
typedef enum
{
  PID_AUTOTUNE_ERR_NONE = 0,
  PID_AUTOTUNE_ERR_TIMEOUT,        /* 整定超时 */
  PID_AUTOTUNE_ERR_OVERTEMP,       /* 整定过程中温度超过安全上限 */
  PID_AUTOTUNE_ERR_NO_OSCILLATION, /* 振荡幅度太小或周期无效 */
  PID_AUTOTUNE_ERR_TEMP_INVALID,   /* 温度数据无效 */
  PID_AUTOTUNE_ERR_CANCELLED       /* 被上层取消 */
} pid_autotune_error_t;

/**
  * @brief 自动整定结果
  */
typedef struct
{
  pid_autotune_state_t state;
  pid_autotune_error_t error;
  float ku;                /* 临界增益，%/°C */
  float pu_s;              /* 临界周期，s */
  float amplitude_c;       /* 温度峰峰值，°C */
  uint8_t cycles;          /* 有效振荡周期数 */
  pid_curve_t standard_curve;
  pid_curve_t boost_curve;
} pid_autotune_result_t;

/**
  * @brief PID 当前状态，供显示/调试使用
  */
typedef struct
{
  float setpoint_c;
  float temperature_c;
  float error_c;
  float output;            /* 0~100% */
  pid_mode_t mode;
  pid_curve_t curve;
  pid_autotune_state_t autotune_state;
  pid_autotune_error_t autotune_error;
} pid_status_t;

/**
  * @brief  初始化 PID 模块（默认标准曲线、300°C）
  * @param  none
  * @retval none
  */
void pid_init(void);

/**
  * @brief  复位 PID 运行状态（积分、微分、输出）
  * @param  none
  * @retval none
  * @note   会同时取消正在运行的自动整定；曲线参数和设定温度保留。
  */
void pid_reset(void);

/**
  * @brief  设置目标温度
  * @param  setpoint_c: 目标温度，°C，内部限幅 0~450
  * @retval none
  */
void pid_set_setpoint(float setpoint_c);

/**
  * @brief  获取目标温度
  * @param  none
  * @retval °C
  */
float pid_get_setpoint(void);

/**
  * @brief  切换标准/boost/自定义曲线
  * @param  mode: PID_MODE_STANDARD / PID_MODE_BOOST / PID_MODE_CUSTOM
  * @retval none
  */
void pid_set_mode(pid_mode_t mode);

/**
  * @brief  获取当前曲线模式
  * @param  none
  * @retval pid_mode_t
  */
pid_mode_t pid_get_mode(void);

/**
  * @brief  设置指定模式的 PID 曲线
  * @param  mode: 要修改的曲线
  * @param  curve: 曲线参数
  * @retval none
  * @note   修改 PID_MODE_STANDARD 时会自动重新生成 boost 曲线；
  *         修改 PID_MODE_CUSTOM 不会自动切换模式，需调用 pid_set_mode()
  *         或直接使用 pid_set_custom_curve()。
  */
void pid_set_curve(pid_mode_t mode, const pid_curve_t *curve);

/**
  * @brief  读取指定模式的 PID 曲线
  * @param  mode: 曲线模式
  * @param  curve: 输出
  * @retval none
  */
void pid_get_curve(pid_mode_t mode, pid_curve_t *curve);

/**
  * @brief  设置并立即启用自定义 PID 曲线
  * @param  curve: 自定义曲线
  * @retval none
  */
void pid_set_custom_curve(const pid_curve_t *curve);

/**
  * @brief  读取 10 点自定义 PID 曲线
  * @param  points: 输出 10 个点
  * @retval none
  */
void pid_get_custom_points(pid_custom_point_t *points);

/**
  * @brief  写入 10 点自定义 PID 曲线
  * @param  points: 10 个点，temp_c 必须严格递增
  * @retval 1 成功；0 参数无效
  */
uint8_t pid_set_custom_points(const pid_custom_point_t *points);

/**
  * @brief  根据当前标准曲线重新生成 boost 加强曲线
  * @param  none
  * @retval none
  * @note   默认 boost = 标准曲线 × (1.6, 1.4, 1.2)，
  *         即更激进的比例、积分和微分作用。
  */
void pid_generate_boost(void);

/**
  * @brief  PID 周期计算（毫秒时间戳版本）
  * @param  temperature_c: 当前烙铁头温度，°C
  * @param  temperature_valid: 1 有效，0 无效
  * @param  now_ms: 毫秒时间戳，需单调递增，允许 32 位回绕
  * @retval 加热功率 0~100%
  * @note   正常 20 kHz 控制请使用 pid_update_fixed()；
  *         本函数主要用于 1 kHz 自动整定步骤或低速调试。
  *         自动整定期间返回 PWM 双电平的高/低占空比，而不是 PID 输出。
  */
float pid_update(float temperature_c, uint8_t temperature_valid, uint32_t now_ms);

/**
  * @brief  PID 固定周期计算（20 kHz 控制环使用）
  * @param  temperature_c: 当前烙铁头温度，°C
  * @param  temperature_valid: 1 有效，0 无效
  * @param  dt_s: 控制周期，单位秒；20 kHz 时传 0.00005f
  * @retval 加热功率 0~100%
  * @note   不读取毫秒时间戳，适合在 20 kHz 定时器中断中调用。
  *         自动整定运行期间不执行普通 PID，返回当前输出。
  */
float pid_update_fixed(float temperature_c, uint8_t temperature_valid, float dt_s);

/**
  * @brief  获取最近一次 PID 输出
  * @param  none
  * @retval 0~100%
  */
float pid_get_output(void);

/**
  * @brief  读取 PID 完整状态
  * @param  status: 输出
  * @retval none
  */
void pid_get_status(pid_status_t *status);

/**
  * @brief  启动 PWM 双电平自动整定
  * @param  setpoint_c: 整定目标温度；传 0 表示使用当前设定温度
  * @retval 1 启动成功；0 正在整定或目标温度不合法
  * @note   整定期间请把 pid_update() 的返回值直接接到 heater，
  *         整定会通过 heater 驱动 PMOS，以 10%/90% 两个 PWM 电平
  *         让温度围绕目标值振荡，不进行 0/100% 通断。
  *         完成后自动生成标准曲线，并由标准曲线生成 boost 曲线。
  *         启动前请确认加热器保护无故障、温度数据有效。
  */
uint8_t pid_autotune_start(float setpoint_c);

/**
  * @brief  取消自动整定
  * @param  none
  * @retval none
  */
void pid_autotune_cancel(void);

/**
  * @brief  查询是否正在自动整定
  * @param  none
  * @retval 1 正在整定；0 否
  */
uint8_t pid_is_autotuning(void);

/**
  * @brief  获取自动整定状态
  * @param  none
  * @retval pid_autotune_state_t
  */
pid_autotune_state_t pid_autotune_get_state(void);

/**
  * @brief  获取自动整定失败原因
  * @param  none
  * @retval pid_autotune_error_t
  */
pid_autotune_error_t pid_autotune_get_error(void);

/**
  * @brief  获取自动整定结果
  * @param  result: 输出；state/error 为当前状态，其余字段在 DONE 后有效
  * @retval none
  */
void pid_autotune_get_result(pid_autotune_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
