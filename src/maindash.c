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

static int maindash_round_float(float value)
{
  if(value >= 0.0f)
  {
    return (int)(value + 0.5f);
  }

  return (int)(value - 0.5f);
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


static const char *maindash_status_text(const sm_status_t *status, uint16_t *color)
{
  float diff;
  power_fault_t power_fault = power_get_fault();

  if(power_fault != POWER_FAULT_NONE)
  {
    *color = UI_RED;
    return "ERR";
  }

  if(status->fault != VERIFY_HEATER_FAULT_NONE)
  {
    *color = UI_RED;
    return "ERR";
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
      return "ERR";

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

static uint32_t maindash_last_draw_ms;

void maindash_init(void)
{
  maindash_last_draw_ms = 0U;
}

#define MAINDASH_DRAW_STEPS (7U)

static uint8_t maindash_force_full;

static uint32_t maindash_setpoint_hint_until_ms;


void maindash_notify_setpoint_adjusted(void)
{
  uint32_t now_ms = sm_get_uptime_ms();

  maindash_setpoint_hint_until_ms = now_ms + 2000U;
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
  show_set = ((int32_t)(maindash_setpoint_hint_until_ms - now_ms) > 0) ? 1U : 0U;
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
static const char *cl_last_top;
static const char *cl_last_status;
static int cl_last_seconds = -1;
static uint8_t cl_last_vibration;
/* 右侧轮播块上一次"真正画上去的内容"：页码 + 两个值的整数形式，见
 * maindash_classic_right_signature()。以前没有可比对的东西，只能每 500ms 无脑重画
 * 整块，这就是右边必然闪烁的直接原因。 */
static uint8_t cl_last_right_pair;
static int cl_last_right_a = -1;
static int cl_last_right_b = -1;
/* 上一拍读到、还没确认的候选值。VIN / 冷端是 10ms 刷新的瞬时 ADC 值，没有滤波，
 * 加热时输入端约 2.5V 峰峰的纹波会把 0.1V / 0.1C 那一位来回推过边界，签名每拍都
 * 变就等于整块 4Hz 重画，还是闪。改成"同一个显示值连续两拍读到"才认它是真变了，
 * 来回抖动的值永远过不了这道闸。 */
static int cl_pend_right_a = -1;
static int cl_pend_right_b = -1;

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

  show_set = ((int32_t)(maindash_setpoint_hint_until_ms - now_ms) > 0) ? 1U : 0U;
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

/* 右侧轮播块"这一屏要显示成什么样"的整数签名，只拿来做脏判断。
 * 取值必须和 maindash_classic_right_draw() 画上去的内容严格一致：
 *   pair 0（SET / MODE）：a = 设定温度取整到 1C，b = PID 模式档位
 *   pair 1（VIN / COLD）：a = 输入电压截断到 0.1V（和显示的 %d.%dV 一致），
 *                         b = 冷端温度取整到 0.1C（和显示的 %d.%dC 一致）
 * 签名相同就说明屏上内容一模一样，整块一个像素都不用动。 */
static void maindash_classic_right_signature(const sm_status_t *status, uint8_t pair,
                                             int *a, int *b)
{
  if(pair == 0U)
  {
    *a = maindash_round_float(status->setpoint_c);
    *b = (int)status->mode;
  }
  else
  {
    *a = (int)(power_get_voltage_mv() / 100U);
    *b = maindash_round_float(status->room_c * 10.0f);
  }
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
  int right_a;
  int right_b;
  /* 必须在函数开头就声明并清零：整页重绘那条路是 goto cache 收尾，声明放晚了
     cache 段读到的是没初始化的值 */
  uint8_t right_dirty = 0U;

  /* 先把这一帧"要显示成什么样"全部算出来，再决定哪一块需要画。这段必须放在整页
   * 重绘分支之前：那个分支用 goto cache 收尾，算晚了存进缓存的就是没初始化的值
   * （原来就是这个顺序，整页重绘之后 cl_last_pair 那几个缓存拿到的是栈上的垃圾）。 */
  show_set = ((int32_t)(maindash_setpoint_hint_until_ms - now_ms) > 0) ? 1U : 0U;
  tip_c = show_set ? maindash_round_float(status->setpoint_c) :
          ((status->temp_valid != 0U) ? maindash_round_float(status->tip_c) : 0);
  if(tip_c < 0) tip_c = 0;
  if(tip_c > 999) tip_c = 999;
  pair = (uint8_t)((now_ms / 2000U) & 1U);
  top_state = maindash_top_state(status, &top_color);
  status_text = maindash_status_text(status, &status_color);
  seconds = (power_get_sleep_remaining_ms() + 999U) / 1000U;
  vibration = power_get_vibration_state();
  maindash_classic_right_signature(status, pair, &right_a, &right_b);

  if(maindash_force_full != 0U)
  {
    maindash_force_full = 0U;
    maindash_classic_draw(status, now_ms);
    /* 整页重绘已经把当前值画上去了，缓存照实记，别等下一帧的脏判断去猜 */
    cl_last_right_a = right_a;
    cl_last_right_b = right_b;
    cl_pend_right_a = right_a;
    cl_pend_right_b = right_b;
    goto cache;
  }

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
  /* 顶栏内容是功率条 + Chill245 + 状态字 + 百分比，和右侧轮播的页码无关。原来这里
   * || 了一个 (pair != cl_last_pair)，等于每 2 秒白刷一整条顶栏（wallpaper_blit
   * 重铺 160x12 + 三段文字），也是闪烁来源之一，去掉；功率百分比变化上面已经会标
   * CL_DIRTY_TOP，不会漏掉。 */
  if(top_state != cl_last_top)
  {
    mask |= CL_DIRTY_TOP;
  }
  if((status_text != cl_last_status) ||
     ((int)seconds != cl_last_seconds) ||
     (vibration != cl_last_vibration))
  {
    mask |= CL_DIRTY_BOTTOM;
  }
  /* 右侧轮播块改成"值变了才画"：页码变了，或者这一屏真正要显示的两个值变了，才标脏。
   * 原来这里没有任何值判断，(now_ms - cl_last_right_ms) >= 500 就重画整块 —— 
   * wallpaper_blit 重铺 84x49 背景 + 6 段文字 + 一条分隔线，2Hz 的整块擦掉重画，
   * 就是用户反馈"右边必然闪"的直接原因。签名和画上去的内容严格一致（VIN 精确到
   * 0.1V、冷端 0.1C、设定温度 1C、MODE 档位），这些真变了照样立刻刷新，
   * 没变就一个像素都不动。 */
  if(pair != cl_last_right_pair)
  {
    /* 2 秒轮播换页，两屏内容完全不同，整块重画是必须的 */
    right_dirty = 1U;
  }
  else if((right_a != cl_last_right_a) || (right_b != cl_last_right_b))
  {
    if((right_a == cl_pend_right_a) && (right_b == cl_pend_right_b))
    {
      /* 上一拍也是这个值，认定是真的变了 */
      right_dirty = 1U;
    }
    else
    {
      /* 先记下候选值，下一拍再确认，专门挡 VIN / 冷端最低位的来回抖动 */
      cl_pend_right_a = right_a;
      cl_pend_right_b = right_b;
    }
  }

  if(right_dirty != 0U)
  {
    mask |= CL_DIRTY_RIGHT;
    cl_pend_right_a = right_a;
    cl_pend_right_b = right_b;
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
  cl_last_right_pair = pair;
  /* cl_last_right_a/b 记的是"屏上此刻画的是什么"，只有真的重画过才更新：无条件更新
   * 会让防抖把自己骗过去（记了新值，屏上还是旧的，之后就再也不画了）。
   * 整页重绘那条 goto 已经在重画后单独赋过值。 */
  if(right_dirty != 0U)
  {
    cl_last_right_a = right_a;
    cl_last_right_b = right_b;
  }
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

  maindash_classic_draw(status, sm_get_uptime_ms());
}

uint8_t maindash_update(uint32_t now_ms)
{
  sm_status_t status;

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
