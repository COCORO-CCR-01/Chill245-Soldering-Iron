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
#include <stdio.h>

#define UI_BLACK                        (0x0000U)
#define UI_WHITE                        (0xFFFFU)
#define UI_GRAY                         (0x9CF3U)
#define UI_DARK                         (0x18C3U)
#define UI_HEADER                       (0x0861U)
#define UI_PINK                         (0xFC9FU)
#define UI_BLUE                         (0x05FFU)
#define UI_RED                          (0xF800U)
#define UI_ORANGE                       UI_PINK
#define UI_GREEN                        UI_BLUE
#define UI_CYAN                         UI_BLUE
#define UI_YELLOW                       UI_WHITE

#define UI_INFO_PERIOD_MS               (200U)

static uint16_t maindash_round_buf[ROUND_GLYPH_W * ROUND_GLYPH_H];

static void maindash_draw_round_digit(uint16_t x, uint16_t y, uint8_t digit)
{
  static const uint16_t palette[4] =
  {
    UI_BLACK, 0x3847U, 0x90B2U, UI_PINK
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
    UI_BLACK, 0x3847U, 0x90B2U, UI_PINK
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

      display_fill_rect(0U, 0U, DISPLAY_WIDTH, 12U, UI_HEADER);
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
static uint16_t maindash_alt_line[BIG_GLYPH_W];

void maindash_notify_setpoint_adjusted(void)
{
  uint32_t now_ms = sm_get_uptime_ms();

  maindash_alt_setpoint_until_ms = now_ms + 2000U;
}

static void maindash_alt_big_digit(uint16_t x, uint16_t y, uint8_t digit)
{
  static const uint16_t palette[4] =
  {
    UI_BLACK, 0x2002U, UI_PINK, UI_PINK
  };
  uint16_t row;
  uint16_t col;
  uint16_t index;
  uint8_t alpha;

  if(digit >= 10U)
  {
    display_fill_rect(x, y, BIG_GLYPH_W, BIG_GLYPH_H, UI_BLACK);
    return;
  }

  display_set_window(x, y, BIG_GLYPH_W, BIG_GLYPH_H);
  display_begin_pixel_stream();

  for(row = 0U; row < BIG_GLYPH_H; row++)
  {
    for(col = 0U; col < BIG_GLYPH_W; col++)
    {
      index = (uint16_t)(row * BIG_GLYPH_STRIDE + (col >> 2));
      alpha = (uint8_t)((big_digits[digit][index] >> ((col & 3U) * 2U)) & 0x03U);
      maindash_alt_line[col] = palette[alpha];
    }

    display_write_pixel_stream(maindash_alt_line, BIG_GLYPH_W);
  }

  display_end_pixel_stream();
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
  display_fill_rect(2U, 0U, 34U, 10U, UI_BLACK);
  display_draw_string(2U, 0U, show_set ? "SET" : "NOW",
                      show_set ? UI_PINK : UI_GRAY, UI_BLACK, 2U);

  if(disabled != 0U)
  {
    display_fill_rect(0U, 10U, DISPLAY_WIDTH, 57U, UI_BLACK);

    blink_on = (uint8_t)(((now_ms / 500U) & 1U) == 0U);
    if(blink_on != 0U)
    {
      display_draw_string(14U, 23U, "DISABLED", UI_GRAY, UI_BLACK, 4U);
    }
  }
  else if(status->temp_valid == 0U)
  {
    display_fill_rect(0U, 10U, DISPLAY_WIDTH, 57U, UI_BLACK);
    display_draw_string(56U, 23U, "---", UI_PINK, UI_BLACK, 4U);
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
    else
    {
      display_fill_rect(9U, 11U, BIG_GLYPH_W, BIG_GLYPH_H, UI_BLACK);
    }

    if((hundreds != 0) || (tens != 0))
    {
      maindash_alt_big_digit(57U, 11U, (uint8_t)tens);
    }
    else
    {
      display_fill_rect(57U, 11U, BIG_GLYPH_W, BIG_GLYPH_H, UI_BLACK);
    }

    maindash_alt_big_digit(105U, 11U, (uint8_t)units);
  }

  /* 左下角信息：报错优先，否则模式/冷端/电压轮换 */
  display_fill_rect(0U, 67U, DISPLAY_WIDTH, 10U, UI_BLACK);

  if(error_text != NULL)
  {
    display_draw_string(2U, 67U, error_text, error_color, UI_BLACK, 2U);
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

    display_draw_string(2U, 67U, info, UI_BLUE, UI_BLACK, 2U);
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


/* ============================ 旧版主界面新排版 ============================ */

static uint16_t maindash_classic_line[UI_GLYPH_W];

static void maindash_classic_temp_digit(uint16_t x, uint16_t y, uint8_t digit)
{
  static const uint16_t palette[4] =
  {
    UI_BLACK, 0x2002U, UI_PINK, UI_PINK
  };
  uint16_t row;
  uint16_t col;
  uint16_t index;
  uint8_t alpha;

  if(digit >= UI_GLYPH_COUNT)
  {
    display_fill_rect(x, y, UI_GLYPH_W, UI_GLYPH_H, UI_BLACK);
    return;
  }

  display_set_window(x, y, UI_GLYPH_W, UI_GLYPH_H);
  display_begin_pixel_stream();

  for(row = 0U; row < UI_GLYPH_H; row++)
  {
    for(col = 0U; col < UI_GLYPH_W; col++)
    {
      index = (uint16_t)(row * UI_GLYPH_STRIDE + (col >> 2));
      alpha = (uint8_t)((ui_digits[digit][index] >> ((col & 3U) * 2U)) & 0x03U);
      maindash_classic_line[col] = palette[alpha];
    }

    display_write_pixel_stream(maindash_classic_line, UI_GLYPH_W);
  }

  display_end_pixel_stream();
}

static void maindash_classic_top_text(uint16_t x, uint16_t y,
                                      const char *text, uint8_t scale,
                                      uint16_t color, uint16_t fill_width)
{
  uint16_t bg;
  uint16_t fg;

  while(*text != '\0')
  {
    bg = (x < fill_width) ? UI_PINK : UI_HEADER;
    fg = (bg == UI_PINK) ? UI_BLACK : color;
    display_draw_char(x, y, *text, fg, bg, scale);
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

  /* 顶栏：整条功率进度 + 功率百分比 */
  fill_width = (uint16_t)(((uint16_t)status->power * 160U) / 100U);

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 12U, UI_HEADER);
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
  display_fill_rect(0U, 13U, 74U, 50U, UI_BLACK);

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
    display_draw_string(13U, 25U, "---", UI_PINK, UI_BLACK, 4U);
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
    else
    {
      display_fill_rect(4U, 18U, UI_GLYPH_W, UI_GLYPH_H, UI_BLACK);
    }

    if((hundreds != 0) || (tens != 0))
    {
      maindash_classic_temp_digit(24U, 18U, (uint8_t)tens);
    }
    else
    {
      display_fill_rect(24U, 18U, UI_GLYPH_W, UI_GLYPH_H, UI_BLACK);
    }

    maindash_classic_temp_digit(44U, 18U, (uint8_t)units);
    display_draw_string(66U, 28U, "C", UI_PINK, UI_BLACK, 2U);
  }

  display_fill_rect(74U, 13U, 1U, 50U, UI_BLUE);

  /* 右侧：一次两行，2 秒换一组 */
  display_fill_rect(75U, 13U, 85U, 50U, UI_BLACK);

  pair = (uint8_t)((now_ms / 2000U) & 1U);

  if(pair == 0U)
  {
    display_draw_string(78U, 20U, "SET", UI_BLUE, UI_BLACK, 1U);
    fmt_snprintf(value, sizeof(value), "%dC",
                 maindash_round_float(status->setpoint_c));
    display_draw_string(104U, 15U, value, UI_WHITE, UI_BLACK, 3U);

    display_fill_rect(78U, 36U, 80U, 1U, UI_DARK);

    display_draw_string(78U, 43U, "MODE", UI_BLUE, UI_BLACK, 1U);
    display_draw_string(104U, 38U, maindash_mode_text(status->mode),
                        UI_WHITE, UI_BLACK, 3U);
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

    display_draw_string(78U, 20U, "VIN", UI_BLUE, UI_BLACK, 1U);
    fmt_snprintf(value, sizeof(value), "%d.%dV",
                 (int)(vin_mv / 1000U), (int)((vin_mv % 1000U) / 100U));
    display_draw_string(104U, 15U, value, UI_WHITE, UI_BLACK, 3U);

    display_fill_rect(78U, 36U, 80U, 1U, UI_DARK);

    display_draw_string(78U, 43U, "COLD", UI_BLUE, UI_BLACK, 1U);
    fmt_snprintf(value, sizeof(value), "%d.%dC", whole, frac);
    display_draw_string(104U, 38U, value, UI_WHITE, UI_BLACK, 3U);
  }

  display_fill_rect(0U, 63U, DISPLAY_WIDTH, 1U, UI_BLUE);

  /* 底部状态 + 休眠遥测 */
  display_fill_rect(0U, 64U, DISPLAY_WIDTH, 13U, UI_BLACK);

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
  display_draw_string(12U, 66U, status_text, status_color, UI_BLACK, 2U);

  {
    uint32_t remaining_ms = power_get_sleep_remaining_ms();
    uint32_t seconds = (remaining_ms + 999U) / 1000U;
    uint8_t vibration = power_get_vibration_state();

    display_draw_string(84U, 66U, "SLEEP", UI_BLUE, UI_BLACK, 1U);
    display_fill_rect(107U, 68U, 4U, 4U,
                      (vibration != 0U) ? UI_PINK : UI_DARK);
    fmt_snprintf(value, sizeof(value), "%lus", (unsigned long)seconds);
    display_draw_string(114U, 66U, value, UI_PINK, UI_BLACK, 1U);
  }

  maindash_last_draw_ms = (now_ms == 0U) ? 1U : now_ms;
}

void maindash_draw(const sm_status_t *status)
{
  if(status == NULL)
  {
    return;
  }

  if(g_eeprom_params.maindash_alt != 0U)
  {
    display_clear(UI_BLACK);
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
    maindash_alt_draw(&status, now_ms);
    return 1U;
  }

  if((maindash_last_draw_ms != 0U) &&
     ((uint32_t)(now_ms - maindash_last_draw_ms) < 250U))
  {
    return 0U;
  }

  sm_get_status(&status);
  maindash_classic_draw(&status, now_ms);
  return 1U;
}
