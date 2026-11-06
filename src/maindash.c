/**
  **************************************************************************
  * @file     maindash.c
  * @brief    160x80 横屏主界面
  **************************************************************************
  * 布局：
  *   y=1    标题 Chill245 + 短状态
  *   y=16   热端温度大号圆润数字（3 位 + °C）
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
#include "fmt.h"
#include "display.h"
#include "powermanagement.h"
#include "onekeyboost.h"
#include "ui_icons.h"
#include "round_digits.h"
#include "big_digits.h"
#include "ui_digits.h"
#include "eeprom.h"
#include "appcolors.h"
#include "wallpaper.h"
#include <stdio.h>

#define UI_BLACK                        appcolors_surface()
#define UI_WHITE                        appcolors_get(APP_COLOR_QUATERNARY)
#define UI_GRAY                         appcolors_get(APP_COLOR_TERTIARY)
#define UI_DARK                         appcolors_surface()
#define UI_HEADER                       appcolors_surface()
#define UI_PINK                         appcolors_get(APP_COLOR_PRIMARY)
#define UI_BLUE                         appcolors_get(APP_COLOR_SECONDARY)
#define UI_RED                          (0xF800U)
#define UI_ORANGE                       UI_PINK
#define UI_GREEN                        (0x07E0U)
#define UI_CYAN                         UI_BLUE
#define UI_YELLOW                       UI_WHITE

#define UI_INFO_PERIOD_MS               (200U)

static uint16_t maindash_round_buf[ROUND_GLYPH_W * ROUND_GLYPH_H];

static void maindash_draw_round_digit(uint16_t x, uint16_t y, uint8_t digit)
{
  static const uint16_t palette[4] =
  {
    0x0000U, 0x3847U, 0x90B2U, 0xFC9FU
  };
  uint16_t col;
  uint16_t row;
  uint16_t index;
  uint8_t alpha;

  if(digit >= ROUND_GLYPH_COUNT)
  {
    display_fill_rect(x, y, ROUND_GLYPH_W, ROUND_GLYPH_H, UI_BLACK);
    return;
  }

  for(row = 0U; row < ROUND_GLYPH_H; row++)
  {
    for(col = 0U; col < ROUND_GLYPH_W; col++)
    {
      index = (uint16_t)(row * 5U + (col >> 2));
      alpha = (uint8_t)((round_digits[digit][index] >> ((col & 3U) * 2U)) & 0x03U);
      maindash_round_buf[row * ROUND_GLYPH_W + col] = palette[alpha];
    }
  }

  display_set_window(x, y, ROUND_GLYPH_W, ROUND_GLYPH_H);
  display_write_pixels(maindash_round_buf, ROUND_GLYPH_W * ROUND_GLYPH_H);
}

#define ROUND_SMALL_W (ROUND_GLYPH_W / 2U)
#define ROUND_SMALL_H (ROUND_GLYPH_H / 2U)

/* 功率用半尺寸圆润数字 */
static void maindash_draw_round_digit_small(uint16_t x, uint16_t y, uint8_t digit)
{
  static const uint16_t palette[4] =
  {
    0x0000U, 0x3847U, 0x90B2U, 0xFC9FU
  };
  uint16_t col;
  uint16_t row;
  uint16_t src_x;
  uint16_t src_y;
  uint16_t index;
  uint8_t alpha;

  if(digit >= ROUND_GLYPH_COUNT)
  {
    display_fill_rect(x, y, ROUND_SMALL_W, ROUND_SMALL_H, UI_BLACK);
    return;
  }

  for(row = 0U; row < ROUND_SMALL_H; row++)
  {
    src_y = (uint16_t)(row * 2U);
    for(col = 0U; col < ROUND_SMALL_W; col++)
    {
      src_x = (uint16_t)(col * 2U);
      index = (uint16_t)(src_y * 5U + (src_x >> 2));
      alpha = (uint8_t)((round_digits[digit][index] >> ((src_x & 3U) * 2U)) & 0x03U);
      maindash_round_buf[row * ROUND_SMALL_W + col] = palette[alpha];
    }
  }

  display_set_window(x, y, ROUND_SMALL_W, ROUND_SMALL_H);
  display_write_pixels(maindash_round_buf, ROUND_SMALL_W * ROUND_SMALL_H);
}

static uint32_t maindash_last_draw_ms;

static int maindash_round_float(float value)
{
  if(value >= 0.0f)
  {
    return (int)(value + 0.5f);
  }

  return (int)(value - 0.5f);
}

static void maindash_draw_temperature(const sm_status_t *status, int display_tip_c)
{
  int tip_c;
  uint8_t hundreds;
  uint8_t tens;
  uint8_t units;

  if(status->temp_valid == 0U)
  {
    display_fill_rect(2U, 13U, 72U, 33U, UI_BLACK);
    maindash_draw_round_digit(2U, 14U, 10U);
    maindash_draw_round_digit(20U, 14U, 10U);
    maindash_draw_round_digit(38U, 14U, 10U);
    return;
  }

  tip_c = display_tip_c;

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
    maindash_draw_round_digit(2U, 14U, hundreds);
  }
  else
  {
    display_fill_rect(2U, 14U, ROUND_GLYPH_W, ROUND_GLYPH_H, UI_BLACK);
  }

  if((hundreds != 0U) || (tens != 0U))
  {
    maindash_draw_round_digit(20U, 14U, tens);
  }
  else
  {
    display_fill_rect(20U, 14U, ROUND_GLYPH_W, ROUND_GLYPH_H, UI_BLACK);
  }

  maindash_draw_round_digit(38U, 14U, units);

  /* °C */
  display_draw_string(57U, 24U, "\x7F" "C", UI_ORANGE, UI_BLACK, 2U);
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

  display_fill_rect(2U, 46U, 70U, 17U, UI_BLACK);

  if(power >= 100U)
  {
    maindash_draw_round_digit_small(4U, 47U, 1U);
    maindash_draw_round_digit_small(14U, 47U, 0U);
    maindash_draw_round_digit_small(24U, 47U, 0U);
    display_draw_string(36U, 49U, "%", UI_GREEN, UI_BLACK, 2U);
  }
  else
  {
    tens = (uint8_t)(power / 10U);
    units = (uint8_t)(power % 10U);

    if(tens != 0U)
    {
      maindash_draw_round_digit_small(4U, 47U, tens);
    }
    else
    {
      display_fill_rect(4U, 47U, ROUND_SMALL_W, ROUND_SMALL_H, UI_BLACK);
    }

    maindash_draw_round_digit_small(14U, 47U, units);
    display_draw_string(26U, 49U, "%", UI_GREEN, UI_BLACK, 2U);
  }

  display_draw_string(42U, 51U, "PWR", UI_GRAY, UI_BLACK, 1U);
  ui_icon_draw(UI_ICON_BOLT, 60U, 48U, UI_GREEN);
}

static void maindash_draw_info_row(uint16_t x, uint16_t y,
                                   ui_icon_t icon, const char *label,
                                   const char *value, uint16_t value_color)
{
  ui_icon_draw(icon, x, (uint16_t)(y + 2U), UI_BLUE);
  display_draw_string((uint16_t)(x + 11U), (uint16_t)(y + 4U),
                      label, UI_BLUE, UI_BLACK, 1U);
  display_fill_rect((uint16_t)(x + 32U), (uint16_t)(y + 1U),
                    48U, 11U, UI_BLACK);
  display_draw_string((uint16_t)(x + 32U), (uint16_t)(y + 1U), value,
                      value_color, UI_BLACK, 2U);
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

#define MAINDASH_DRAW_STEPS (7U)

static uint8_t maindash_step_active;
static uint8_t maindash_force_full;
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
static uint32_t maindash_last_sleep_s;
static uint8_t maindash_last_vibration;
static uint8_t maindash_telemetry_valid;

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

          display_draw_string(2U, 1U, "Chill245", UI_WHITE, UI_HEADER, 2U);
      ui_icon_draw(maindash_top_icon(status), 124U, 2U, top_color);
      display_draw_string(134U, 1U, top_state, top_color, UI_HEADER, 2U);

      display_fill_rect(0U, 12U, DISPLAY_WIDTH, 1U, UI_BLUE);
      display_fill_rect(0U, 63U, DISPLAY_WIDTH, 1U, UI_BLUE);
      display_fill_rect(74U, 13U, 1U, 50U, UI_BLUE);
      display_fill_rect(0U, 13U, 2U, 50U, UI_PINK);

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

        /* 显示回差：避免在 +-0.5°C 边界来回跳 1°C */
        if((maindash_cache_valid != 0U) &&
           (maindash_last_tip >= 0) &&
           (((tip_c - maindash_last_tip) == 1) ||
            ((maindash_last_tip - tip_c) == 1)))
        {
          float diff = status->tip_c - (float)maindash_last_tip;

          if(diff < 0.0f)
          {
            diff = -diff;
          }

          if(diff < 0.7f)
          {
            tip_c = maindash_last_tip;
          }
        }
      }

      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_tip == tip_c))
      {
        break;
      }

      maindash_draw_temperature(status, tip_c);
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

      fmt_snprintf(value, sizeof(value), "%d.%dV",
               vin_mv / 1000, (vin_mv % 1000) / 100);
      maindash_draw_info_row(78U, 13U, UI_ICON_PLUG, "VIN", value,
                             (power_get_fault() != POWER_FAULT_NONE) ?
                             UI_RED : UI_BLUE);

      if(cold_x10_new < 0)
      {
        fmt_snprintf(value, sizeof(value), "-%d.%dC",
                 (-cold_x10_new) / 10, (-cold_x10_new) % 10);
      }
      else
      {
        fmt_snprintf(value, sizeof(value), "%d.%dC",
                 cold_x10_new / 10, cold_x10_new % 10);
      }
      maindash_draw_info_row(78U, 26U, UI_ICON_SNOW, "COLD", value, UI_WHITE);

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

      fmt_snprintf(value, sizeof(value), "%dC", setpoint_new);
      maindash_draw_info_row(78U, 39U, UI_ICON_TARGET, "SET", value, UI_WHITE);
      maindash_draw_info_row(78U, 52U, UI_ICON_MODE, "MODE", mode_new, UI_WHITE);

      maindash_last_setpoint = setpoint_new;
      maindash_last_mode_text = mode_new;
      break;
    }

    case 5U:
      status_text = maindash_status_text(status, &status_color);
      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_last_status_text == status_text) &&
         (maindash_last_status_color == status_color))
      {
        break;
      }

      /* 只清左半区，右侧休眠/震动指示由 step 6 维护 */
      display_fill_rect(0U, 64U, 82U, 13U, UI_BLACK);
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

        ui_icon_draw(status_icon, 2U, 65U, status_color);
      }
      display_draw_string(12U, 66U, status_text, status_color, UI_BLACK, 2U);

      maindash_last_status_text = status_text;
      maindash_last_status_color = status_color;
      break;

    case 6U:
    {
      uint32_t remaining_ms = power_get_sleep_remaining_ms();
      uint32_t seconds = (remaining_ms + 999U) / 1000U;
      uint8_t vibration = power_get_vibration_state();

      if(seconds > 9999U)
      {
        seconds = 9999U;
      }

      if((maindash_force_draw == 0U) && (maindash_cache_valid != 0U) &&
         (maindash_telemetry_valid != 0U) &&
         (maindash_last_sleep_s == seconds) &&
         (maindash_last_vibration == vibration))
      {
        break;
      }

      display_fill_rect(82U, 64U, 74U, 13U, UI_BLACK);
      display_draw_string(84U, 66U, "SLEEP", UI_BLUE, UI_BLACK, 1U);

      /* 震动触发圆点：触发时粉色，静止时暗蓝 */
      display_fill_rect(107U, 68U, 4U, 4U,
                        (vibration != 0U) ? UI_PINK : UI_DARK);

      fmt_snprintf(value, sizeof(value), "%lus", (unsigned long)seconds);
      display_draw_string(114U, 66U, value, UI_PINK, UI_BLACK, 1U);

      maindash_last_sleep_s = seconds;
      maindash_last_vibration = vibration;
      maindash_telemetry_valid = 1U;
      break;
    }

    default:
      break;
  }
}


/* ============================ 新版全屏主界面 ============================ */

static uint32_t maindash_alt_last_draw_ms;
static uint32_t maindash_alt_setpoint_until_ms;


void maindash_notify_setpoint_adjusted(void)
{
  uint32_t now_ms = sm_get_uptime_ms();

  maindash_alt_setpoint_until_ms = now_ms + 2000U;
}

static void maindash_alt_big_digit(uint16_t x, uint16_t y, uint8_t digit)
{
  uint16_t row;
  uint16_t col;
  uint16_t bit;
  uint8_t on;

  if(digit >= BIG_GLYPH_COUNT)
  {
    return;
  }

  for(row = 0U; row < BIG_GLYPH_H; row++)
  {
    for(col = 0U; col < BIG_GLYPH_W; col++)
    {
      bit = (uint16_t)(row * BIG_GLYPH_W + col);
      on = (uint8_t)((big_digits[digit][bit >> 3] >>
                      (7U - (bit & 7U))) & 1U);
      if(on != 0U)
      {
        /* 只画前景，壁纸保留在字形内部，形成透明大字效果 */
        display_draw_pixel((uint16_t)(x + col), (uint16_t)(y + row), UI_WHITE);
      }
    }
  }
}

static const char *maindash_alt_error(const sm_status_t *status, uint16_t *color)
{
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

  if(status->state == SM_STATE_FAULT)
  {
    *color = UI_RED;
    return "FAULT";
  }

  *color = UI_GRAY;
  return NULL;
}

static void maindash_alt_draw(const sm_status_t *status, uint32_t now_ms)
{
  uint8_t disabled;
  uint8_t show_set;
  uint8_t blink_on;
  int tip_c;
  int shown_c;
  int hundreds;
  int tens;
  int units;
  int percent;
  uint16_t error_color;
  const char *error_text;
  char info[24];

  if(wallpaper_is_available() != 0U)
  {
    wallpaper_draw();
  }
  else
  {
    display_fill_rect(0U, 0U, DISPLAY_WIDTH, DISPLAY_HEIGHT, UI_BLACK);
  }

  tip_c = (status->temp_valid != 0U) ?
          maindash_round_float(status->tip_c) : 0;
  if(tip_c < 0)
  {
    tip_c = 0;
  }
  if(tip_c > 999)
  {
    tip_c = 999;
  }

  show_set = ((int32_t)(maindash_alt_setpoint_until_ms - now_ms) > 0) ? 1U : 0U;
  shown_c = show_set ?
            maindash_round_float(status->setpoint_c) : tip_c;
  if(shown_c < 0)
  {
    shown_c = 0;
  }
  if(shown_c > 999)
  {
    shown_c = 999;
  }

  error_text = maindash_alt_error(status, &error_color);
  disabled = ((power_get_state() != POWER_STATE_ACTIVE) ||
              (sm_is_heater_enabled() == 0U) ||
              (status->state == SM_STATE_DISABLED)) ? 1U : 0U;

  /* 左上角状态 */
  display_draw_string_transparent(2U, 0U, show_set ? "SET" : "NOW",
                      show_set ? UI_PINK : UI_GRAY, 2U);

  if(disabled != 0U)
  {
    blink_on = (uint8_t)(((now_ms / 500U) & 1U) == 0U);
    if(blink_on != 0U)
    {
      display_draw_string_transparent(14U, 23U, "DISABLED", UI_GRAY, 4U);
    }
  }
  else if((show_set == 0U) && (status->temp_valid == 0U))
  {
    display_draw_string_transparent(56U, 23U, "---", UI_WHITE, 4U);
  }
  else
  {
    hundreds = shown_c / 100;
    tens = (shown_c / 10) % 10;
    units = shown_c % 10;

    if(hundreds != 0)
    {
      maindash_alt_big_digit(9U, 11U, (uint8_t)hundreds);
    }

    if((hundreds != 0) || (tens != 0))
    {
      maindash_alt_big_digit(57U, 11U, (uint8_t)tens);
    }

    maindash_alt_big_digit(105U, 11U, (uint8_t)units);
  }

  /* 左下角信息：报错优先，否则模式/冷端/电压轮换 */
  if(error_text != NULL)
  {
    display_draw_string_transparent(2U, 67U, error_text, error_color, 2U);
  }
  else
  {
    switch((now_ms / 2000U) % 3U)
    {
      case 0U:
        fmt_snprintf(info, sizeof(info), "MODE %s",
                     maindash_mode_text(status->mode));
        break;

      case 1U:
      {
        int cold_x10 = maindash_round_float(status->room_c * 10.0f);
        int whole = cold_x10 / 10;
        int frac = cold_x10 % 10;

        if(frac < 0)
        {
          frac = -frac;
        }

        fmt_snprintf(info, sizeof(info), "COLD %d.%dC", whole, frac);
        break;
      }

      default:
      {
        uint16_t vin_mv = power_get_voltage_mv();

        fmt_snprintf(info, sizeof(info), "VIN %d.%dV",
                     (int)(vin_mv / 1000U),
                     (int)((vin_mv % 1000U) / 100U));
        break;
      }
    }

    display_draw_string_transparent(2U, 67U, info, UI_BLUE, 2U);
  }

  /* 最下方进度条：直接显示当前加热功率 0~100% */
  percent = (int)status->power;

  if(percent < 0)
  {
    percent = 0;
  }
  if(percent > 100)
  {
    percent = 100;
  }

  display_fill_rect(2U, 78U, 156U, 2U, UI_DARK);
  if(percent > 0)
  {
    uint16_t width = (uint16_t)((154U * (uint16_t)percent) / 100U);

    display_fill_rect(2U, 78U, width, 2U,
                      (disabled != 0U) ? UI_GRAY :
                      ((error_text != NULL) ? UI_RED : UI_PINK));
  }

  maindash_alt_last_draw_ms = (now_ms == 0U) ? 1U : now_ms;
}


/* ============================ 新版 maindash 脏刷新 ============================ */

#define ALT_DIRTY_TOP       (1U << 0)
#define ALT_DIRTY_TEMP      (1U << 1)
#define ALT_DIRTY_BODY      (1U << 2)
#define ALT_DIRTY_INFO      (1U << 3)
#define ALT_DIRTY_PROGRESS  (1U << 4)

static int alt_last_shown = -1;
static int alt_last_power = -1;
static uint8_t alt_last_set;
static uint8_t alt_last_disabled;
static uint8_t alt_last_valid;
static uint8_t alt_last_blink;
static uint8_t alt_last_info;
static const char *alt_last_error;

static void maindash_alt_draw_temp(int shown_c, uint8_t valid)
{
  int hundreds = shown_c / 100;
  int tens = (shown_c / 10) % 10;
  int units = shown_c % 10;

  if(valid == 0U)
  {
    display_draw_string_transparent(56U, 23U, "---", UI_WHITE, 4U);
    return;
  }

  if(hundreds != 0)
  {
    maindash_alt_big_digit(9U, 11U, (uint8_t)hundreds);
  }
  if((hundreds != 0) || (tens != 0))
  {
    maindash_alt_big_digit(57U, 11U, (uint8_t)tens);
  }
  maindash_alt_big_digit(105U, 11U, (uint8_t)units);
}

static void maindash_alt_partial(const sm_status_t *status, uint32_t now_ms,
                                 uint16_t mask)
{
  int shown_c;
  uint8_t disabled;
  uint8_t show_set;
  uint16_t error_color;
  const char *error_text;
  char info[24];
  uint8_t info_index;
  uint8_t blink_on;
  int percent;

  show_set = ((int32_t)(maindash_alt_setpoint_until_ms - now_ms) > 0) ? 1U : 0U;
  shown_c = show_set ?
            maindash_round_float(status->setpoint_c) :
            ((status->temp_valid != 0U) ? maindash_round_float(status->tip_c) : 0);
  if(shown_c < 0) shown_c = 0;
  if(shown_c > 999) shown_c = 999;

  error_text = maindash_alt_error(status, &error_color);
  disabled = ((power_get_state() != POWER_STATE_ACTIVE) ||
              (sm_is_heater_enabled() == 0U) ||
              (status->state == SM_STATE_DISABLED)) ? 1U : 0U;
  info_index = (uint8_t)((now_ms / 2000U) % 3U);
  blink_on = (uint8_t)(((now_ms / 500U) & 1U) == 0U);

  if((mask & ALT_DIRTY_BODY) != 0U)
  {
    wallpaper_blit(0U, 10U, DISPLAY_WIDTH, 57U);

    if(disabled != 0U)
    {
      if(blink_on != 0U)
      {
        display_draw_string_transparent(14U, 23U, "DISABLED", UI_GRAY, 4U);
      }
    }
    else if((show_set == 0U) && (status->temp_valid == 0U))
    {
      display_draw_string_transparent(56U, 23U, "---", UI_WHITE, 4U);
    }
    else
    {
      maindash_alt_draw_temp(shown_c, 1U);
    }
  }
  else if(((mask & ALT_DIRTY_TEMP) != 0U) && (disabled == 0U))
  {
    wallpaper_blit(9U, 11U, 138U, 56U);
    maindash_alt_draw_temp(shown_c, (uint8_t)((show_set != 0U) || (status->temp_valid != 0U)));
  }

  if((mask & ALT_DIRTY_TOP) != 0U)
  {
    wallpaper_blit(2U, 0U, 34U, 10U);
    display_draw_string_transparent(2U, 0U, show_set ? "SET" : "NOW",
                                    show_set ? UI_PINK : UI_GRAY, 2U);
  }

  if((mask & ALT_DIRTY_INFO) != 0U)
  {
    wallpaper_blit(0U, 67U, DISPLAY_WIDTH, 10U);

    if(error_text != NULL)
    {
      display_draw_string_transparent(2U, 67U, error_text, error_color, 2U);
    }
    else
    {
      switch(info_index)
      {
        case 0U:
          fmt_snprintf(info, sizeof(info), "MODE %s",
                       maindash_mode_text(status->mode));
          break;
        case 1U:
        {
          int cold_x10 = maindash_round_float(status->room_c * 10.0f);
          int whole = cold_x10 / 10;
          int frac = cold_x10 % 10;
          if(frac < 0) frac = -frac;
          fmt_snprintf(info, sizeof(info), "COLD %d.%dC", whole, frac);
          break;
        }
        default:
        {
          uint16_t vin_mv = power_get_voltage_mv();
          fmt_snprintf(info, sizeof(info), "VIN %d.%dV",
                       (int)(vin_mv / 1000U),
                       (int)((vin_mv % 1000U) / 100U));
          break;
        }
      }
      display_draw_string_transparent(2U, 67U, info, UI_BLUE, 2U);
    }
  }

  if((mask & ALT_DIRTY_PROGRESS) != 0U)
  {
    percent = (int)status->power;
    if(percent < 0) percent = 0;
    if(percent > 100) percent = 100;

    wallpaper_blit(2U, 78U, 156U, 2U);
    if(percent > 0)
    {
      uint16_t width = (uint16_t)((154U * (uint16_t)percent) / 100U);
      display_fill_rect(2U, 78U, width, 2U,
                        (disabled != 0U) ? UI_GRAY :
                        ((error_text != NULL) ? UI_RED : UI_PINK));
    }
  }
}

static void maindash_alt_update(const sm_status_t *status, uint32_t now_ms)
{
  uint16_t mask = 0U;
  int shown_c;
  uint8_t disabled;
  uint8_t show_set;
  uint8_t info_index;
  uint8_t blink_on;
  uint16_t error_color;
  const char *error_text;

  if(maindash_force_full != 0U)
  {
    maindash_force_full = 0U;
    maindash_alt_draw(status, now_ms);
    goto cache;
  }

  show_set = ((int32_t)(maindash_alt_setpoint_until_ms - now_ms) > 0) ? 1U : 0U;
  shown_c = show_set ?
            maindash_round_float(status->setpoint_c) :
            ((status->temp_valid != 0U) ? maindash_round_float(status->tip_c) : 0);
  if(shown_c < 0) shown_c = 0;
  if(shown_c > 999) shown_c = 999;

  error_text = maindash_alt_error(status, &error_color);
  disabled = ((power_get_state() != POWER_STATE_ACTIVE) ||
              (sm_is_heater_enabled() == 0U) ||
              (status->state == SM_STATE_DISABLED)) ? 1U : 0U;
  info_index = (uint8_t)((now_ms / 2000U) % 3U);
  blink_on = (uint8_t)(((now_ms / 500U) & 1U) == 0U);

  if(show_set != alt_last_set)
  {
    mask |= ALT_DIRTY_TOP | ALT_DIRTY_TEMP;
  }
  if((disabled == 0U) &&
     ((shown_c != alt_last_shown) ||
      (status->temp_valid != alt_last_valid)))
  {
    mask |= ALT_DIRTY_TEMP;
  }
  if(disabled != alt_last_disabled)
  {
    mask |= ALT_DIRTY_BODY | ALT_DIRTY_PROGRESS;
  }
  if((error_text != alt_last_error) || (info_index != alt_last_info))
  {
    mask |= ALT_DIRTY_INFO;
  }
  if((int)status->power != alt_last_power)
  {
    mask |= ALT_DIRTY_PROGRESS;
  }
  if((disabled != 0U) && (blink_on != alt_last_blink))
  {
    mask |= ALT_DIRTY_BODY;
  }

  if(mask != 0U)
  {
    maindash_alt_partial(status, now_ms, mask);
  }

cache:
  alt_last_shown = shown_c;
  alt_last_power = (int)status->power;
  alt_last_set = show_set;
  alt_last_disabled = disabled;
  alt_last_valid = status->temp_valid;
  alt_last_blink = blink_on;
  alt_last_info = info_index;
  alt_last_error = error_text;
}

/* ============================ 旧版主界面新排版 ============================ */

static void maindash_classic_temp_digit(uint16_t x, uint16_t y, uint8_t digit)
{
  uint16_t row;
  uint16_t col;
  uint16_t bit;
  uint8_t on;

  if(digit >= UI_GLYPH_COUNT)
  {
    return;
  }

  for(row = 0U; row < UI_GLYPH_H; row++)
  {
    for(col = 0U; col < UI_GLYPH_W; col++)
    {
      bit = (uint16_t)(row * UI_GLYPH_W + col);
      on = (uint8_t)((ui_digits[digit][bit >> 3] >>
                      (7U - (bit & 7U))) & 1U);
      if(on != 0U)
      {
        display_draw_pixel((uint16_t)(x + col), (uint16_t)(y + row), UI_WHITE);
      }
    }
  }
}

static void maindash_classic_top_text(uint16_t x, uint16_t y,
                                      const char *text, uint8_t scale,
                                      uint16_t color, uint16_t fill_width)
{
  uint16_t fg;

  while(*text != '\0')
  {
    fg = (x < fill_width) ? UI_BLACK : color;
    display_draw_char_transparent((uint16_t)(x + 1U),
                                  (uint16_t)(y + 1U), *text,
                                  display_contrast_shadow(fg), scale);
    if((x > 0U) && (y > 0U))
    {
      display_draw_char_transparent((uint16_t)(x - 1U),
                                    (uint16_t)(y - 1U), *text,
                                    display_contrast_shadow(fg), scale);
    }
    display_draw_char_transparent(x, y, *text, fg, scale);
    x = (uint16_t)(x + (4U * scale));
    text++;
  }
}

static void maindash_classic_draw(const sm_status_t *status, uint32_t now_ms)
{
  const char *top_state;
  const char *status_text;
  uint16_t top_color;
  uint16_t status_color;
  uint16_t fill_width;
  int tip_c;
  int hundreds;
  int tens;
  int units;
  uint8_t pair;
  uint8_t show_set;
  char value[20];
  char text[20];

  if(status == NULL)
  {
    return;
  }

  if(wallpaper_is_available() != 0U)
  {
    wallpaper_draw();
  }
  else
  {
    display_fill_rect(0U, 0U, DISPLAY_WIDTH, DISPLAY_HEIGHT, UI_BLACK);
  }

  /* 顶栏：整条功率进度 + 功率百分比 */
  fill_width = (uint16_t)(((uint16_t)status->power * 160U) / 100U);

  if(fill_width > 0U)
  {
    display_fill_rect(0U, 0U, fill_width, 12U, UI_PINK);
  }

  maindash_classic_top_text(2U, 1U, "Chill245", 2U, UI_WHITE, fill_width);

  top_state = maindash_top_state(status, &top_color);
  maindash_classic_top_text(80U, 1U, top_state, 2U, top_color, fill_width);

  fmt_snprintf(text, sizeof(text), "%u%%", (unsigned)status->power);
  maindash_classic_top_text((uint16_t)(158U - display_string_width(text, 2U)),
                            1U, text, 2U, UI_WHITE, fill_width);

  display_fill_rect(0U, 12U, DISPLAY_WIDTH, 1U, UI_BLUE);

  /* 左侧只显示热端温度；按键调温后 2 s 内显示设定温度 */
  show_set = ((int32_t)(maindash_alt_setpoint_until_ms - now_ms) > 0) ? 1U : 0U;
  tip_c = show_set ?
          maindash_round_float(status->setpoint_c) :
          ((status->temp_valid != 0U) ?
           maindash_round_float(status->tip_c) : 0);
  if(tip_c < 0)
  {
    tip_c = 0;
  }
  if(tip_c > 999)
  {
    tip_c = 999;
  }

  if((show_set == 0U) && (status->temp_valid == 0U))
  {
    display_draw_string_transparent(13U, 25U, "---", UI_WHITE, 4U);
  }
  else
  {
    hundreds = tip_c / 100;
    tens = (tip_c / 10) % 10;
    units = tip_c % 10;

    if(hundreds != 0)
    {
      maindash_classic_temp_digit(4U, 18U, (uint8_t)hundreds);
    }

    if((hundreds != 0) || (tens != 0))
    {
      maindash_classic_temp_digit(24U, 18U, (uint8_t)tens);
    }

    maindash_classic_temp_digit(44U, 18U, (uint8_t)units);
    display_draw_string_transparent(66U, 28U, "C", UI_WHITE, 2U);
  }

  display_fill_rect(74U, 13U, 1U, 50U, UI_BLUE);

  /* 右侧：一次两行，2 秒换一组；两行使用不透明面板保证可读 */
  pair = (uint8_t)((now_ms / 2000U) & 1U);

  if(pair == 0U)
  {
    display_draw_string_transparent(78U, 20U, "SET", UI_BLUE, 1U);
    fmt_snprintf(value, sizeof(value), "%dC",
                 maindash_round_float(status->setpoint_c));
    display_draw_string_transparent(104U, 15U, value, UI_WHITE, 3U);

    display_fill_rect(78U, 36U, 80U, 1U, UI_DARK);

    display_draw_string_transparent(78U, 43U, "MODE", UI_BLUE, 1U);
    display_draw_string_transparent(104U, 38U, maindash_mode_text(status->mode),
                        UI_WHITE, 3U);
  }
  else
  {
    uint16_t vin_mv = power_get_voltage_mv();
    int cold_x10 = maindash_round_float(status->room_c * 10.0f);
    int whole = cold_x10 / 10;
    int frac = cold_x10 % 10;

    if(frac < 0)
    {
      frac = -frac;
    }

    display_draw_string_transparent(78U, 20U, "VIN", UI_BLUE, 1U);
    fmt_snprintf(value, sizeof(value), "%d.%dV",
                 (int)(vin_mv / 1000U), (int)((vin_mv % 1000U) / 100U));
    display_draw_string_transparent(104U, 15U, value, UI_WHITE, 3U);

    display_fill_rect(78U, 36U, 80U, 1U, UI_DARK);

    display_draw_string_transparent(78U, 43U, "COLD", UI_BLUE, 1U);
    fmt_snprintf(value, sizeof(value), "%d.%dC", whole, frac);
    display_draw_string_transparent(104U, 38U, value, UI_WHITE, 3U);
  }

  display_fill_rect(0U, 63U, DISPLAY_WIDTH, 1U, UI_BLUE);

  /* 底部状态 + 休眠遥测 */
  status_text = maindash_status_text(status, &status_color);
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

    ui_icon_draw(status_icon, 2U, 65U, status_color);
  }
  display_draw_string_transparent(12U, 66U, status_text, status_color, 2U);

  {
    uint32_t remaining_ms = power_get_sleep_remaining_ms();
    uint32_t seconds = (remaining_ms + 999U) / 1000U;
    uint8_t vibration = power_get_vibration_state();

    display_draw_string_transparent(84U, 66U, "SLEEP", UI_BLUE, 1U);
    display_fill_rect(107U, 68U, 4U, 4U,
                      (vibration != 0U) ? UI_PINK : UI_DARK);
    fmt_snprintf(value, sizeof(value), "%lus", (unsigned long)seconds);
    display_draw_string_transparent(114U, 66U, value, UI_PINK, 1U);
  }

  maindash_last_draw_ms = (now_ms == 0U) ? 1U : now_ms;
}

/* ============================ 旧版 maindash 脏刷新 ============================ */

#define CL_DIRTY_TOP       (1U << 0)
#define CL_DIRTY_TEMP      (1U << 1)
#define CL_DIRTY_RIGHT     (1U << 2)
#define CL_DIRTY_BOTTOM    (1U << 3)
#define CL_DIRTY_PROGRESS  (1U << 4)

static int cl_last_tip = -1;
static int cl_last_power = -1;
static uint8_t cl_last_set;
static uint8_t cl_last_valid;
static uint8_t cl_last_pair;
static const char *cl_last_top;
static const char *cl_last_status;
static int cl_last_seconds = -1;
static uint8_t cl_last_vibration;
static uint32_t cl_last_right_ms;

static void maindash_classic_top_draw(const sm_status_t *status)
{
  uint16_t fill_width;
  const char *top_state;
  uint16_t top_color;
  char text[20];

  fill_width = (uint16_t)(((uint16_t)status->power * 160U) / 100U);
  wallpaper_blit(0U, 0U, DISPLAY_WIDTH, 12U);
  if(fill_width > 0U)
  {
    display_fill_rect(0U, 0U, fill_width, 12U, UI_PINK);
  }

  maindash_classic_top_text(2U, 1U, "Chill245", 2U, UI_WHITE, fill_width);
  top_state = maindash_top_state(status, &top_color);
  maindash_classic_top_text(80U, 1U, top_state, 2U, top_color, fill_width);
  fmt_snprintf(text, sizeof(text), "%u%%", (unsigned)status->power);
  maindash_classic_top_text((uint16_t)(158U - display_string_width(text, 2U)),
                            1U, text, 2U, UI_WHITE, fill_width);
}

static void maindash_classic_temp_draw(const sm_status_t *status, uint32_t now_ms)
{
  uint8_t show_set;
  int tip_c;
  int hundreds;
  int tens;
  int units;

  show_set = ((int32_t)(maindash_alt_setpoint_until_ms - now_ms) > 0) ? 1U : 0U;
  tip_c = show_set ? maindash_round_float(status->setpoint_c) :
          ((status->temp_valid != 0U) ? maindash_round_float(status->tip_c) : 0);
  if(tip_c < 0) tip_c = 0;
  if(tip_c > 999) tip_c = 999;

  wallpaper_blit(0U, 13U, 74U, 50U);

  if((show_set == 0U) && (status->temp_valid == 0U))
  {
    display_draw_string_transparent(13U, 25U, "---", UI_WHITE, 4U);
    return;
  }

  hundreds = tip_c / 100;
  tens = (tip_c / 10) % 10;
  units = tip_c % 10;

  if(hundreds != 0)
  {
    maindash_classic_temp_digit(4U, 18U, (uint8_t)hundreds);
  }
  if((hundreds != 0) || (tens != 0))
  {
    maindash_classic_temp_digit(24U, 18U, (uint8_t)tens);
  }
  maindash_classic_temp_digit(44U, 18U, (uint8_t)units);
  display_draw_string_transparent(66U, 28U, "C", UI_WHITE, 2U);
}

static void maindash_classic_right_draw(const sm_status_t *status, uint32_t now_ms)
{
  uint8_t pair;
  char value[20];

  wallpaper_blit(76U, 14U, 84U, 49U);
  pair = (uint8_t)((now_ms / 2000U) & 1U);

  if(pair == 0U)
  {
    display_draw_string_transparent(78U, 20U, "SET", UI_BLUE, 1U);
    fmt_snprintf(value, sizeof(value), "%dC",
                 maindash_round_float(status->setpoint_c));
    display_draw_string_transparent(104U, 15U, value, UI_WHITE, 3U);
    display_fill_rect(78U, 36U, 80U, 1U, UI_DARK);
    display_draw_string_transparent(78U, 43U, "MODE", UI_BLUE, 1U);
    display_draw_string_transparent(104U, 38U, maindash_mode_text(status->mode),
                                    UI_WHITE, 3U);
  }
  else
  {
    uint16_t vin_mv = power_get_voltage_mv();
    int cold_x10 = maindash_round_float(status->room_c * 10.0f);
    int whole = cold_x10 / 10;
    int frac = cold_x10 % 10;
    if(frac < 0) frac = -frac;

    display_draw_string_transparent(78U, 20U, "VIN", UI_BLUE, 1U);
    fmt_snprintf(value, sizeof(value), "%d.%dV",
                 (int)(vin_mv / 1000U), (int)((vin_mv % 1000U) / 100U));
    display_draw_string_transparent(104U, 15U, value, UI_WHITE, 3U);
    display_fill_rect(78U, 36U, 80U, 1U, UI_DARK);
    display_draw_string_transparent(78U, 43U, "COLD", UI_BLUE, 1U);
    fmt_snprintf(value, sizeof(value), "%d.%dC", whole, frac);
    display_draw_string_transparent(104U, 38U, value, UI_WHITE, 3U);
  }
}

static void maindash_classic_bottom_draw(const sm_status_t *status)
{
  const char *status_text;
  uint16_t status_color;
  ui_icon_t status_icon = UI_ICON_POWER;
  char value[20];
  uint32_t remaining_ms;
  uint32_t seconds;
  uint8_t vibration;

  wallpaper_blit(0U, 64U, DISPLAY_WIDTH, 13U);
  status_text = maindash_status_text(status, &status_color);

  if(status_color == UI_RED) status_icon = UI_ICON_WARN;
  else if(status_color == UI_GREEN) status_icon = UI_ICON_CHECK;
  else if(status_color == UI_YELLOW) status_icon = UI_ICON_CLOCK;
  else if(status_color == UI_ORANGE) status_icon = UI_ICON_FLAME;

  ui_icon_draw(status_icon, 2U, 65U, status_color);
  display_draw_string_transparent(12U, 66U, status_text, status_color, 2U);

  remaining_ms = power_get_sleep_remaining_ms();
  seconds = (remaining_ms + 999U) / 1000U;
  vibration = power_get_vibration_state();
  display_draw_string_transparent(84U, 66U, "SLEEP", UI_BLUE, 1U);
  display_fill_rect(107U, 68U, 4U, 4U,
                    (vibration != 0U) ? UI_PINK : UI_DARK);
  fmt_snprintf(value, sizeof(value), "%lus", (unsigned long)seconds);
  display_draw_string_transparent(114U, 66U, value, UI_PINK, 1U);
}

static void maindash_classic_progress_draw(const sm_status_t *status)
{
  int percent = (int)status->power;
  if(percent < 0) percent = 0;
  if(percent > 100) percent = 100;

  wallpaper_blit(2U, 78U, 156U, 2U);
  if(percent > 0)
  {
    uint16_t width = (uint16_t)((154U * (uint16_t)percent) / 100U);
    display_fill_rect(2U, 78U, width, 2U, UI_PINK);
  }
}

static void maindash_classic_partial(const sm_status_t *status, uint32_t now_ms,
                                     uint16_t mask)
{
  if((mask & CL_DIRTY_TOP) != 0U)
  {
    maindash_classic_top_draw(status);
  }
  if((mask & CL_DIRTY_TEMP) != 0U)
  {
    maindash_classic_temp_draw(status, now_ms);
  }
  if((mask & CL_DIRTY_RIGHT) != 0U)
  {
    maindash_classic_right_draw(status, now_ms);
  }
  if((mask & CL_DIRTY_BOTTOM) != 0U)
  {
    maindash_classic_bottom_draw(status);
  }
  if((mask & CL_DIRTY_PROGRESS) != 0U)
  {
    maindash_classic_progress_draw(status);
  }
}

static void maindash_classic_update(const sm_status_t *status, uint32_t now_ms)
{
  uint16_t mask = 0U;
  uint8_t show_set;
  int tip_c;
  uint8_t pair;
  const char *top_state;
  uint16_t top_color;
  const char *status_text;
  uint16_t status_color;
  uint32_t seconds;
  uint8_t vibration;

  if(maindash_force_full != 0U)
  {
    maindash_force_full = 0U;
    maindash_classic_draw(status, now_ms);
    goto cache;
  }

  show_set = ((int32_t)(maindash_alt_setpoint_until_ms - now_ms) > 0) ? 1U : 0U;
  tip_c = show_set ? maindash_round_float(status->setpoint_c) :
          ((status->temp_valid != 0U) ? maindash_round_float(status->tip_c) : 0);
  if(tip_c < 0) tip_c = 0;
  if(tip_c > 999) tip_c = 999;
  pair = (uint8_t)((now_ms / 2000U) & 1U);
  top_state = maindash_top_state(status, &top_color);
  status_text = maindash_status_text(status, &status_color);
  seconds = (power_get_sleep_remaining_ms() + 999U) / 1000U;
  vibration = power_get_vibration_state();

  if((show_set != cl_last_set) ||
     (tip_c != cl_last_tip) ||
     (status->temp_valid != cl_last_valid))
  {
    mask |= CL_DIRTY_TEMP;
  }
  if((int)status->power != cl_last_power)
  {
    mask |= CL_DIRTY_TOP | CL_DIRTY_PROGRESS;
  }
  if((top_state != cl_last_top) || (pair != cl_last_pair))
  {
    mask |= CL_DIRTY_TOP;
  }
  if((uint32_t)(now_ms - cl_last_right_ms) >= 500U)
  {
    mask |= CL_DIRTY_RIGHT;
    cl_last_right_ms = now_ms;
  }
  if((status_text != cl_last_status) ||
     ((int)seconds != cl_last_seconds) ||
     (vibration != cl_last_vibration))
  {
    mask |= CL_DIRTY_BOTTOM;
  }
  if(pair != cl_last_pair)
  {
    mask |= CL_DIRTY_RIGHT;
  }

  if(mask != 0U)
  {
    maindash_classic_partial(status, now_ms, mask);
  }

cache:
  cl_last_tip = tip_c;
  cl_last_power = (int)status->power;
  cl_last_set = show_set;
  cl_last_valid = status->temp_valid;
  cl_last_pair = pair;
  cl_last_top = top_state;
  cl_last_status = status_text;
  cl_last_seconds = (int)seconds;
  cl_last_vibration = vibration;
}

void maindash_invalidate(void)
{
  maindash_force_full = 1U;
}

void maindash_draw(const sm_status_t *status)
{
  maindash_force_full = 0U;
  if(status == NULL)
  {
    return;
  }

  if(g_eeprom_params.maindash_alt != 0U)
  {
    maindash_alt_draw(status, sm_get_uptime_ms());
    return;
  }

  maindash_classic_draw(status, sm_get_uptime_ms());
}

uint8_t maindash_update(uint32_t now_ms)
{
  sm_status_t status;

  if(g_eeprom_params.maindash_alt != 0U)
  {
    if((maindash_alt_last_draw_ms != 0U) &&
       ((uint32_t)(now_ms - maindash_alt_last_draw_ms) < 250U))
    {
      return 0U;
    }

    sm_get_status(&status);
    maindash_alt_update(&status, now_ms);
    maindash_alt_last_draw_ms = (now_ms == 0U) ? 1U : now_ms;
    return 1U;
  }

  if((maindash_last_draw_ms != 0U) &&
     ((uint32_t)(now_ms - maindash_last_draw_ms) < 250U))
  {
    return 0U;
  }

  sm_get_status(&status);
  maindash_classic_update(&status, now_ms);
  maindash_last_draw_ms = (now_ms == 0U) ? 1U : now_ms;
  return 1U;
}
