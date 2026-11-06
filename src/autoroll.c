/**
  **************************************************************************
  * @file     autoroll.c
  * @brief    主界面/图表自动轮换
  **************************************************************************
  * 页面：
  *   0 主界面
  *   1 TEMP/PWR 曲线
  *   2 VOLTAGE 曲线
  *   3 COLD 曲线
  *
  * 默认 5 秒轮换；曲线每 250 ms 采样、页面约 500 ms 重绘。
  **************************************************************************
  */

#include "autoroll.h"
#include "maindash.h"
#include "graph.h"
#include "display.h"
#include "appcolors.h"
#include "statemachine.h"
#include "menu.h"
#include "onekeyboost.h"
#include "powermanagement.h"
#include "eeprom.h"

#define AUTOROLL_DEFAULT_MS             (5000U)
#define AUTOROLL_MIN_MS                 (1000U)
#define AUTOROLL_MAX_MS                 (60000U)
#define AUTOROLL_GRAPH_REDRAW_MS        (250U)

#define UI_BLACK                        appcolors_surface()
#define UI_CYAN                         appcolors_get(APP_COLOR_PRIMARY)
#define UI_DARK                         appcolors_get(APP_COLOR_QUINARY)

static autoroll_page_t autoroll_page;
static uint32_t autoroll_interval_ms;
static uint32_t autoroll_last_roll_ms;
static uint32_t autoroll_last_draw_ms;
static uint8_t autoroll_enabled;
static uint8_t autoroll_dirty;

static void autoroll_draw_indicator(void)
{
  uint8_t i;
  uint16_t x;
  uint16_t color;

  /* 新版全屏主界面底部整条是温度进度条，不叠加页面指示 */
  if((autoroll_page == AUTOROLL_PAGE_MAIN) &&
     (g_eeprom_params.maindash_alt != 0U))
  {
    return;
  }

  for(i = 0U; i < (uint8_t)AUTOROLL_PAGE_COUNT; i++)
  {
    x = (uint16_t)(30U + (i * 26U));
    color = ((uint8_t)autoroll_page == i) ? UI_CYAN : UI_DARK;
    display_fill_rect(x, 77U, 22U, 2U, color);
  }
}

static void autoroll_draw_page(void)
{
  sm_status_t status;

  switch(autoroll_page)
  {
    case AUTOROLL_PAGE_TEMP_POWER:
      graph_draw_temp_power();
      break;

    case AUTOROLL_PAGE_VOLTAGE:
      graph_draw_voltage();
      break;

    case AUTOROLL_PAGE_COLD:
      graph_draw_cold();
      break;

    case AUTOROLL_PAGE_MAIN:
    default:
      graph_invalidate();
      sm_get_status(&status);
      maindash_draw(&status);
      break;
  }

  autoroll_draw_indicator();
}

void autoroll_init(uint32_t interval_ms)
{
  autoroll_page = AUTOROLL_PAGE_MAIN;
  autoroll_interval_ms = (interval_ms < AUTOROLL_MIN_MS) ? AUTOROLL_MIN_MS : interval_ms;

  if(autoroll_interval_ms > AUTOROLL_MAX_MS)
  {
    autoroll_interval_ms = AUTOROLL_MAX_MS;
  }

  autoroll_last_roll_ms = sm_get_uptime_ms();
  autoroll_last_draw_ms = 0U;
  autoroll_enabled = 1U;
  autoroll_dirty = 1U;
  graph_invalidate();

  /* 翻页按键统一由 main_buttons() 仲裁，autoroll 不再注册按键回调 */
}

void autoroll_update(uint32_t now_ms)
{
  uint8_t new_sample;

  new_sample = graph_task(now_ms);

  /* 已删除定时自动翻页：只保留 NEXT/PREV 短按手动切换 */

  if(autoroll_dirty != 0U)
  {
    autoroll_draw_page();
    autoroll_dirty = 0U;
    autoroll_last_draw_ms = now_ms;
    return;
  }

  if(autoroll_page == AUTOROLL_PAGE_MAIN)
  {
    if(maindash_update(now_ms) != 0U)
    {
      autoroll_draw_indicator();
    }

    return;
  }

  if((new_sample != 0U) &&
     ((uint32_t)(now_ms - autoroll_last_draw_ms) >= AUTOROLL_GRAPH_REDRAW_MS))
  {
    autoroll_draw_page();
    autoroll_last_draw_ms = now_ms;
  }
}

void autoroll_set_interval(uint32_t interval_ms)
{
  if(interval_ms < AUTOROLL_MIN_MS)
  {
    interval_ms = AUTOROLL_MIN_MS;
  }

  if(interval_ms > AUTOROLL_MAX_MS)
  {
    interval_ms = AUTOROLL_MAX_MS;
  }

  autoroll_interval_ms = interval_ms;
  autoroll_last_roll_ms = sm_get_uptime_ms();
}

uint32_t autoroll_get_interval(void)
{
  return autoroll_interval_ms;
}

void autoroll_enable(uint8_t enable)
{
  autoroll_enabled = (enable != 0U) ? 1U : 0U;
  autoroll_last_roll_ms = sm_get_uptime_ms();
}

uint8_t autoroll_is_enabled(void)
{
  return autoroll_enabled;
}

void autoroll_next(void)
{
  autoroll_page = (autoroll_page_t)(((uint8_t)autoroll_page + 1U) %
                                    (uint8_t)AUTOROLL_PAGE_COUNT);
  autoroll_last_roll_ms = sm_get_uptime_ms();
  graph_invalidate();
  autoroll_dirty = 1U;
}

void autoroll_prev(void)
{
  if((uint8_t)autoroll_page == 0U)
  {
    autoroll_page = (autoroll_page_t)((uint8_t)AUTOROLL_PAGE_COUNT - 1U);
  }
  else
  {
    autoroll_page = (autoroll_page_t)((uint8_t)autoroll_page - 1U);
  }

  autoroll_last_roll_ms = sm_get_uptime_ms();
  graph_invalidate();
  autoroll_dirty = 1U;
}

void autoroll_set_page(autoroll_page_t page)
{
  if(page < AUTOROLL_PAGE_COUNT)
  {
    autoroll_page = page;
    autoroll_last_roll_ms = sm_get_uptime_ms();
    graph_invalidate();
    autoroll_dirty = 1U;
  }
}

autoroll_page_t autoroll_get_page(void)
{
  return autoroll_page;
}

void autoroll_request_redraw(void)
{
  graph_invalidate();
  autoroll_dirty = 1U;
}

void autoroll_on_button(button_id_t button, button_event_t event)
{
  if(menu_is_open() != 0U)
  {
    return;
  }

  if((event != BUTTON_EVENT_CLICK) &&
     (event != BUTTON_EVENT_LONG_PRESS) &&
     (event != BUTTON_EVENT_REPEAT))
  {
    return;
  }

  /* 休眠时 NEXT 的这次 CLICK 只用于唤醒，不再翻页 */
  if((button == BUTTON_NEXT) &&
     (event == BUTTON_EVENT_CLICK) &&
     (power_take_wake_click() != 0U))
  {
    return;
  }

  /* NEXT 长按固定用于 OneKeyBoost，不再翻页 */
  if((button == BUTTON_NEXT) && (event == BUTTON_EVENT_LONG_PRESS))
  {
    return;
  }

  /* Boost 期间 NEXT 的保持/连发不再翻页 */
  if((button == BUTTON_NEXT) &&
     (onekeyboost_is_active() != 0U) &&
     ((event == BUTTON_EVENT_LONG_PRESS) ||
      (event == BUTTON_EVENT_REPEAT)))
  {
    return;
  }

  /* Boost 期间 PREV 只用于退出 Boost，不翻页 */
  if((button == BUTTON_PREV) &&
     (onekeyboost_is_active() != 0U))
  {
    return;
  }

  switch(button)
  {
    case BUTTON_PREV:
      autoroll_prev();
      break;

    case BUTTON_NEXT:
      autoroll_next();
      break;

    default:
      break;
  }
}
