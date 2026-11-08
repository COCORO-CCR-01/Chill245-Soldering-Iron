/**
  **************************************************************************
  * @file     hms.c
  * @brief    HMS 硬件管理/故障屏幕
  **************************************************************************
  * 不可恢复错误一旦置位，HMS 持续占用屏幕，正常 UI 不再绘制。
  * 设置菜单可以临时调出 HMS 自检：无故障显示 ALL OK。
  **************************************************************************
  */

#include "hms.h"
#include "display.h"
#include "appcolors.h"
#include "ui_icons.h"
#include "statemachine.h"
#include "powermanagement.h"
#include "buzzer.h"
#include "buttons.h"
#include "fmt.h"
#include <stddef.h>

#define HMS_BLACK       appcolors_surface()
#define HMS_WHITE       appcolors_get(APP_COLOR_QUATERNARY)
#define HMS_RED         (0xF800U)
#define HMS_GREEN       appcolors_get(APP_COLOR_SECONDARY)
#define HMS_YELLOW      appcolors_get(APP_COLOR_PRIMARY)
#define HMS_GRAY        appcolors_get(APP_COLOR_TERTIARY)
#define HMS_DARK        appcolors_surface()
#define HMS_VIEW_MS     (3000U)

typedef enum
{
  HMS_VIEW_NONE = 0,
  HMS_VIEW_ALL_OK,
  HMS_VIEW_FAULT,
  HMS_VIEW_EEPROM
} hms_view_t;

static hms_code_t hms_code;
static uint8_t hms_fatal;
static uint8_t hms_drawn;
static hms_view_t hms_view;
static uint32_t hms_view_until_ms;
static uint8_t hms_menu_last;
static uint32_t hms_info_ms;

static void hms_draw_center(const char *text, uint16_t y, uint16_t color,
                            uint8_t scale)
{
  uint16_t width;

  if(text == NULL)
  {
    return;
  }

  width = display_string_width(text, scale);

  if(width >= DISPLAY_WIDTH)
  {
    display_draw_string(0U, y, text, color, HMS_BLACK, scale);
  }
  else
  {
    display_draw_string((uint16_t)((DISPLAY_WIDTH - width) / 2U),
                        y, text, color, HMS_BLACK, scale);
  }
}

static const char *hms_code_text(void)
{
  switch(hms_code)
  {
    case HMS_CODE_EEPROM_CRC:
      return "EEPROM CRC";

    case HMS_CODE_EEPROM_WRITE:
      return "EEPROM WRITE";

    case HMS_CODE_HEATER_WD:
      return "HEATER WD";

    default:
      return "SYSTEM";
  }
}

static void hms_draw_fatal(void)
{
  uint16_t code_y = 28U;

  display_clear(HMS_BLACK);

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 14U, HMS_RED);
  ui_icon_draw(UI_ICON_WARN, 3U, 3U, HMS_WHITE);
  display_draw_string(15U, 2U, "HMS", HMS_WHITE, HMS_RED, 2U);
  display_draw_string(132U, 3U, "HALT", HMS_WHITE, HMS_RED, 1U);

  hms_draw_center("SYSTEM HALTED", 18U, HMS_RED, 1U);

  if(display_string_width(hms_code_text(), 2U) <= (DISPLAY_WIDTH - 4U))
  {
    hms_draw_center(hms_code_text(), code_y, HMS_WHITE, 2U);
  }
  else
  {
    hms_draw_center(hms_code_text(), code_y + 2U, HMS_WHITE, 1U);
  }

  display_fill_rect(8U, 49U, (uint16_t)(DISPLAY_WIDTH - 16U), 1U, HMS_DARK);
  hms_draw_center("HEATER DISABLED", 54U, HMS_GRAY, 1U);
  hms_draw_center("POWER CYCLE TO RESET", 66U, HMS_GRAY, 1U);
}

static void hms_draw_all_ok(void)
{
  display_clear(HMS_BLACK);

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 14U, HMS_GREEN);
  ui_icon_draw(UI_ICON_CHECK, 3U, 3U, HMS_WHITE);
  display_draw_string(15U, 2U, "HMS", HMS_WHITE, HMS_GREEN, 2U);
  display_draw_string(132U, 3U, "SELF", HMS_WHITE, HMS_GREEN, 1U);

  hms_draw_center("ALL OK", 27U, HMS_GREEN, 3U);
  hms_draw_center("NO FAULTS", 52U, HMS_GRAY, 1U);
  hms_draw_center("AUTO RETURN", 65U, HMS_DARK, 1U);
}

static const char *hms_fault_text(verifyheater_fault_t fault)
{
  switch(fault)
  {
    case VERIFY_HEATER_FAULT_TC_OPEN:
      return "HOTEND SENSOR OPEN";
    case VERIFY_HEATER_FAULT_TC_SHORT:
      return "HOTEND SENSOR SHORT";
    case VERIFY_HEATER_FAULT_TEMP_INVALID:
      return "TEMP INVALID";
    case VERIFY_HEATER_FAULT_NO_RISE:
      return "TEMP NO RISE";
    case VERIFY_HEATER_FAULT_TEMP_JUMP:
      return "TEMP JUMP";
    case VERIFY_HEATER_FAULT_TEMP_DROP:
      return "TEMP DROP";
    case VERIFY_HEATER_FAULT_OVERTEMP:
      return "OVER TEMPERATURE";
    case VERIFY_HEATER_FAULT_FULL_POWER:
      return "FULL POWER";
    default:
      return "SYSTEM ERROR";
  }
}

static void hms_draw_fault_notice(void)
{
  sm_status_t status;
  power_fault_t power_fault;
  const char *fault_text;
  char line[16];
  uint16_t vin_mv;
  int cold_c;
  int tip_c;
  int set_c;

  sm_get_status(&status);
  power_fault = power_get_fault();
  vin_mv = power_get_voltage_mv();
  cold_c = (int)(status.room_c + 0.5f);
  tip_c = (int)(status.tip_c + 0.5f);
  set_c = (int)(status.setpoint_c + 0.5f);

  display_clear(HMS_BLACK);
  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 30U, HMS_YELLOW);

  /* 两行参数，全部 2 倍字体 */
  display_draw_string(2U, 2U, "ERR", HMS_BLACK, HMS_YELLOW, 2U);

  fmt_snprintf(line, sizeof(line), "VIN %uV",
               (unsigned)((vin_mv + 500U) / 1000U));
  display_draw_string(30U, 2U, line, HMS_BLACK, HMS_YELLOW, 2U);

  fmt_snprintf(line, sizeof(line), "COLD %dC", cold_c);
  display_draw_string(90U, 2U, line, HMS_BLACK, HMS_YELLOW, 2U);

  fmt_snprintf(line, sizeof(line), "NOW %dC", tip_c);
  display_draw_string(2U, 16U, line, HMS_BLACK, HMS_YELLOW, 2U);

  fmt_snprintf(line, sizeof(line), "SET %dC", set_c);
  display_draw_string(90U, 16U, line, HMS_BLACK, HMS_YELLOW, 2U);

  if(power_fault == POWER_FAULT_UNDERVOLTAGE)
  {
    fault_text = "INPUT UNDERVOLTAGE";
  }
  else if(power_fault == POWER_FAULT_OVERVOLTAGE)
  {
    fault_text = "INPUT OVERVOLTAGE";
  }
  else
  {
    fault_text = hms_fault_text(status.fault);
  }

  hms_draw_center(fault_text, 38U, HMS_YELLOW, 2U);
  hms_draw_center("PRESS MENU TO RETRY", 66U, HMS_GRAY, 2U);
}

static void hms_draw_eeprom_recovered(void)
{
  display_clear(HMS_BLACK);

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 14U, HMS_YELLOW);
  ui_icon_draw(UI_ICON_CHECK, 3U, 3U, HMS_BLACK);
  display_draw_string(15U, 2U, "HMS", HMS_BLACK, HMS_YELLOW, 2U);
  display_draw_string(128U, 3U, "EEPROM", HMS_BLACK, HMS_YELLOW, 1U);

  hms_draw_center("EEPROM", 28U, HMS_WHITE, 2U);
  hms_draw_center("RECOVERED", 46U, HMS_GREEN, 2U);
  hms_draw_center("DEFAULTS SAVED", 64U, HMS_GRAY, 1U);
}

void hms_init(void)
{
  hms_code = HMS_CODE_NONE;
  hms_fatal = 0U;
  hms_drawn = 0U;
  hms_view = HMS_VIEW_NONE;
  hms_view_until_ms = 0U;
}

void hms_set_fatal(hms_code_t code)
{
  if((code == HMS_CODE_NONE) || (hms_fatal != 0U))
  {
    return;
  }

  hms_fatal = 1U;
  hms_code = code;
  hms_view = HMS_VIEW_NONE;
  hms_drawn = 0U;

  /* 不可恢复错误：先确保加热关闭 */
  (void)sm_set_heater_enabled(0U);

  buzzer_play(BUZZER_SOUND_FAULT);
}

uint8_t hms_has_fatal(void)
{
  return hms_fatal;
}

uint8_t hms_is_active(void)
{
  return ((hms_fatal != 0U) || (hms_view != HMS_VIEW_NONE)) ? 1U : 0U;
}

hms_code_t hms_get_code(void)
{
  return hms_code;
}

void hms_show_all_ok(void)
{
  sm_status_t status;

  if(hms_fatal != 0U)
  {
    return;
  }

  sm_get_status(&status);

  if((status.fault != VERIFY_HEATER_FAULT_NONE) ||
     (status.state == SM_STATE_FAULT) ||
     (power_get_fault() != POWER_FAULT_NONE))
  {
    hms_view = HMS_VIEW_FAULT;
  }
  else
  {
    hms_view = HMS_VIEW_ALL_OK;
  }

  hms_view_until_ms = sm_get_uptime_ms() + HMS_VIEW_MS;
  hms_drawn = 0U;
}

void hms_show_eeprom_recovered(void)
{
  if(hms_fatal != 0U)
  {
    return;
  }

  hms_view = HMS_VIEW_EEPROM;
  hms_view_until_ms = sm_get_uptime_ms() + HMS_VIEW_MS;
  hms_drawn = 0U;
}

uint8_t hms_task(uint32_t now_ms)
{
  sm_status_t status;

  if(hms_fatal != 0U)
  {
    if(hms_drawn == 0U)
    {
      hms_draw_fatal();
      hms_drawn = 1U;
    }

    return 1U;
  }

  sm_get_status(&status);

  /* 任意保护故障常驻全屏 HMS，优先于 maindash、菜单和开机联锁 */
  if((status.fault != VERIFY_HEATER_FAULT_NONE) ||
     (status.state == SM_STATE_FAULT) ||
     (power_get_fault() != POWER_FAULT_NONE))
  {
    uint8_t menu_pressed;

    if(hms_view != HMS_VIEW_FAULT)
    {
      hms_view = HMS_VIEW_FAULT;
      hms_drawn = 0U;
      hms_menu_last = buttons_is_pressed(BUTTON_MENU);
    }

    if((hms_drawn == 0U) ||
       ((uint32_t)(now_ms - hms_info_ms) >= 1000U))
    {
      hms_draw_fault_notice();
      hms_drawn = 1U;
      hms_info_ms = (now_ms == 0U) ? 1U : now_ms;
    }

    menu_pressed = buttons_is_pressed(BUTTON_MENU);
    if((menu_pressed != 0U) && (hms_menu_last == 0U))
    {
      (void)sm_clear_fault();
      (void)power_clear_fault();
      (void)sm_set_heater_enabled(1U);
      hms_view = HMS_VIEW_NONE;
      hms_drawn = 0U;
      hms_menu_last = 1U;
      return 0U;
    }
    hms_menu_last = menu_pressed;
    return 1U;
  }

  if(hms_view == HMS_VIEW_FAULT)
  {
    hms_view = HMS_VIEW_NONE;
    hms_drawn = 0U;
  }

  if(hms_view == HMS_VIEW_NONE)
  {
    return 0U;
  }

  if((int32_t)(now_ms - hms_view_until_ms) >= 0)
  {
    hms_view = HMS_VIEW_NONE;
    hms_drawn = 0U;
    return 0U;
  }

  if(hms_drawn == 0U)
  {
    if(hms_view == HMS_VIEW_ALL_OK)
    {
      hms_draw_all_ok();
    }
    else if(hms_view == HMS_VIEW_EEPROM)
    {
      hms_draw_eeprom_recovered();
    }
    else
    {
      hms_draw_fault_notice();
    }

    hms_drawn = 1U;
  }

  return 1U;
}

void hms_force_redraw(void)
{
  if((hms_fatal != 0U) || (hms_view != HMS_VIEW_NONE))
  {
    hms_drawn = 0U;
  }
}
