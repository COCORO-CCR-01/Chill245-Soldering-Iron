#ifndef __BUZZER_H
#define __BUZZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  蜂鸣器提示音类型
  * @note   每种音效使用不同的频率/节奏，全部为非阻塞播放。
  */
typedef enum
{
  BUZZER_SOUND_BOOT = 0,       /* 开机 */
  BUZZER_SOUND_KEY,            /* 按键 */
  BUZZER_SOUND_SETPOINT,       /* 到达预设温度 */
  BUZZER_SOUND_SLEEP,          /* 进入降温休眠 */
  BUZZER_SOUND_DEEP_SLEEP,     /* 关闭加热器 */
  BUZZER_SOUND_SHUTDOWN,       /* 关闭背光/关机 */
  BUZZER_SOUND_WAKE,           /* 从休眠唤醒 */
  BUZZER_SOUND_FAULT,          /* 故障警报 */
  BUZZER_SOUND_MODE,           /* 切换 PID 模式 */
  BUZZER_SOUND_MENU_OPEN,      /* 打开菜单 */
  BUZZER_SOUND_MENU_CLOSE,     /* 关闭菜单 */
  BUZZER_SOUND_AUTOTUNE_START, /* 开始自动整定 */
  BUZZER_SOUND_AUTOTUNE_DONE,  /* 自动整定完成 */
  BUZZER_SOUND_AUTOTUNE_FAIL,  /* 自动整定失败/取消 */
  BUZZER_SOUND_ERROR,          /* 操作错误 */
  BUZZER_SOUND_COUNT
} buzzer_sound_t;

/**
  * @brief  初始化蜂鸣器
  * @param  none
  * @retval none
  * @note   PB5 = TMR3_CH2，MUX_1，高电平经 NPN 驱动蜂鸣器。
  *         必须在 sm_init()、power_init()、menu_init() 之后调用。
  */
void buzzer_init(void);

/**
  * @brief  蜂鸣器周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   建议每 1 ms 调用；内部每 20 ms 轮询一次系统状态。
  */
void buzzer_task(uint32_t now_ms);

/**
  * @brief  重新同步系统状态，不播放提示音
  * @param  none
  * @retval none
  * @note   加载 EEPROM 参数后调用，避免把上电参数变化误判为模式切换。
  */
void buzzer_sync_state(void);

/**
  * @brief  播放指定提示音
  * @param  sound: 音效
  * @retval none
  * @note   高优先级音效会打断低优先级音效，低优先级不会打断高优先级。
  */
void buzzer_play(buzzer_sound_t sound);

/**
  * @brief  立即停止当前提示音
  * @param  none
  * @retval none
  */
void buzzer_stop(void);

/**
  * @brief  打开/关闭蜂鸣器
  * @param  enable: 1 打开，0 关闭
  * @retval none
  */
void buzzer_set_enabled(uint8_t enable);

/**
  * @brief  查询蜂鸣器是否使能
  * @param  none
  * @retval 1 使能；0 关闭
  */
uint8_t buzzer_is_enabled(void);

/**
  * @brief  设置蜂鸣器音量
  * @param  volume: 0~100，0 静音，100 最大
  * @retval none
  * @note   通过 PWM 占空比近似调节；100 对应 50% 占空比。
  */
void buzzer_set_volume(uint8_t volume);

/**
  * @brief  获取蜂鸣器音量
  * @param  none
  * @retval 0~100
  */
uint8_t buzzer_get_volume(void);

/**
  * @brief  查询是否正在播放
  * @param  none
  * @retval 1 播放中；0 空闲
  */
uint8_t buzzer_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif
