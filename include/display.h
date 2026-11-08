#ifndef __DISPLAY_H
#define __DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ST7735S 160(H) x 80(V) RGB565，横屏 */
#define DISPLAY_WIDTH                   (160U)
#define DISPLAY_HEIGHT                  (80U)

/* RGB565 常用颜色 */
#define DISPLAY_COLOR_BLACK             (0x0000U)
#define DISPLAY_COLOR_WHITE             (0xFFFFU)
#define DISPLAY_COLOR_RED               (0xF800U)
#define DISPLAY_COLOR_GREEN             (0x07E0U)
#define DISPLAY_COLOR_BLUE              (0x001FU)
#define DISPLAY_COLOR_YELLOW            (0xFFE0U)
#define DISPLAY_COLOR_CYAN              (0x07FFU)
#define DISPLAY_COLOR_MAGENTA           (0xF81FU)
#define DISPLAY_COLOR_GRAY              (0x8410U)

/**
  * @brief  初始化 ST7735S 显示驱动
  * @param  none
  * @retval none
  * @note   4-line SPI：
  *           PA7 = MOSI
  *           PA5 = SCLK
  *           PB0 = RS / DC
  *           PA4 = CS
  *           PA3 = 背光
  *         初始化后背光保持关闭，由 display_backlight_set() 打开。
  */
void display_init(void);

/**
  * @brief  打开/关闭背光
  * @param  on: 1 打开，0 关闭
  * @retval none
  */
void display_backlight_set(uint8_t on);

/**
  * @brief  获取显示宽度
  * @param  none
  * @retval 80
  */
uint16_t display_get_width(void);

/**
  * @brief  获取显示高度
  * @param  none
  * @retval 160
  */
uint16_t display_get_height(void);

/**
  * @brief  设置像素写入窗口
  * @param  x: 左上角 X
  * @param  y: 左上角 Y
  * @param  w: 宽度
  * @param  h: 高度
  * @retval none
  * @note   调用后接 display_write_pixels() 可批量写入 RGB565 数据。
  */
void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
  * @brief  设置深色/浅色主题
  * @param  light: 1=浅色，0=深色（默认纯黑背景）
  * @retval none
  * @note   只影响颜色映射，不改变调用方的逻辑颜色。
  */
void display_set_light_theme(uint8_t light);

/**
  * @brief  0/180° 屏幕翻转，用于左右手
  */
void display_set_flip(uint8_t flip);

/**
  * @brief  写入 RGB565 像素数组
  * @param  colors: 像素数组
  * @param  count: 像素数量
  * @retval none
  * @note   调用前需先 display_set_window()。
  */
void display_write_pixels(const uint16_t *colors, uint32_t count);

/**
  * @brief  开始一次连续像素流（同一个窗口只发一次 RAMWR）
  * @retval none
  * @note   调用前需先 display_set_window()；配合
  *         display_write_pixel_stream() / display_end_pixel_stream() 使用。
  */
void display_begin_pixel_stream(void);

/**
  * @brief  向已开始的像素流追加 RGB565 像素
  * @param  colors: 像素数组
  * @param  count: 像素数量
  * @retval none
  */
void display_write_pixel_stream(const uint16_t *colors, uint32_t count);

/**
  * @brief  像素流，绕过暗/亮主题颜色映射（壁纸、启动图使用）
  */
void display_write_pixel_stream_raw(const uint16_t *colors, uint32_t count);

/**
  * @brief  结束连续像素流并释放 CS
  * @retval none
  */
void display_end_pixel_stream(void);

/**
  * @brief  填充整个屏幕
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_fill(uint16_t color);

/**
  * @brief  清屏
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_clear(uint16_t color);

/**
  * @brief  填充矩形
  * @param  x: 左上角 X
  * @param  y: 左上角 Y
  * @param  w: 宽度
  * @param  h: 高度
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
  * @brief  画一个像素
  * @param  x: X
  * @param  y: Y
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_draw_pixel(uint16_t x, uint16_t y, uint16_t color);

/**
  * @brief  画水平线
  * @param  x: 起点 X
  * @param  y: Y
  * @param  w: 长度
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);

/**
  * @brief  画垂直线
  * @param  x: X
  * @param  y: 起点 Y
  * @param  h: 长度
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color);

/**
  * @brief  画直线（Bresenham）
  * @param  x0/y0: 起点
  * @param  x1/y1: 终点
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

/**
  * @brief  画矩形边框
  * @param  x/y: 左上角
  * @param  w/h: 宽高
  * @param  color: RGB565 颜色
  * @retval none
  */
void display_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
  * @brief  画一个字符（3x5 字体，自动转大写）
  * @param  x/y: 左上角
  * @param  ch: ASCII 字符；0x7F 表示 °
  * @param  fg: 前景色
  * @param  bg: 背景色
  * @param  scale: 放大倍数 1~4
  * @retval none
  */
void display_draw_char(uint16_t x, uint16_t y, char ch,
                       uint16_t fg, uint16_t bg, uint8_t scale);

/**
  * @brief  画字符串
  * @param  x/y: 左上角
  * @param  text: 字符串（建议大写）
  * @param  fg: 前景色
  * @param  bg: 背景色
  * @param  scale: 放大倍数 1~4
  * @retval none
  */
void display_draw_string(uint16_t x, uint16_t y, const char *text,
                         uint16_t fg, uint16_t bg, uint8_t scale);

/**
  * @brief  透明字符/字符串，只写前景像素并带 1px 阴影
  */
void display_draw_char_transparent(uint16_t x, uint16_t y, char ch,
                                   uint16_t fg, uint8_t scale);
void display_draw_string_transparent(uint16_t x, uint16_t y,
                                     const char *text,
                                     uint16_t fg, uint8_t scale);

uint16_t display_contrast_shadow(uint16_t fg);

/**
  * @brief  计算字符串像素宽度
  * @param  text: 字符串
  * @param  scale: 放大倍数
  * @retval 宽度
  */
uint16_t display_string_width(const char *text, uint8_t scale);

/**
  * @brief  发送单条命令
  * @param  cmd: ST7735S 命令
  * @retval none
  */
void display_write_command(uint8_t cmd);

/**
  * @brief  发送命令参数/数据
  * @param  data: 数据指针
  * @param  len: 字节数
  * @retval none
  */
void display_write_data(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
