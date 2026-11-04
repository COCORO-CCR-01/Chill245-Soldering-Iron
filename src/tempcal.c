/**
  **************************************************************************
  * @file     tempcal.c
  * @brief    C245 热端 7 点校准（调试串口 + 屏幕）
  **************************************************************************
  * 流程：100/150/200/250/300/350/400°C 七个目标点。
  *   1. 自动设定目标并等待温度稳定 3 s
  *   2. 用消隐后的 ADC 平均 1 s
  *   3. 提示用户输入外部温度计实测值
  *   4. 七点完成后生成 10 点 ADC->温度表，启用并写入 EEPROM
  **************************************************************************
  */

#include "tempcal.h"
#include "fmt.h"
#include "onekeyboost.h"
#include "temp.h"
#include "statemachine.h"
#include "powermanagement.h"
#include "hms.h"
#include "eeprom.h"
#include "buttons.h"
#include "display.h"
#include "ui_icons.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEMPCAL_BLACK       (0x0000U)
#define TEMPCAL_WHITE       (0xFFFFU)
#define TEMPCAL_RED         (0xF800U)
#define TEMPCAL_GREEN       (0x07E0U)
#define TEMPCAL_YELLOW      (0xFFE0U)
#define TEMPCAL_GRAY        (0x8410U)
#define TEMPCAL_DARK        (0x2104U)
#define TEMPCAL_BAR         (0x0010U)
#define TEMPCAL_CYAN        (0x07FFU)

#define TEMPCAL_POINTS              (7U)
#define TEMPCAL_STABLE_MS           (3000U)
#define TEMPCAL_SAMPLE_MS           (1000U)
#define TEMPCAL_DONE_MS             (2500U)
#define TEMPCAL_TIMEOUT_MS          (300000U)
#define TEMPCAL_STABLE_WINDOW_C     (3.0f)
#define TEMPCAL_SAMPLE_WINDOW_C     (4.0f)
#define TEMPCAL_ACTUAL_MIN_C        (0.0f)
#define TEMPCAL_ACTUAL_MAX_C        (600.0f)

/* newlib-nano 未链接浮点 printf，使用整数缩放格式化 */
static void tempcal_format_c(char *text, uint16_t size, float value,
                             uint8_t decimals)
{
  int32_t scaled;
  uint32_t abs_scaled;
  uint32_t whole;
  uint32_t frac;
  const char *sign;

  if((text == NULL) || (size == 0U))
  {
    return;
  }

  if(decimals == 0U)
  {
    scaled = (int32_t)(value + ((value >= 0.0f) ? 0.5f : -0.5f));
    fmt_snprintf(text, size, "%ldC", (long)scaled);
    return;
  }

  scaled = (int32_t)((value * 10.0f) +
                     ((value >= 0.0f) ? 0.5f : -0.5f));
  sign = (scaled < 0) ? "-" : "";
  abs_scaled = (scaled < 0) ? (uint32_t)(-scaled) : (uint32_t)scaled;
  whole = abs_scaled / 10U;
  frac = abs_scaled % 10U;
  fmt_snprintf(text, size, "%s%lu.%luC", sign, (unsigned long)whole,
           (unsigned long)frac);
}

typedef enum
{
  TEMPCAL_STATE_IDLE = 0,
  TEMPCAL_STATE_SETTLE,
  TEMPCAL_STATE_SAMPLE,
  TEMPCAL_STATE_MEASURE,
  TEMPCAL_STATE_DONE,
  TEMPCAL_STATE_ERROR
} tempcal_state_t;

static const float tempcal_targets[TEMPCAL_POINTS] =
{
  100.0f, 150.0f, 200.0f, 250.0f, 300.0f, 350.0f, 400.0f
};

static tempcal_state_t tempcal_state;
static uint8_t tempcal_index;
static uint8_t tempcal_need_draw;
static uint32_t tempcal_state_ms;
static uint32_t tempcal_settle_ms;
static uint32_t tempcal_last_draw_ms;
static uint32_t tempcal_sample_sum;
static uint32_t tempcal_sample_count;
static float tempcal_cold_sum;
static uint16_t tempcal_adc_avg;
static float tempcal_cold_avg;
static float tempcal_actual_c;
static float tempcal_orig_setpoint_c;
static pid_mode_t tempcal_orig_mode;
static uint8_t tempcal_orig_heater_enabled;

static uint16_t tempcal_adc[TEMPCAL_POINTS];
static float tempcal_actual[TEMPCAL_POINTS];
static float tempcal_cold[TEMPCAL_POINTS];

static uint8_t tempcal_initialized;

static float tempcal_absf(float value)
{
  return (value < 0.0f) ? -value : value;
}

static float tempcal_clampf(float value, float min_value, float max_value)
{
  if(value < min_value)
  {
    return min_value;
  }

  if(value > max_value)
  {
    return max_value;
  }

  return value;
}

static void tempcal_restore_runtime(void)
{
  (void)sm_set_setpoint(tempcal_orig_setpoint_c);
  (void)sm_set_pid_mode(tempcal_orig_mode);

  if(tempcal_orig_heater_enabled == 0U)
  {
    (void)sm_set_heater_enabled(0U);
  }
}

static void tempcal_fail(void)
{
  tempcal_restore_runtime();
  tempcal_state = TEMPCAL_STATE_ERROR;
  tempcal_state_ms = sm_get_uptime_ms();
  tempcal_need_draw = 1U;
}

static uint8_t tempcal_build_curve(void)
{
  temp_tc_cal_point_t points[TEMP_TC_CAL_POINTS];
  float temp_at_ref[TEMPCAL_POINTS];
  float cal_ref = 0.0f;
  float default_first;
  float default_last;
  float low_offset;
  float high_offset;

  for(uint8_t i = 0U; i < TEMPCAL_POINTS; i++)
  {
    if((i > 0U) && (tempcal_adc[i] <= tempcal_adc[i - 1U]))
    {
      return 0U;
    }

    cal_ref += tempcal_cold[i];
  }

  cal_ref /= (float)TEMPCAL_POINTS;

  for(uint8_t i = 0U; i < TEMPCAL_POINTS; i++)
  {
    temp_at_ref[i] = tempcal_actual[i] + (cal_ref - tempcal_cold[i]);
    temp_at_ref[i] = tempcal_clampf(temp_at_ref[i], -50.0f, 600.0f);
  }

  default_first = temp_thermocouple_default_celsius(tempcal_adc[0], cal_ref);
  default_last = temp_thermocouple_default_celsius(
    tempcal_adc[TEMPCAL_POINTS - 1U], cal_ref);

  low_offset = temp_at_ref[0] - default_first;
  high_offset = temp_at_ref[TEMPCAL_POINTS - 1U] - default_last;

  for(uint8_t j = 0U; j < TEMP_TC_CAL_POINTS; j++)
  {
    uint16_t adc = (uint16_t)(((uint32_t)j * 4095U) /
                              (TEMP_TC_CAL_POINTS - 1U));
    float temp_c;

    if(adc <= tempcal_adc[0])
    {
      temp_c = temp_thermocouple_default_celsius(adc, cal_ref) + low_offset;
    }
    else if(adc >= tempcal_adc[TEMPCAL_POINTS - 1U])
    {
      temp_c = temp_thermocouple_default_celsius(adc, cal_ref) + high_offset;
    }
    else
    {
      uint8_t segment = 0U;

      while((segment + 1U) < TEMPCAL_POINTS)
      {
        if(adc <= tempcal_adc[segment + 1U])
        {
          break;
        }
        segment++;
      }

      {
        uint16_t adc0 = tempcal_adc[segment];
        uint16_t adc1 = tempcal_adc[segment + 1U];

        if(adc1 == adc0)
        {
          temp_c = temp_at_ref[segment];
        }
        else
        {
          temp_c = temp_at_ref[segment] +
            (((float)(adc - adc0) / (float)(adc1 - adc0)) *
             (temp_at_ref[segment + 1U] - temp_at_ref[segment]));
        }
      }
    }

    temp_c = tempcal_clampf(temp_c, -50.0f, 600.0f);
    points[j].adc = adc;
    points[j].temp_c = temp_c;
  }

  if(temp_set_cal_curve(points, 1U, cal_ref) == 0U)
  {
    return 0U;
  }

  return 1U;
}

static void tempcal_finish(void)
{
  tempcal_restore_runtime();

  if(tempcal_build_curve() == 0U)
  {
    tempcal_fail();
    return;
  }

  eeprom_capture_current();

  if(eeprom_save() == 0U)
  {
    tempcal_fail();
    return;
  }

  tempcal_state = TEMPCAL_STATE_DONE;
  tempcal_state_ms = sm_get_uptime_ms();
  tempcal_need_draw = 1U;
}

static void tempcal_draw_bar(void)
{
  for(uint8_t i = 0U; i < TEMPCAL_POINTS; i++)
  {
    uint16_t color = TEMPCAL_DARK;

    if(i < tempcal_index)
    {
      color = TEMPCAL_GREEN;
    }
    else if(i == tempcal_index)
    {
      color = TEMPCAL_YELLOW;
    }

    display_fill_rect((uint16_t)(3U + (i * 22U)), 72U, 20U, 3U, color);
  }
}

static void tempcal_draw(void)
{
  sm_status_t status;
  char text[24];

  sm_get_status(&status);

  display_clear(TEMPCAL_BLACK);
  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 14U, TEMPCAL_BAR);
  ui_icon_draw(UI_ICON_THERMO, 3U, 3U, TEMPCAL_WHITE);
  display_draw_string(15U, 2U, "TIP CAL", TEMPCAL_WHITE, TEMPCAL_BAR, 2U);
  fmt_snprintf(text, sizeof(text), "%u/%u",
           (unsigned)(tempcal_index + 1U), (unsigned)TEMPCAL_POINTS);
  display_draw_string((uint16_t)(DISPLAY_WIDTH - display_string_width(text, 1U) - 3U),
                      4U, text, TEMPCAL_CYAN, TEMPCAL_BAR, 1U);

  if(tempcal_state == TEMPCAL_STATE_ERROR)
  {
    ui_icon_draw(UI_ICON_WARN, 4U, 22U, TEMPCAL_RED);
    display_draw_string(16U, 22U, "CAL ERROR", TEMPCAL_RED, TEMPCAL_BLACK, 2U);
    display_draw_string(4U, 50U, "CHECK FAULT / EEPROM", TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
    return;
  }

  if(tempcal_state == TEMPCAL_STATE_DONE)
  {
    ui_icon_draw(UI_ICON_CHECK, 4U, 22U, TEMPCAL_GREEN);
    display_draw_string(16U, 22U, "CAL SAVED", TEMPCAL_GREEN, TEMPCAL_BLACK, 2U);
    display_draw_string(4U, 50U, "RETURNING TO MAIN", TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
    return;
  }

  display_draw_string(3U, 17U, "TARGET", TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
  tempcal_format_c(text, sizeof(text), tempcal_targets[tempcal_index], 0U);
  display_draw_string(58U, 15U, text, TEMPCAL_WHITE, TEMPCAL_BLACK, 2U);

  display_draw_string(3U, 33U, "TIP", TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
  tempcal_format_c(text, sizeof(text), status.tip_c, 1U);
  display_draw_string(58U, 31U, text, TEMPCAL_CYAN, TEMPCAL_BLACK, 2U);

  if(tempcal_state == TEMPCAL_STATE_MEASURE)
  {
    display_draw_string(3U, 48U, "ACTUAL", TEMPCAL_YELLOW, TEMPCAL_BLACK, 1U);
    tempcal_format_c(text, sizeof(text), tempcal_actual_c, 1U);
    display_draw_string(58U, 47U, text, TEMPCAL_YELLOW, TEMPCAL_BLACK, 2U);
    display_draw_string(2U, 64U, "PREV/NEXT ADJ  MENU OK",
                        TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
  }
  else if(tempcal_state == TEMPCAL_STATE_SAMPLE)
  {
    display_draw_string(3U, 48U, "STABLE, SAMPLING ADC", TEMPCAL_GREEN, TEMPCAL_BLACK, 1U);
    display_draw_string(3U, 62U, "PLEASE WAIT...", TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
  }
  else
  {
    display_draw_string(3U, 48U, "HEATING, WAIT STABLE", TEMPCAL_YELLOW, TEMPCAL_BLACK, 1U);
    display_draw_string(3U, 62U, "LONG MENU = CANCEL", TEMPCAL_GRAY, TEMPCAL_BLACK, 1U);
  }

  tempcal_draw_bar();
}

uint8_t tempcal_start(void)
{
  sm_status_t status;

  if((tempcal_initialized == 0U) ||
     (tempcal_state != TEMPCAL_STATE_IDLE))
  {
    return 0U;
  }

  if(hms_is_active() != 0U)
  {
    return 0U;
  }

  if(onekeyboost_is_active() != 0U)
  {
    onekeyboost_cancel();
  }

  sm_get_status(&status);

  if((status.autotune_state == PID_AUTOTUNE_RUNNING) ||
     (status.fault != VERIFY_HEATER_FAULT_NONE) ||
     (status.state == SM_STATE_FAULT) ||
     (power_get_fault() != POWER_FAULT_NONE) ||
     (power_get_state() != POWER_STATE_ACTIVE))
  {
    return 0U;
  }

  tempcal_orig_setpoint_c = sm_get_setpoint();
  tempcal_orig_mode = sm_get_pid_mode();
  tempcal_orig_heater_enabled = sm_is_heater_enabled();

  if(tempcal_orig_heater_enabled == 0U)
  {
    if(sm_set_heater_enabled(1U) == 0U)
    {
      return 0U;
    }
  }

  (void)sm_set_pid_mode(PID_MODE_STANDARD);
  (void)sm_set_setpoint(tempcal_targets[0]);

  tempcal_index = 0U;
  tempcal_actual_c = tempcal_targets[0];
  tempcal_settle_ms = 0U;
  tempcal_sample_sum = 0U;
  tempcal_sample_count = 0U;
  tempcal_cold_sum = 0.0f;
  tempcal_state = TEMPCAL_STATE_SETTLE;
  tempcal_state_ms = sm_get_uptime_ms();
  tempcal_need_draw = 1U;


  return 1U;
}

void tempcal_cancel(void)
{
  if(tempcal_state == TEMPCAL_STATE_IDLE)
  {
    return;
  }

  tempcal_restore_runtime();
  tempcal_state = TEMPCAL_STATE_IDLE;
  tempcal_need_draw = 0U;
}

uint8_t tempcal_is_active(void)
{
  return (tempcal_state != TEMPCAL_STATE_IDLE) ? 1U : 0U;
}

void tempcal_adjust_actual(float delta_c)
{
  if(tempcal_state != TEMPCAL_STATE_MEASURE)
  {
    return;
  }

  tempcal_actual_c = tempcal_clampf(tempcal_actual_c + delta_c,
                                    TEMPCAL_ACTUAL_MIN_C,
                                    TEMPCAL_ACTUAL_MAX_C);
  tempcal_need_draw = 1U;
}

uint8_t tempcal_submit_actual(float actual_c)
{
  if(tempcal_state != TEMPCAL_STATE_MEASURE)
  {
    return 0U;
  }

  if((actual_c < TEMPCAL_ACTUAL_MIN_C) ||
     (actual_c > TEMPCAL_ACTUAL_MAX_C))
  {
    return 0U;
  }

  tempcal_adc[tempcal_index] = tempcal_adc_avg;
  tempcal_actual[tempcal_index] = actual_c;
  tempcal_cold[tempcal_index] = tempcal_cold_avg;


  tempcal_index++;

  if(tempcal_index >= TEMPCAL_POINTS)
  {
    tempcal_finish();
    return 1U;
  }

  (void)sm_set_setpoint(tempcal_targets[tempcal_index]);
  tempcal_actual_c = tempcal_targets[tempcal_index];
  tempcal_settle_ms = 0U;
  tempcal_state = TEMPCAL_STATE_SETTLE;
  tempcal_state_ms = sm_get_uptime_ms();
  tempcal_need_draw = 1U;

  return 1U;
}

void tempcal_task(uint32_t now_ms)
{
  sm_status_t status;
  float diff;

  if(tempcal_state == TEMPCAL_STATE_IDLE)
  {
    return;
  }

  if((tempcal_state == TEMPCAL_STATE_DONE) ||
     (tempcal_state == TEMPCAL_STATE_ERROR))
  {
    if((uint32_t)(now_ms - tempcal_state_ms) >= TEMPCAL_DONE_MS)
    {
      tempcal_state = TEMPCAL_STATE_IDLE;
      tempcal_need_draw = 0U;
    }
    else if((tempcal_need_draw != 0U) ||
            ((uint32_t)(now_ms - tempcal_last_draw_ms) >= 500U))
    {
      tempcal_draw();
      tempcal_last_draw_ms = now_ms;
      tempcal_need_draw = 0U;
    }
    return;
  }

  if(hms_has_fatal() != 0U)
  {
    tempcal_cancel();
    return;
  }

  sm_get_status(&status);

  if((status.fault != VERIFY_HEATER_FAULT_NONE) ||
     (status.state == SM_STATE_FAULT) ||
     (power_get_fault() != POWER_FAULT_NONE) ||
     (status.autotune_state == PID_AUTOTUNE_RUNNING))
  {
    tempcal_fail();
  }
  else if((uint32_t)(now_ms - tempcal_state_ms) >= TEMPCAL_TIMEOUT_MS)
  {
    tempcal_fail();
  }
  else
  {
    switch(tempcal_state)
    {
      case TEMPCAL_STATE_SETTLE:
        if(status.temp_valid == 0U)
        {
          tempcal_settle_ms = 0U;
          break;
        }

        diff = tempcal_absf(status.tip_c - tempcal_targets[tempcal_index]);

        if(diff <= TEMPCAL_STABLE_WINDOW_C)
        {
          if(tempcal_settle_ms == 0U)
          {
            tempcal_settle_ms = now_ms;
          }
          else if((uint32_t)(now_ms - tempcal_settle_ms) >= TEMPCAL_STABLE_MS)
          {
            tempcal_sample_sum = 0U;
            tempcal_sample_count = 0U;
            tempcal_cold_sum = 0.0f;
            tempcal_state = TEMPCAL_STATE_SAMPLE;
            tempcal_state_ms = now_ms;
            tempcal_need_draw = 1U;
                    }
        }
        else
        {
          tempcal_settle_ms = 0U;
        }
        break;

      case TEMPCAL_STATE_SAMPLE:
        diff = tempcal_absf(status.tip_c - tempcal_targets[tempcal_index]);

        if(diff > TEMPCAL_SAMPLE_WINDOW_C)
        {
          tempcal_settle_ms = 0U;
          tempcal_state = TEMPCAL_STATE_SETTLE;
          tempcal_state_ms = now_ms;
          tempcal_need_draw = 1U;
          break;
        }

        tempcal_sample_sum += status.tc_raw;
        tempcal_sample_count++;
        tempcal_cold_sum += status.room_c;

        if((uint32_t)(now_ms - tempcal_state_ms) >= TEMPCAL_SAMPLE_MS)
        {
          if(tempcal_sample_count == 0U)
          {
            tempcal_fail();
            break;
          }

          tempcal_adc_avg = (uint16_t)(tempcal_sample_sum /
                                       tempcal_sample_count);
          tempcal_cold_avg = tempcal_cold_sum /
                             (float)tempcal_sample_count;
          tempcal_state = TEMPCAL_STATE_MEASURE;
          tempcal_state_ms = now_ms;
          tempcal_actual_c = tempcal_targets[tempcal_index];
          tempcal_need_draw = 1U;

        }
        break;

      case TEMPCAL_STATE_MEASURE:
        break;

      default:
        break;
    }
  }

  if((tempcal_need_draw != 0U) ||
     ((uint32_t)(now_ms - tempcal_last_draw_ms) >= 500U))
  {
    tempcal_draw();
    tempcal_last_draw_ms = now_ms;
    tempcal_need_draw = 0U;
  }
}

static void tempcal_on_button(button_id_t button, button_event_t event)
{
  if(tempcal_state == TEMPCAL_STATE_IDLE)
  {
    return;
  }

  if((button == BUTTON_MENU) && (event == BUTTON_EVENT_LONG_PRESS))
  {
    tempcal_cancel();
    return;
  }

  if(tempcal_state != TEMPCAL_STATE_MEASURE)
  {
    return;
  }

  if((button == BUTTON_MENU) && (event == BUTTON_EVENT_CLICK))
  {
    (void)tempcal_submit_actual(tempcal_actual_c);
  }
  else if((button == BUTTON_PREV) && (event == BUTTON_EVENT_CLICK))
  {
    tempcal_adjust_actual(-0.5f);
  }
  else if((button == BUTTON_NEXT) && (event == BUTTON_EVENT_CLICK))
  {
    tempcal_adjust_actual(0.5f);
  }
}

void tempcal_init(void)
{
  if(tempcal_initialized != 0U)
  {
    return;
  }

  tempcal_state = TEMPCAL_STATE_IDLE;
  tempcal_index = 0U;
  tempcal_need_draw = 0U;
  tempcal_actual_c = TEMPCAL_ACTUAL_MIN_C;
  tempcal_initialized = 1U;

  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_CLICK,
                                  tempcal_on_button);
  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_LONG_PRESS,
                                  tempcal_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_CLICK,
                                  tempcal_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_CLICK,
                                  tempcal_on_button);

}
