#ifndef __AUTOROLL_H
#define __AUTOROLL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "buttons.h"

typedef enum
{
  AUTOROLL_PAGE_MAIN = 0,
  AUTOROLL_PAGE_TEMP_POWER,
  AUTOROLL_PAGE_VOLTAGE,
  AUTOROLL_PAGE_COLD,
  AUTOROLL_PAGE_COUNT
} autoroll_page_t;

/**
  * @brief  初始化自动轮换
  * @param  interval_ms: 每个页面停留时间，内部限幅 1000~60000 ms
  * @retval none
  */
void autoroll_init(uint32_t interval_ms);

/**
  * @brief  周期任务
  * @param  now_ms: 当前毫秒时间戳
  * @retval none
  * @note   负责采样曲线、自动换页、页面绘制和页面指示。
  */
void autoroll_update(uint32_t now_ms);

/**
  * @brief  按键事件入口
  * @param  button: 按键
  * @param  event: 事件
  * @retval none
  * @note   PREV/NEXT 单击翻页，长按/连发快速翻页；MENU 暂不注册，留给后续 UI 菜单。
  *         自动轮换的计时会在手动翻页后重新开始。
  */
void autoroll_on_button(button_id_t button, button_event_t event);

/**
  * @brief  设置自动轮换间隔
  * @param  interval_ms: 1000~60000 ms
  * @retval none
  */
void autoroll_set_interval(uint32_t interval_ms);

/**
  * @brief  获取轮换间隔
  * @param  none
  * @retval ms
  */
uint32_t autoroll_get_interval(void);

/**
  * @brief  开启/关闭自动轮换
  * @param  enable: 1 开启，0 关闭
  * @retval none
  */
void autoroll_enable(uint8_t enable);

/**
  * @brief  查询自动轮换是否开启
  * @param  none
  * @retval 1/0
  */
uint8_t autoroll_is_enabled(void);

/**
  * @brief  手动下一页
  * @param  none
  * @retval none
  */
void autoroll_next(void);

/**
  * @brief  手动上一页
  * @param  none
  * @retval none
  */
void autoroll_prev(void);

/**
  * @brief  跳转到指定页面
  * @param  page: 页面
  * @retval none
  */
void autoroll_set_page(autoroll_page_t page);

/**
  * @brief  获取当前页面
  * @param  none
  * @retval autoroll_page_t
  */
autoroll_page_t autoroll_get_page(void);

/**
  * @brief  请求重绘当前页面
  * @param  none
  * @retval none
  */
void autoroll_request_redraw(void);

#ifdef __cplusplus
}
#endif

#endif
