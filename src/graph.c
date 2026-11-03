/**
  **************************************************************************
  * @file     graph.c
  * @brief    160x80 横屏曲线图
  **************************************************************************
  * 历史窗口：80 点，250 ms/点 = 20 秒。
  *
  * 图表：
  *   TEMP/PWR  热端温度（红）+ 功率占比（绿）
  *   VOLTAGE   输入电压（青）
  *   COLD      冷端温度（黄）
  **************************************************************************
  */

#include "graph.h"
#include "display.h"
#include "statemachine.h"
#include "powermanagement.h"
#include <stdio.h>
#include <string.h>

#define GX0                             (32U)
#define GX1                             (156U)
#define GY0                             (18U)
#define GY1                             (66U)
#define GW                              (GX1 - GX0)
#define GH                              (GY1 - GY0)

#define UI_BLACK                        (0x0000U)
#define UI_WHITE                        (0xFFFFU)
#define UI_GRAY                         (0x8410U)
#define UI_DARK                         (0x4208U)
#define UI_RED                          (0xF800U)
#define UI_GREEN                        (0x07E0U)
#define UI_CYAN                         (0x07FFU)
#define UI_YELLOW                       (0xFFE0U)

static int16_t graph_temp_x10[GRAPH_POINTS];
static int16_t graph_power_pct[GRAPH_POINTS];
static int16_t graph_vin_cv[GRAPH_POINTS];
static int16_t graph_cold_x10[GRAPH_POINTS];

static uint16_t graph_head;
static uint16_t graph_count;
static uint32_t graph_last_sample_ms;

static int16_t graph_round_x10(float value)
{
  if(value >= 0.0f)
  {
    return (int16_t)((value * 10.0f) + 0.5f);
  }

  return (int16_t)((value * 10.0f) - 0.5f);
}

static uint16_t graph_index(uint16_t offset)
{
  return (uint16_t)((graph_head + GRAPH_POINTS - graph_count + offset) % GRAPH_POINTS);
}

static uint16_t graph_x(uint16_t offset)
{
  uint32_t x = (uint32_t)(GRAPH_POINTS - graph_count + offset);
  x = (x * GW) / (GRAPH_POINTS - 1U);
  return (uint16_t)(GX0 + x);
}

static uint16_t graph_y(int32_t value, int32_t min_value, int32_t max_value)
{
  int32_t y;

  if(max_value <= min_value)
  {
    return GY1;
  }

  if(value < min_value)
  {
    value = min_value;
  }

  if(value > max_value)
  {
    value = max_value;
  }

  y = GY1 - ((value - min_value) * (int32_t)GH) / (max_value - min_value);

  if(y < (int32_t)GY0)
  {
    y = GY0;
  }

  if(y > (int32_t)GY1)
  {
    y = GY1;
  }

  return (uint16_t)y;
}

static void graph_draw_frame(const char *title, const char *subtitle)
{
  uint16_t i;
  uint16_t x;
  uint16_t y;
  uint16_t subtitle_x;

  display_draw_string(2U, 1U, title, UI_WHITE, UI_BLACK, 2U);

  if(subtitle != NULL)
  {
    subtitle_x = (uint16_t)(DISPLAY_WIDTH -
                            display_string_width(subtitle, 1U) - 2U);
    display_draw_string(subtitle_x, 2U, subtitle, UI_GRAY, UI_BLACK, 1U);
  }

  display_draw_rect(GX0, GY0, (uint16_t)(GW + 1U), (uint16_t)(GH + 1U), UI_GRAY);

  for(i = 1U; i < 4U; i++)
  {
    y = (uint16_t)(GY0 + ((GH * i) / 4U));
    display_draw_hline(GX0, y, (uint16_t)(GW + 1U), UI_DARK);

    x = (uint16_t)(GX0 + ((GW * i) / 4U));
    display_draw_vline(x, GY0, (uint16_t)(GH + 1U), UI_DARK);
  }

  display_draw_string(GX0, (uint16_t)(GY1 + 3U), "20S", UI_GRAY, UI_BLACK, 1U);
}

static void graph_draw_y_label(int32_t value, uint16_t y, char suffix)
{
  char text[16];

  if(suffix != '\0')
  {
    snprintf(text, sizeof(text), "%d%c", (int)value, suffix);
  }
  else
  {
    snprintf(text, sizeof(text), "%d", (int)value);
  }

  display_draw_string(0U, y, text, UI_GRAY, UI_BLACK, 1U);
}

static void graph_draw_series(const int16_t *values,
                              int32_t min_value, int32_t max_value,
                              uint16_t color)
{
  uint16_t i;
  uint16_t index;
  uint16_t x;
  uint16_t y;
  uint16_t prev_x = 0U;
  uint16_t prev_y = 0U;
  uint8_t have_prev = 0U;

  if(graph_count == 0U)
  {
    return;
  }

  for(i = 0U; i < graph_count; i++)
  {
    index = graph_index(i);
    x = graph_x(i);
    y = graph_y(values[index], min_value, max_value);

    if(have_prev != 0U)
    {
      display_draw_line((int16_t)prev_x, (int16_t)prev_y,
                        (int16_t)x, (int16_t)y, color);
    }
    else
    {
      display_draw_pixel(x, y, color);
      have_prev = 1U;
    }

    prev_x = x;
    prev_y = y;
  }
}

static void graph_range_i16(const int16_t *values,
                            int32_t *min_value, int32_t *max_value,
                            int32_t min_span)
{
  uint16_t i;
  uint16_t index;
  int32_t lo = 32767;
  int32_t hi = -32768;
  int32_t mid;
  int32_t pad;

  if(graph_count == 0U)
  {
    *min_value = 0;
    *max_value = min_span;
    return;
  }

  for(i = 0U; i < graph_count; i++)
  {
    index = graph_index(i);

    if(values[index] < lo)
    {
      lo = values[index];
    }

    if(values[index] > hi)
    {
      hi = values[index];
    }
  }

  if((hi - lo) < min_span)
  {
    mid = (hi + lo) / 2;
    lo = mid - (min_span / 2);
    hi = lo + min_span;
  }

  pad = (hi - lo) / 10;

  if(pad < 1)
  {
    pad = 1;
  }

  *min_value = lo - pad;
  *max_value = hi + pad;
}

void graph_init(void)
{
  memset(graph_temp_x10, 0, sizeof(graph_temp_x10));
  memset(graph_power_pct, 0, sizeof(graph_power_pct));
  memset(graph_vin_cv, 0, sizeof(graph_vin_cv));
  memset(graph_cold_x10, 0, sizeof(graph_cold_x10));

  graph_head = 0U;
  graph_count = 0U;
  graph_last_sample_ms = 0U;
}

uint8_t graph_task(uint32_t now_ms)
{
  sm_status_t status;

  if((graph_count != 0U) &&
     ((uint32_t)(now_ms - graph_last_sample_ms) < GRAPH_SAMPLE_MS))
  {
    return 0U;
  }

  sm_get_status(&status);

  graph_temp_x10[graph_head] = graph_round_x10(status.tip_c);
  graph_power_pct[graph_head] = (int16_t)status.power;
  graph_vin_cv[graph_head] = (int16_t)(power_get_voltage_mv() / 10U);
  graph_cold_x10[graph_head] = graph_round_x10(status.room_c);

  graph_head = (uint16_t)((graph_head + 1U) % GRAPH_POINTS);

  if(graph_count < GRAPH_POINTS)
  {
    graph_count++;
  }

  graph_last_sample_ms = now_ms;
  return 1U;
}

void graph_draw_temp_power(void)
{
  display_clear(UI_BLACK);
  graph_draw_frame("TEMP/PWR", "T450C P100%");

  graph_draw_y_label(450, (uint16_t)(GY0 - 5U), '\0');
  graph_draw_y_label(0, (uint16_t)(GY1 - 5U), '\0');

  graph_draw_series(graph_temp_x10, 0, 4500, UI_RED);
  graph_draw_series(graph_power_pct, 0, 100, UI_GREEN);
}

void graph_draw_voltage(void)
{
  int32_t min_value;
  int32_t max_value;
  char subtitle[24];

  graph_range_i16(graph_vin_cv, &min_value, &max_value, 500);

  if(min_value < 0)
  {
    min_value = 0;
  }

  display_clear(UI_BLACK);

  snprintf(subtitle, sizeof(subtitle), "%d-%dV",
           (int)(min_value / 100), (int)(max_value / 100));
  graph_draw_frame("VOLTAGE", subtitle);

  graph_draw_y_label(max_value / 100, (uint16_t)(GY0 - 5U), 'V');
  graph_draw_y_label(min_value / 100, (uint16_t)(GY1 - 5U), 'V');

  graph_draw_series(graph_vin_cv, min_value, max_value, UI_CYAN);
}

void graph_draw_cold(void)
{
  int32_t min_value;
  int32_t max_value;

  graph_range_i16(graph_cold_x10, &min_value, &max_value, 20);

  display_clear(UI_BLACK);
  graph_draw_frame("COLD", "JUNCTION");

  graph_draw_y_label(max_value / 10, (uint16_t)(GY0 - 5U), 'C');
  graph_draw_y_label(min_value / 10, (uint16_t)(GY1 - 5U), 'C');

  graph_draw_series(graph_cold_x10, min_value, max_value, UI_YELLOW);
}
