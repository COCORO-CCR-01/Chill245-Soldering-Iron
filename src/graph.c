/**
  **************************************************************************
  * @file     graph.c
  * @brief    160x80 横屏曲线图
  **************************************************************************
  * 历史窗口：80 点，250 ms/点 = 20 秒。
  * 图表：TEMP/PWR、VOLTAGE、COLD。
  **************************************************************************
  */

#include "graph.h"
#include "fmt.h"
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

/* 4bpp 绂诲睆缁樺浘鍖猴細鍏堢敾杩涘唴瀛橈紝鍐嶆暣鍧楀啓灞忥紝閬垮厤姣忓抚鍏堟竻榛戦€犳垚闂睆 */
#define GB_W                            (GW + 1U)
#define GB_H                            (GH + 1U)
#define GB_PIXELS                       (GB_W * GB_H)

#define GB_BLACK                        (0U)
#define GB_RED                          (1U)
#define GB_GREEN                        (2U)
#define GB_CYAN                         (3U)
#define GB_YELLOW                       (4U)
#define GB_GRAY                         (5U)
#define GB_DARK                         (6U)
#define GB_WHITE                        (7U)

static const uint16_t graph_palette[8] =
{
  0x0000U, 0xFC9FU, 0x05FFU, 0x05FFU, 0xFC9FU, 0x9CF3U, 0x18C3U, 0xFFFFU
};

static uint8_t graph_pixels[(GB_PIXELS + 1U) / 2U];
static uint16_t graph_line_buf[GB_W];
static uint8_t graph_static_page;
static uint8_t graph_static_valid;

void graph_invalidate(void)
{
  graph_static_valid = 0U;
}

static void graph_buf_clear(void)
{
  memset(graph_pixels, 0, sizeof(graph_pixels));
}

static void graph_buf_pixel(uint16_t x, uint16_t y, uint8_t color)
{
  uint16_t index;
  uint8_t *cell;

  if((x >= GB_W) || (y >= GB_H))
  {
    return;
  }

  index = (uint16_t)((y * GB_W) + x);
  cell = &graph_pixels[index >> 1];

  if((x & 1U) != 0U)
  {
    *cell = (uint8_t)((*cell & 0xF0U) | (color & 0x0FU));
  }
  else
  {
    *cell = (uint8_t)((*cell & 0x0FU) | ((color & 0x0FU) << 4));
  }
}

static void graph_buf_hline(uint16_t x, uint16_t y, uint16_t w, uint8_t color)
{
  uint16_t i;

  for(i = 0U; i < w; i++)
  {
    graph_buf_pixel((uint16_t)(x + i), y, color);
  }
}

static void graph_buf_vline(uint16_t x, uint16_t y, uint16_t h, uint8_t color)
{
  uint16_t i;

  for(i = 0U; i < h; i++)
  {
    graph_buf_pixel(x, (uint16_t)(y + i), color);
  }
}

static void graph_buf_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint8_t color)
{
  graph_buf_hline(x, y, w, color);
  graph_buf_hline(x, (uint16_t)(y + h - 1U), w, color);
  graph_buf_vline(x, y, h, color);
  graph_buf_vline((uint16_t)(x + w - 1U), y, h, color);
}

static void graph_buf_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                           uint8_t color)
{
  int16_t dx = (x1 >= x0) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
  int16_t sx = (x1 >= x0) ? 1 : -1;
  int16_t dy = (y1 >= y0) ? (int16_t)(y0 - y1) : (int16_t)(y1 - y0);
  int16_t sy = (y1 >= y0) ? 1 : -1;
  int16_t err = (int16_t)(dx + dy);
  int16_t e2;

  for(;;)
  {
    graph_buf_pixel((uint16_t)x0, (uint16_t)y0, color);

    if((x0 == x1) && (y0 == y1))
    {
      break;
    }

    e2 = (int16_t)(2 * err);
    if(e2 >= dy)
    {
      err = (int16_t)(err + dy);
      x0 = (int16_t)(x0 + sx);
    }
    if(e2 <= dx)
    {
      err = (int16_t)(err + dx);
      y0 = (int16_t)(y0 + sy);
    }
  }
}

static void graph_flush(void)
{
  uint16_t x;
  uint16_t y;
  uint8_t cell;
  uint8_t color;

  /* 整个绘图区一次开窗、一次 RAMWR，逐行连续送像素，避免旧行残留 */
  display_set_window(GX0, GY0, GB_W, GB_H);
  display_begin_pixel_stream();

  for(y = 0U; y < GB_H; y++)
  {
    for(x = 0U; x < GB_W; x++)
    {
      cell = graph_pixels[((y * GB_W) + x) >> 1];
      color = ((x & 1U) != 0U) ? (uint8_t)(cell & 0x0FU) : (uint8_t)(cell >> 4);
      graph_line_buf[x] = graph_palette[color & 0x07U];
    }

    display_write_pixel_stream(graph_line_buf, GB_W);
  }

  display_end_pixel_stream();
}

static void graph_prepare_static(uint8_t page)
{
  if((graph_static_valid != 0U) && (graph_static_page == page))
  {
    return;
  }

  /* 只在进入曲线页/换页时清一次静态区域，周期刷新不再整屏清黑 */
  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 18U, UI_BLACK);
  display_fill_rect(0U, 18U, GX0, (uint16_t)(GH + 1U), UI_BLACK);
  display_fill_rect(0U, 67U, DISPLAY_WIDTH, 13U, UI_BLACK);
  display_fill_rect(157U, 12U, 3U, 68U, UI_BLACK);

  graph_static_page = page;
  graph_static_valid = 1U;
}

static void graph_draw_frame(const char *title, const char *current)
{
  uint16_t i;
  char text[16];

  display_draw_string(2U, 1U, title, UI_WHITE, UI_BLACK, 2U);

  if(current != NULL)
  {
    /* 固定 8 字符宽度，右侧当前量刷新时不会留下旧字符 */
    fmt_snprintf(text, sizeof(text), "%8s", current);
    display_draw_string(94U, 2U, text, UI_WHITE, UI_BLACK, 2U);
  }

  graph_buf_rect(0U, 0U, GB_W, GB_H, GB_GRAY);

  for(i = 1U; i < 4U; i++)
  {
    graph_buf_hline(0U, (uint16_t)((GH * i) / 4U), GB_W, GB_DARK);
    graph_buf_vline((uint16_t)((GW * i) / 4U), 0U, GB_H, GB_DARK);
  }

  display_draw_string(GX0, (uint16_t)(GY1 + 1U), "20S", UI_GRAY, UI_BLACK, 2U);
}

static void graph_draw_y_label(int32_t value, uint16_t y, char suffix)
{
  char text[16];
  uint16_t width;
  uint16_t x;

  if(suffix != '\0')
  {
    fmt_snprintf(text, sizeof(text), "%d%c", (int)value, suffix);
  }
  else
  {
    fmt_snprintf(text, sizeof(text), "%d", (int)value);
  }

  width = display_string_width(text, 2U);
  x = (width < GX0) ? (uint16_t)(GX0 - width) : 0U;

  display_fill_rect(0U, y, GX0, 10U, UI_BLACK);
  display_draw_string(x, y, text, UI_GRAY, UI_BLACK, 2U);
}

static void graph_draw_series(const int16_t *values,
                              int32_t min_value, int32_t max_value,
                              uint8_t color)
{
  uint16_t i;
  uint16_t index;
  int16_t x;
  int16_t y;
  int16_t prev_x = 0;
  int16_t prev_y = 0;
  uint8_t have_prev = 0U;

  if(graph_count == 0U)
  {
    return;
  }

  for(i = 0U; i < graph_count; i++)
  {
    index = graph_index(i);
    x = (int16_t)(graph_x(i) - GX0);
    y = (int16_t)(graph_y(values[index], min_value, max_value) - GY0);

    if(have_prev != 0U)
    {
      graph_buf_line(prev_x, prev_y, x, y, color);
    }
    else
    {
      graph_buf_pixel((uint16_t)x, (uint16_t)y, color);
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
  sm_status_t status;
  char current[20];
  int tip_c;

  sm_get_status(&status);
  tip_c = (status.temp_valid != 0U) ?
          (int)(status.tip_c + 0.5f) : 0;
  if(tip_c < 0)
  {
    tip_c = 0;
  }
  if(tip_c > 999)
  {
    tip_c = 999;
  }
  fmt_snprintf(current, sizeof(current), "%dC%d%%", tip_c, (int)status.power);

  graph_prepare_static(1U);
  graph_buf_clear();
  graph_draw_frame("TEMP/PWR", current);

  graph_draw_y_label(450, (uint16_t)(GY0 - 3U), '\0');
  graph_draw_y_label(0, (uint16_t)(GY1 - 5U), '\0');

  graph_draw_series(graph_temp_x10, 0, 4500, GB_RED);
  graph_draw_series(graph_power_pct, 0, 100, GB_GREEN);
  graph_flush();
}

void graph_draw_voltage(void)
{
  int32_t min_value;
  int32_t max_value;
  char current[20];
  uint16_t vin_mv;

  graph_range_i16(graph_vin_cv, &min_value, &max_value, 500);

  if(min_value < 0)
  {
    min_value = 0;
  }

  vin_mv = power_get_voltage_mv();
  fmt_snprintf(current, sizeof(current), "%d.%dV",
           (int)(vin_mv / 1000U), (int)((vin_mv % 1000U) / 100U));

  graph_prepare_static(2U);
  graph_buf_clear();
  graph_draw_frame("VOLTAGE", current);

  graph_draw_y_label(max_value / 100, (uint16_t)(GY0 - 3U), 'V');
  graph_draw_y_label(min_value / 100, (uint16_t)(GY1 - 5U), 'V');

  graph_draw_series(graph_vin_cv, min_value, max_value, GB_CYAN);
  graph_flush();
}

void graph_draw_cold(void)
{
  sm_status_t status;
  int32_t min_value;
  int32_t max_value;
  char current[20];
  int room_x10;

  graph_range_i16(graph_cold_x10, &min_value, &max_value, 20);

  sm_get_status(&status);
  room_x10 = (int)((status.room_c * 10.0f) +
                   ((status.room_c >= 0.0f) ? 0.5f : -0.5f));
  fmt_snprintf(current, sizeof(current), "%d.%dC",
           room_x10 / 10, (room_x10 < 0) ? (-room_x10) % 10 : room_x10 % 10);

  graph_prepare_static(3U);
  graph_buf_clear();
  graph_draw_frame("COLD", current);

  graph_draw_y_label(max_value / 10, (uint16_t)(GY0 - 3U), 'C');
  graph_draw_y_label(min_value / 10, (uint16_t)(GY1 - 5U), 'C');

  graph_draw_series(graph_cold_x10, min_value, max_value, GB_YELLOW);
  graph_flush();
}

