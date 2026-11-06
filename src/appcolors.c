#include "appcolors.h"

static uint16_t app_colors[APP_COLOR_COUNT];

static const uint16_t appcolors_default[APP_COLOR_COUNT] =
{
  0xFC9FU,  /* primary: pink */
  0x05FFU,  /* secondary: blue */
  0x9CF3U,  /* tertiary: neutral gray */
  0xFFFFU,  /* quaternary: white */
  0x0861U   /* quinary: dark blue surface */
};

static uint16_t appcolors_darken(uint16_t color)
{
  uint16_t r = (uint16_t)((color >> 11) & 0x1FU);
  uint16_t g = (uint16_t)((color >> 5) & 0x3FU);
  uint16_t b = (uint16_t)(color & 0x1FU);

  r = (uint16_t)((r * 7U) / 16U);
  g = (uint16_t)((g * 7U) / 16U);
  b = (uint16_t)((b * 7U) / 16U);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

void appcolors_init(void)
{
  uint8_t i;

  for(i = 0U; i < APP_COLOR_COUNT; i++)
  {
    app_colors[i] = appcolors_default[i];
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
  return appcolors_darken(app_colors[APP_COLOR_QUINARY]);
}
