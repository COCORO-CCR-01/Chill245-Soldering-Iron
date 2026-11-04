#ifndef __MENU_H
#define __MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "buttons.h"

/**
  * @brief  初始化设置菜单
  * @param  none
  * @retval none
  * @note   必须在 buttons_init() 之后调用。
  */
void menu_init(void);

/**
  * @brief  菜单周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   检测 MENU 长按 2 秒进入/返回，并在菜单打开时刷新屏幕。
  */
void menu_update(uint32_t now_ms);

/**
  * @brief  菜单是否打开
  * @param  none
  * @retval 1 打开；0 关闭
  */
uint8_t menu_is_open(void);

/**
  * @brief  打开菜单
  * @param  none
  * @retval none
  */
void menu_open(void);

/**
  * @brief  双击 MENU 打开快速设定温度编辑
  * @param  none
  * @retval none
  * @note   直接进入 TEMPERATURE -> SETPOINT 编辑，NEXT/PREV 调整，MENU 确认退出。
  */
void menu_open_quick_setpoint(void);

/**
  * @brief  关闭菜单并应用设置
  * @param  none
  * @retval none
  */
void menu_close(void);

/**
  * @brief  强制关闭菜单，不应用/保存设置
  * @param  none
  * @retval none
  * @note   HMS 等异常界面抢占屏幕时使用。
  */
void menu_force_close(void);

#ifdef __cplusplus
}
#endif

#endif
