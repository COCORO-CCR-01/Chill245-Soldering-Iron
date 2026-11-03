#ifndef __BUTTONS_H
#define __BUTTONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define BUTTON_DEBOUNCE_MS              (20U)
#define BUTTON_LONG_PRESS_MS            (2000U)
#define BUTTON_REPEAT_MS                (200U)

typedef enum
{
  BUTTON_MENU = 0,   /* PA15，按下拉低 */
  BUTTON_PREV,       /* PB3，按下拉低 */
  BUTTON_NEXT,       /* PB4，按下拉低 */
  BUTTON_COUNT
} button_id_t;

typedef enum
{
  BUTTON_EVENT_PRESS = 0,
  BUTTON_EVENT_RELEASE,
  BUTTON_EVENT_CLICK,
  BUTTON_EVENT_LONG_PRESS,
  BUTTON_EVENT_REPEAT,
  BUTTON_EVENT_COUNT
} button_event_t;

typedef void (*button_callback_t)(button_id_t button, button_event_t event);

/**
  * @brief  初始化三个按键
  * @param  none
  * @retval none
  * @note   PA15=menu，PB3=prev，PB4=next，内部上拉，按下为低。
  *         会清空回调注册表，注册需在 buttons_init() 之后。
  */
void buttons_init(void);

/**
  * @brief  按键扫描
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   建议每 1 ms 调用一次；内部 20 ms 消抖。
  */
void buttons_update(uint32_t now_ms);

/**
  * @brief  注册按键事件回调
  * @param  button: 按键
  * @param  event: 事件
  * @param  callback: 回调
  * @retval 1 成功；0 失败（参数无效、表满或重复注册）
  */
uint8_t buttons_register_callback(button_id_t button,
                                  button_event_t event,
                                  button_callback_t callback);

/**
  * @brief  查询按键是否处于稳定按下状态
  * @param  button: 按键
  * @retval 1 按下；0 释放
  */
uint8_t buttons_is_pressed(button_id_t button);

/**
  * @brief  获取当前按下持续时间
  * @param  button: 按键
  * @param  now_ms: 当前时间戳
  * @retval ms；未按下返回 0
  */
uint32_t buttons_get_press_duration(button_id_t button, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
