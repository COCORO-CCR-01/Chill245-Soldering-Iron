#include "appcolors.h"

static uint16_t app_colors[APP_COLOR_COUNT];        /* 生效板：屏幕真正用的这一份 */
static uint16_t app_colors_base[APP_COLOR_COUNT];   /* 基准板：存盘用，浅色叠加不碰它 */

static const uint16_t appcolors_default[APP_COLOR_COUNT] =
{
  0xFC9FU,  /* primary: pink */
  0x05FFU,  /* secondary: blue */
  0x9CF3U,  /* tertiary: neutral gray */
  0xFFFFU,  /* quaternary: white */
  0x0861U   /* quinary: dark blue surface */
};

void appcolors_init(void)
{
  uint8_t i;

  for(i = 0U; i < APP_COLOR_COUNT; i++)
  {
    app_colors[i] = appcolors_default[i];
    app_colors_base[i] = appcolors_default[i];
  }
}

void appcolors_set(const uint16_t colors[APP_COLOR_COUNT])
{
  uint8_t i;

  if(colors == 0)
  {
    return;
  }

  for(i = 0U; i < APP_COLOR_COUNT; i++)
  {
    app_colors[i] = colors[i];
    /* 外部灌进来的整套调色板（EEPROM 里的存档、开机 Monet 自定义色）既是生效板也是
     * 基准板 —— 它没有经过浅色叠加，可以安全地当作基准存回去 */
    app_colors_base[i] = colors[i];
  }
}

void appcolors_get_all(uint16_t colors[APP_COLOR_COUNT])
{
  uint8_t i;

  for(i = 0U; i < APP_COLOR_COUNT; i++)
  {
    colors[i] = app_colors[i];
  }
}

/* 取基准板（存盘用）。不要用 appcolors_get_all()：那份可能已经被浅色叠加改过 */
void appcolors_get_base_all(uint16_t colors[APP_COLOR_COUNT])
{
  uint8_t i;

  for(i = 0U; i < APP_COLOR_COUNT; i++)
  {
    colors[i] = app_colors_base[i];
  }
}

/* 回到基准板：浅色模式关掉的时候调这个，把叠加层整个撤掉 */
void appcolors_restore_base(void)
{
  uint8_t i;

  for(i = 0U; i < APP_COLOR_COUNT; i++)
  {
    app_colors[i] = app_colors_base[i];
  }
}

/* 老版本的存盘逻辑会把"浅色叠加之后"的调色板当默认值存进 EEPROM，被污染过的存档在
 * 暗色模式下会拿回一套浅色板（surface 亮、文字近黑）。这里按 surface 亮度识别出来，
 * 丢回出厂暗色板。只在 ui_light == 0 时调用，不会动浅色模式自己的显示。 */
void appcolors_heal_dark_base(void)
{
  uint8_t i;

  if(app_colors_base[APP_COLOR_QUINARY] > 0x7FFFU)
  {
    for(i = 0U; i < APP_COLOR_COUNT; i++)
    {
      app_colors_base[i] = appcolors_default[i];
    }
  }

  appcolors_restore_base();
}

uint16_t appcolors_get(app_color_slot_t slot)
{
  if((uint8_t)slot >= APP_COLOR_COUNT)
  {
    return 0U;
  }

  return app_colors[slot];
}

uint16_t appcolors_surface(void)
{
  /* Host Monet chooses a dark or light quinary together with contrasting text. */
  return app_colors[APP_COLOR_QUINARY];
}

void appcolors_apply_light_defaults(void)
{
  app_colors[APP_COLOR_PRIMARY]    = 0xB800U;  /* dark red */
  app_colors[APP_COLOR_SECONDARY]  = 0x0010U;  /* dark blue */
  app_colors[APP_COLOR_TERTIARY]   = 0x4208U;  /* dark gray */
  app_colors[APP_COLOR_QUATERNARY] = 0x0001U;  /* near black */
  app_colors[APP_COLOR_QUINARY]    = 0xFFFEU;  /* near white surface */
}
