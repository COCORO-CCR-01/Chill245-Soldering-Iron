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
#include "menu.h"
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
static uint8_t hms_menu_long_done;
/* PREV / NEXT 的上一次状态：抬起的时候触发"重试" */
static uint8_t hms_pair_last;
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

  /* 温度本身不可信（没插烙铁 / 热偶开路）的时候，这里不要印一个假数字。主界面这种
     情况画 "---"，故障界面跟着一起画，用户看一眼就知道是"没插"，而不是"读到 X 度"。 */
  if(status.temp_valid != 0U)
  {
    fmt_snprintf(line, sizeof(line), "NOW %dC", tip_c);
  }
  else
  {
    fmt_snprintf(line, sizeof(line), "NOW ---");
  }

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

  hms_draw_center(fault_text, 36U, HMS_YELLOW, 2U);

  /* 重试用 PREV / NEXT，MENU 专门留给"进设置"—— 主界面长按 MENU 也是进设置，
     同一个键在哪个界面都是这个意思。以前重试是短按 MENU，用户想长按进设置时手稍微
     松快一点就被当成短按重试掉，两个动作挤在一个键上。
     （写 PREV/NEXT 而不是 -/+：这块点阵字库里没有 '+' 字形，只有 % - . / : 和
     数字字母，印 + 会是个空格。） */
  hms_draw_center("PREV/NEXT TO RETRY", 58U, HMS_GRAY, 1U);
  hms_draw_center("HOLD MENU SETUP", 70U, HMS_GRAY, 1U);
}

/* 当前该显示哪一句故障文字：电源欠压 / 过压优先，其余按 verifyheater 的故障码。
 * 返回的都是静态字符串，可以直接比指针。 */
static const char *hms_fault_text_now(void)
{
  sm_status_t status;
  power_fault_t power_fault = power_get_fault();

  if(power_fault == POWER_FAULT_UNDERVOLTAGE)
  {
    return "INPUT UNDERVOLTAGE";
  }

  if(power_fault == POWER_FAULT_OVERVOLTAGE)
  {
    return "INPUT OVERVOLTAGE";
  }

  sm_get_status(&status);

  return hms_fault_text(status.fault);
}

/* 屏上此刻画的是哪句故障文字，用来判断故障类型有没有变（变了才需要整屏重画） */
static const char *hms_shown_fault_text;

/* 只刷黄色横幅里那四个会变的数值：VIN / COLD / NOW / SET。每个字段先把自己的槽位
 * 铺回黄色再画新值，槽位边界按同一行相邻字段的位置划好，不会互相吃字。
 *
 * 故障界面每秒要刷新一次数值，原来每次都走 hms_draw_fault_notice()：display_clear
 * 整屏 160x80（25600 字节）+ 30 像素高的横幅 + 故障文字 + 两行操作提示全部重画，
 * 一秒一次"整屏黑掉再画回来"，就是用户说的 HMS 全屏闪烁。拆开后每秒只动这
 * 四个槽位，其余一个像素不碰。 */
static void hms_draw_fault_values(void)
{
  sm_status_t status;
  uint16_t vin_mv;
  char line[16];

  sm_get_status(&status);
  vin_mv = power_get_voltage_mv();

  fmt_snprintf(line, sizeof(line), "VIN %uV",
               (unsigned)((vin_mv + 500U) / 1000U));
  display_fill_rect(30U, 2U, 58U, 10U, HMS_YELLOW);
  display_draw_string(30U, 2U, line, HMS_BLACK, HMS_YELLOW, 2U);

  fmt_snprintf(line, sizeof(line), "COLD %dC", (int)(status.room_c + 0.5f));
  display_fill_rect(90U, 2U, 70U, 10U, HMS_YELLOW);
  display_draw_string(90U, 2U, line, HMS_BLACK, HMS_YELLOW, 2U);

  /* 温度不可信（没插烙铁 / 热偶开路）时画 "NOW ---"，不印假数字，和主界面一致 */
  if(status.temp_valid != 0U)
  {
    fmt_snprintf(line, sizeof(line), "NOW %dC", (int)(status.tip_c + 0.5f));
  }
  else
  {
    fmt_snprintf(line, sizeof(line), "NOW ---");
  }

  display_fill_rect(2U, 16U, 86U, 10U, HMS_YELLOW);
  display_draw_string(2U, 16U, line, HMS_BLACK, HMS_YELLOW, 2U);

  fmt_snprintf(line, sizeof(line), "SET %dC", (int)(status.setpoint_c + 0.5f));
  display_fill_rect(90U, 16U, 70U, 10U, HMS_YELLOW);
  display_draw_string(90U, 16U, line, HMS_BLACK, HMS_YELLOW, 2U);
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
  hms_menu_long_done = 0U;
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
    uint8_t pair_pressed;

    if(hms_view != HMS_VIEW_FAULT)
    {
      hms_view = HMS_VIEW_FAULT;
      hms_drawn = 0U;
      hms_menu_last = buttons_is_pressed(BUTTON_MENU);
      hms_menu_long_done = 0U;
      /* 刚进故障界面时如果手里正按着 PREV/NEXT，先记成"已经按下"，
       * 免得画面一出来就直接被当成一次重试。 */
      hms_pair_last = ((buttons_is_pressed(BUTTON_PREV) != 0U) ||
                       (buttons_is_pressed(BUTTON_NEXT) != 0U)) ? 1U : 0U;
    }

    if(hms_drawn == 0U)
    {
      /* 第一次进这个界面：整屏画一遍（黄色横幅 + ERR + 故障文字 + 两行提示 + 数值） */
      hms_draw_fault_notice();
      hms_drawn = 1U;
      hms_shown_fault_text = hms_fault_text_now();
      hms_info_ms = (now_ms == 0U) ? 1U : now_ms;
    }
    else if((uint32_t)(now_ms - hms_info_ms) >= 1000U)
    {
      hms_info_ms = (now_ms == 0U) ? 1U : now_ms;

      if(hms_fault_text_now() != hms_shown_fault_text)
      {
        /* 故障类型变了，中间那句故障文字得换，整屏重画一次 */
        hms_draw_fault_notice();
        hms_shown_fault_text = hms_fault_text_now();
      }
      else
      {
        /* 每秒的例行刷新只更新横幅里那四个数值，不再整屏刷黑再全部重画 */
        hms_draw_fault_values();
      }
    }

    menu_pressed = buttons_is_pressed(BUTTON_MENU);

    /* 长按 MENU：进设置（受限电源设置页）。所有故障都认这个手势，跟主界面
     * "长按 MENU 进设置"一致；重试已经交给 PREV / NEXT，不再跟这个键抢。 */
    if((menu_pressed != 0U) &&
       (buttons_get_press_duration(BUTTON_MENU, now_ms) >= 2000U) &&
       (hms_menu_long_done == 0U))
    {
      hms_menu_long_done = 1U;
      menu_open_service_setup();
      return 1U;
    }

    /* MENU 抬起只清长按标记，让下一次长按重新计时；MENU 本身不再触发重试。 */
    if((menu_pressed == 0U) && (hms_menu_last != 0U))
    {
      hms_menu_long_done = 0U;
    }

    /* PREV / NEXT 在抬起时重试。跟原来 MENU 一样用抬起而不是按下：一是行为一致，
     * 二是故障刚弹出来时用户手里要正好按着键，不会立刻被重试掉。 */
    pair_pressed = ((buttons_is_pressed(BUTTON_PREV) != 0U) ||
                    (buttons_is_pressed(BUTTON_NEXT) != 0U)) ? 1U : 0U;

    if((pair_pressed == 0U) && (hms_pair_last != 0U))
    {
      (void)sm_clear_fault();
      (void)power_clear_fault();
      (void)sm_set_heater_enabled(1U);
      hms_view = HMS_VIEW_NONE;
      hms_drawn = 0U;
      hms_pair_last = 0U;
      return 0U;
    }

    hms_pair_last = pair_pressed;

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
