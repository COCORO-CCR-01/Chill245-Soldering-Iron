/**
  **************************************************************************
  * @file     maindash.c
  * @brief    160x80 横屏主界面
  **************************************************************************
  * 布局：
  *   y=1    标题 JBC245 + 短状态
  *   y=16   热端温度大号七段数码（3 位 + °C）
  *   y=50   功率百分比（2 位 + %）
  *   y=76   VIN 电压
  *   y=92   COLD 冷端
  *   y=108  SET 目标温度
  *   y=124  MODE 曲线
  *   y=142  工作状态短信息
  *   y=156  页面指示（autoroll 绘制）
  **************************************************************************
  */

#include "maindash.h"
#include "display.h"
#include "powermanagement.h"
#include "onekeyboost.h"
#include "ui_icons.h"
#include <stdio.h>

#define UI_BLACK                        (0x0000U)
#define UI_WHITE                        (0xFFFFU)
#define UI_GRAY                         (0x8410U)
#define UI_DARK                         (0x4208U)
#define UI_ORANGE                       (0xFD20U)
#define UI_GREEN                        (0x07E0U)
#define UI_RED                          (0xF800U)
#define UI_CYAN                         (0x07FFU)
#define UI_YELLOW                       (0xFFE0U)

#define UI_INFO_PERIOD_MS               (200U)

static uint32_t maindash_last_draw_ms;

static int maindash_round_float(float value)
{
  if(value >= 0.0f)
  {
    return (int)(value + 0.5f);
  }

  return (int)(value - 0.5f);
}

/**
  * @brief  七段数码管数字
  * @param  x/y/w/h: 数字外框
  * @param  thickness: 笔画宽度
  * @param  digit: 0~9
  */
static void maindash_draw_seg_digit(uint16_t x, uint16_t y,
                                    uint16_t w, uint16_t h,
                                    uint8_t thickness, uint8_t digit,
                                    uint16_t color)
{
  static const uint8_t patterns[10] =
  {
    0x3FU, 0x06U, 0x5BU, 0x4FU, 0x66U,
    0x6DU, 0x7DU, 0x07U, 0x7FU, 0x6FU
  };

  uint8_t mask;
  uint16_t mid;
  uint16_t upper_bottom;
  uint16_t lower_top;
  uint16_t upper_h;
  uint16_t lower_h;

  digit = (uint8_t)(digit % 10U);
  mask = patterns[digit];

  display_fill_rect(x, y, w, h, UI_BLACK);

  mid = (uint16_t)(y + (h / 2U));
  upper_bottom = (uint16_t)(mid - (thickness / 2U) - 1U);
  lower_top = (uint16_t)(mid + (thickness / 2U) + 1U);
  upper_h = (uint16_t)(upper_bottom - (y + thickness));
  lower_h = (uint16_t)((y + h - thickness) - lower_top);

  /* A 上横 */
  if((mask & 0x01U) != 0U)
  {
    display_fill_rect((uint16_t)(x + thickness), y,
                      (uint16_t)(w - (2U * thickness)), thickness, color);
  }

  /* B 右上竖 */
  if((mask & 0x02U) != 0U)
  {
    display_fill_rect((uint16_t)(x + w - thickness), (uint16_t)(y + thickness),
                      thickness, upper_h, color);
  }

  /* C 右下竖 */
  if((mask & 0x04U) != 0U)
  {
    display_fill_rect((uint16_t)(x + w - thickness), lower_top,
                      thickness, lower_h, color);
  }

  /* D 下横 */
  if((mask & 0x08U) != 0U)
  {
    display_fill_rect((uint16_t)(x + thickness), (uint16_t)(y + h - thickness),
                      (uint16_t)(w - (2U * thickness)), thickness, color);
  }

  /* E 左下竖 */
  if((mask & 0x10U) != 0U)
  {
    display_fill_rect(x, lower_top, thickness, lower_h, color);
  }

  /* F 左上竖 */
  if((mask & 0x20U) != 0U)
  {
    display_fill_rect(x, (uint16_t)(y + thickness), thickness, upper_h, color);
  }

  /* G 中横 */
  if((mask & 0x40U) != 0U)
  {
    display_fill_rect((uint16_t)(x + thickness),
                      (uint16_t)(mid - (thickness / 2U)),
                      (uint16_t)(w - (2U * thickness)), thickness, color);
  }
}

static void maindash_draw_temperature(const sm_status_t *status)
{
  int tip_c;
  uint8_t hundreds;
  uint8_t tens;
  uint8_t units;

  if(status->temp_valid == 0U)
  {
    display_fill_rect(2U, 14U, 64U, 26U, UI_BLACK);
    display_draw_string(8U, 19U, "---", UI_ORANGE, UI_BLACK, 3U);
    return;
  }

  tip_c = maindash_round_float(status->tip_c);

  if(tip_c < 0)
  {
    tip_c = 0;
  }

  if(tip_c > 999)
  {
    tip_c = 999;
  }

  hundreds = (uint8_t)(tip_c / 100);
  tens = (uint8_t)((tip_c / 10) % 10);
  units = (uint8_t)(tip_c % 10);

  if(hundreds != 0U)
  {
    maindash_draw_seg_digit(4U, 14U, 14U, 24U, 3U, hundreds, UI_ORANGE);
  }
  else
  {
    display_fill_rect(4U, 14U, 14U, 24U, UI_BLACK);
  }

  if((hundreds != 0U) || (tens != 0U))
  {
    maindash_draw_seg_digit(19U, 14U, 14U, 24U, 3U, tens, UI_ORANGE);
  }
  else
  {
    display_fill_rect(19U, 14U, 14U, 24U, UI_BLACK);
  }

  maindash_draw_seg_digit(34U, 14U, 14U, 24U, 3U, units, UI_ORANGE);

  /* °C */
  display_draw_string(50U, 19U, "\x7F" "C", UI_ORANGE, UI_BLACK, 2U);
}

static void maindash_draw_power(const sm_status_t *status)
{
  uint8_t power = status->power;
  uint8_t tens;
  uint8_t units;

  if(power > 100U)
  {
    power = 100U;
  }

  if(power >= 100U)
  {
    display_fill_rect(2U, 44U, 24U, 16U, UI_BLACK);
    display_draw_string(2U, 47U, "100", UI_GREEN, UI_BLACK, 2U);
  }
  else
  {
    tens = (uint8_t)(power / 10U);
    units = (uint8_t)(power % 10U);

    if(tens != 0U)
    {
      maindash_draw_seg_digit(4U, 44U, 10U, 16U, 2U, tens, UI_GREEN);
    }
    else
    {
      display_fill_rect(4U, 44U, 10U, 16U, UI_BLACK);
    }

    maindash_draw_seg_digit(15U, 44U, 10U, 16U, 2U, units, UI_GREEN);
  }

  display_draw_string(27U, 47U, "%", UI_GREEN, UI_BLACK, 2U);
  ui_icon_draw(UI_ICON_BOLT, 44U, 46U, UI_GREEN);
  display_draw_string(38U, 49U, "PWR", UI_GRAY, UI_BLACK, 1U);
}

static void maindash_draw_info_row(uint16_t x, uint16_t y,
                                   ui_icon_t icon, const char *label,
                                   const char *value, uint16_t value_color)
{
  ui_icon_draw(icon, x, y, UI_GRAY);
  display_draw_string((uint16_t)(x + 10U), y, label, UI_GRAY, UI_BLACK, 1U);
  display_fill_rect((uint16_t)(x + 36U), y, 40U, 10U, UI_BLACK);
  display_draw_string((uint16_t)(x + 36U), y, value,
                      value_color, UI_BLACK, 1U);
}

static const char *maindash_top_state(const sm_status_t *status, uint16_t *color)
{
  if(power_get_fault() != POWER_FAULT_NONE)
  {
    *color = UI_RED;
    return "FLT";
  }

  if(status->fault != VERIFY_HEATER_FAULT_NONE)
  {
    *color = UI_RED;
    return "FLT";
  }

  if(onekeyboost_is_active() != 0U)
  {
    *color = UI_ORANGE;
    return "BST";
  }

  if(status->temp_valid == 0U)
  {
    *color = UI_GRAY;
    return "INI";
  }

  switch(status->state)
  {
    case SM_STATE_AUTOTUNE:
      *color = UI_YELLOW;
      return "TUN";

    case SM_STATE_DISABLED:
      *color = UI_GRAY;
      return "OFF";

    case SM_STATE_FAULT:
      *color = UI_RED;
      return "FLT";

    case SM_STATE_HEATING:
      *color = UI_ORANGE;
      return "RUN";

    case SM_STATE_INIT:
    default:
      *color = UI_GRAY;
      return "INI";
  }
}

static ui_icon_t maindash_top_icon(const sm_status_t *status)
{
  if((power_get_fault() != POWER_FAULT_NONE) ||
     (status->fault != VERIFY_HEATER_FAULT_NONE) ||
     (status->state == SM_STATE_FAULT))
  {
    return UI_ICON_WARN;
  }

  if(status->temp_valid == 0U)
  {
    return UI_ICON_CLOCK;
  }

  switch(status->state)
  {
    case SM_STATE_AUTOTUNE:
      return UI_ICON_GEAR;

    case SM_STATE_DISABLED:
      return UI_ICON_POWER;

    case SM_STATE_HEATING:
      return UI_ICON_FLAME;

    case SM_STATE_INIT:
    default:
      return UI_ICON_CLOCK;
  }
}

static const char *maindash_fault_text(verifyheater_fault_t fault)
{
  switch(fault)
  {
    case VERIFY_HEATER_FAULT_TC_OPEN:
      return "TC OPEN";

    case VERIFY_HEATER_FAULT_TC_SHORT:
      return "TC SHORT";

    case VERIFY_HEATER_FAULT_TEMP_INVALID:
      return "TEMP INV";

    case VERIFY_HEATER_FAULT_NO_RISE:
      return "NO RISE";

    case VERIFY_HEATER_FAULT_TEMP_JUMP:
      return "TEMP JUMP";

    case VERIFY_HEATER_FAULT_TEMP_DROP:
      return "TEMP DROP";

    case VERIFY_HEATER_FAULT_OVERTEMP:
      return "OVER TEMP";

    case VERIFY_HEATER_FAULT_FULL_POWER:
      return "FULL PWR";

    default:
      return "FAULT";
  }
}

static const char *maindash_status_text(const sm_status_t *status, uint16_t *color)
{
  float diff;
  power_fault_t power_fault = power_get_fault();

  if(power_fault != POWER_FAULT_NONE)
  {
    *color = UI_RED;
    return (power_fault == POWER_FAULT_UNDERVOLTAGE) ? "V LOW" : "V HIGH";
  }

  if(status->fault != VERIFY_HEATER_FAULT_NONE)
  {
    *color = UI_RED;
    return maindash_fault_text(status->fault);
  }

  if(onekeyboost_is_active() != 0U)
  {
    *color = UI_ORANGE;
    return "BOOST";
  }

  if(status->temp_valid == 0U)
  {
    *color = UI_GRAY;
    return "WAIT";
  }

  switch(status->state)
  {
    case SM_STATE_AUTOTUNE:
      *color = UI_YELLOW;
      return "TUNING";

    case SM_STATE_DISABLED:
      *color = UI_GRAY;
      return "DISABLED";

    case SM_STATE_FAULT:
      *color = UI_RED;
      return "FAULT";

    case SM_STATE_HEATING:
      diff = status->tip_c - status->setpoint_c;

      if(diff < 0.0f)
      {
        diff = -diff;
      }

      if(diff <= 5.0f)
      {
        *color = UI_GREEN;
        return "READY";
      }

      *color = UI_ORANGE;
      return "HEATING";

    case SM_STATE_INIT:
    default:
      *color = UI_GRAY;
      return "INIT";
  }
}

static const char *maindash_mode_text(pid_mode_t mode)
{
  switch(mode)
  {
    case PID_MODE_BOOST:
      return "BST";

    case PID_MODE_CUSTOM:
      return "CUS";

    case PID_MODE_STANDARD:
    default:
      return "STD";
  }
}

void maindash_init(void)
{
  maindash_last_draw_ms = 0U;
}

#define MAINDASH_DRAW_STEPS (6U)

static uint8_t maindash_step_active;
static uint8_t maindash_step_index;
static uint8_t maindash_cache_valid;
static uint8_t maindash_force_draw;

static const char *maindash_last_top_state;
static uint16_t maindash_last_top_color;
static const char *maindash_last_status_text;
static uint16_t maindash_last_status_color;
static const char *maindash_last_mode_text;
static int maindash_last_tip;
static int maindash_last_power;
static int maindash_last_vin_mv;
static int maindash_last_cold_x10;
static int maindash_last_setpoint;

static void maindash_draw_step(const sm_status_t *status, uint8_t step)
{
  const char *top_state;
  const char *status_text;
  uint16_t top_color;
  uint16_t status_color;
  char value[16];

  if(status == NULL)
  {
    return;
  }

  switch(step)
  {
    case 0U:
      top_state = maindash_top_state(status, &top_color);
      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_top_state == top_state) &&
         (maindash_last_top_color == top_color))
      {
        break;
      }

      display_fill_rect(0U, 0U, DISPLAY_WIDTH, 12U, UI_BLACK);
      display_draw_string(2U, 1U, "JBC245", UI_WHITE, UI_BLACK, 2U);
      ui_icon_draw(maindash_top_icon(status), 124U, 2U, top_color);
      display_draw_string(134U, 1U, top_state, top_color, UI_BLACK, 2U);

      display_fill_rect(0U, 12U, DISPLAY_WIDTH, 1U, UI_DARK);
      display_fill_rect(78U, 14U, 1U, 46U, UI_DARK);

      maindash_last_top_state = top_state;
      maindash_last_top_color = top_color;
      break;

    case 1U:
    {
      int tip_c = -1;

      if(status->temp_valid != 0U)
      {
        tip_c = maindash_round_float(status->tip_c);
        if(tip_c < 0)
        {
          tip_c = 0;
        }
        if(tip_c > 999)
        {
          tip_c = 999;
        }
      }

      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_tip == tip_c))
      {
        break;
      }

      maindash_draw_temperature(status);
      maindash_last_tip = tip_c;
      break;
    }

    case 2U:
    {
      int power = status->power;

      if(power > 100)
      {
        power = 100;
      }

      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_power == power))
      {
        break;
      }

      maindash_draw_power(status);
      maindash_last_power = power;
      break;
    }

    case 3U:
    {
      int vin_mv = (int)power_get_voltage_mv();
      int vin_key = vin_mv / 100;
      int cold_x10_new = maindash_round_float(status->room_c * 10.0f);

      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_vin_mv == vin_key) &&
         (maindash_last_cold_x10 == cold_x10_new))
      {
        break;
      }

      snprintf(value, sizeof(value), "%d.%dV",
               vin_mv / 1000, (vin_mv % 1000) / 100);
      maindash_draw_info_row(84U, 14U, UI_ICON_PLUG, "VIN", value,
                             (power_get_fault() != POWER_FAULT_NONE) ?
                             UI_RED : UI_CYAN);

      if(cold_x10_new < 0)
      {
        snprintf(value, sizeof(value), "-%d.%dC",
                 (-cold_x10_new) / 10, (-cold_x10_new) % 10);
      }
      else
      {
        snprintf(value, sizeof(value), "%d.%dC",
                 cold_x10_new / 10, cold_x10_new % 10);
      }
      maindash_draw_info_row(84U, 26U, UI_ICON_SNOW, "COLD", value, UI_YELLOW);

      maindash_last_vin_mv = vin_key;
      maindash_last_cold_x10 = cold_x10_new;
      break;
    }

    case 4U:
    {
      int setpoint_new = maindash_round_float(status->setpoint_c);
      const char *mode_new = maindash_mode_text(status->mode);

      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_setpoint == setpoint_new) &&
         (maindash_last_mode_text == mode_new))
      {
        break;
      }

      snprintf(value, sizeof(value), "%dC", setpoint_new);
      maindash_draw_info_row(84U, 38U, UI_ICON_TARGET, "SET", value, UI_WHITE);
      maindash_draw_info_row(84U, 50U, UI_ICON_MODE, "MODE", mode_new, UI_WHITE);

      maindash_last_setpoint = setpoint_new;
      maindash_last_mode_text = mode_new;
      break;
    }

    case 5U:
    default:
      status_text = maindash_status_text(status, &status_color);
      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_status_text == status_text) &&
         (maindash_last_status_color == status_color))
      {
        break;
      }

      display_fill_rect(0U, 60U, DISPLAY_WIDTH, 12U, UI_BLACK);
      {
        ui_icon_t status_icon = UI_ICON_POWER;

        if(status_color == UI_RED)
        {
          status_icon = UI_ICON_WARN;
        }
        else if(status_color == UI_GREEN)
        {
          status_icon = UI_ICON_CHECK;
        }
        else if(status_color == UI_YELLOW)
        {
          status_icon = UI_ICON_CLOCK;
        }
        else if(status_color == UI_ORANGE)
        {
          status_icon = UI_ICON_FLAME;
        }

        ui_icon_draw(status_icon, 2U, 60U, status_color);
      }
      display_draw_string(12U, 61U, status_text, status_color, UI_BLACK, 2U);

      maindash_last_status_text = status_text;
      maindash_last_status_color = status_color;
      break;
  }
}

void maindash_draw(const sm_status_t *status)
{
  uint8_t step;

  if(status == NULL)
  {
    return;
  }

  /* 页面切换/开机首帧允许一次整屏重绘 */
  maindash_step_active = 0U;
  maindash_cache_valid = 0U;
  maindash_force_draw = 1U;
  maindash_last_draw_ms = sm_get_uptime_ms();
  if(maindash_last_draw_ms == 0U)
  {
    maindash_last_draw_ms = 1U;
  }

  display_clear(UI_BLACK);

  for(step = 0U; step < MAINDASH_DRAW_STEPS; step++)
  {
    maindash_draw_step(status, step);
  }

  maindash_force_draw = 0U;
  maindash_cache_valid = 1U;
}

uint8_t maindash_update(uint32_t now_ms)
{
  sm_status_t status;

  if(maindash_step_active == 0U)
  {
    if((maindash_last_draw_ms != 0U) &&
       ((uint32_t)(now_ms - maindash_last_draw_ms) < UI_INFO_PERIOD_MS))
    {
      return 0U;
    }

    maindash_step_active = 1U;
    maindash_step_index = 0U;
  }

  sm_get_status(&status);
  maindash_draw_step(&status, maindash_step_index);
  maindash_step_index++;

  if(maindash_step_index >= MAINDASH_DRAW_STEPS)
  {
    maindash_step_active = 0U;
    maindash_cache_valid = 1U;
    maindash_last_draw_ms = (now_ms == 0U) ? 1U : now_ms;
    return 1U;
  }

  return 0U;
}
