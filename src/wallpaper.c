#include "wallpaper.h"
#include "bootlogo.h"
#include "display.h"

static uint8_t wallpaper_packed[6400];
static uint16_t wallpaper_palette[16];
static uint16_t wallpaper_line[160];
static uint8_t wallpaper_ready;

static uint16_t wallpaper_dim(uint16_t color)
{
  uint16_t r = (uint16_t)((color >> 11) & 0x1FU);
  uint16_t g = (uint16_t)((color >> 5) & 0x3FU);
  uint16_t b = (uint16_t)(color & 0x1FU);

  r = (uint16_t)((r * 8U) / 16U);
  g = (uint16_t)((g * 8U) / 16U);
  b = (uint16_t)((b * 8U) / 16U);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint8_t wallpaper_prepare(void)
{
  wallpaper_ready = bootlogo_copy_image(wallpaper_packed,
                                        sizeof(wallpaper_packed),
                                        wallpaper_palette);
  return wallpaper_ready;
}

uint8_t wallpaper_is_available(void)
{
  return (wallpaper_ready != 0U) ? 1U : wallpaper_prepare();
}

void wallpaper_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t row;
  uint16_t col;
  uint16_t bit;
  uint8_t index;
  uint16_t px;
  uint16_t py;

  if((w == 0U) || (h == 0U) || (x >= 160U) || (y >= 80U))
  {
    return;
  }

  if((x + w) > 160U)
  {
    w = (uint16_t)(160U - x);
  }
  if((y + h) > 80U)
  {
    h = (uint16_t)(80U - y);
  }

  if(wallpaper_ready == 0U)
  {
    display_fill_rect(x, y, w, h, 0x0000U);
    return;
  }

  display_set_window(x, y, w, h);
  display_begin_pixel_stream();

  for(row = 0U; row < h; row++)
  {
    py = (uint16_t)(y + row);
    for(col = 0U; col < w; col++)
    {
      px = (uint16_t)(x + col);
      bit = (uint16_t)(py * 160U + px);
      index = wallpaper_packed[bit >> 1];
      index = ((bit & 1U) == 0U) ? (uint8_t)(index >> 4) : (uint8_t)(index & 0x0FU);
      wallpaper_line[col] = wallpaper_dim(wallpaper_palette[index]);
    }

    display_write_pixel_stream_raw(wallpaper_line, w);
  }

  display_end_pixel_stream();
}

void wallpaper_draw(void)
{
  if(wallpaper_ready == 0U)
  {
    (void)wallpaper_prepare();
  }

  wallpaper_blit(0U, 0U, 160U, 80U);
}
