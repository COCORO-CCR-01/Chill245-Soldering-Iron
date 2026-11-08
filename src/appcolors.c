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
