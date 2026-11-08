#ifndef __APPCOLORS_H
#define __APPCOLORS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  APP_COLOR_PRIMARY = 0,
  APP_COLOR_SECONDARY,
  APP_COLOR_TERTIARY,
  APP_COLOR_QUATERNARY,
  APP_COLOR_QUINARY,
  APP_COLOR_COUNT
} app_color_slot_t;

void appcolors_init(void);
void appcolors_set(const uint16_t colors[APP_COLOR_COUNT]);
void appcolors_get_all(uint16_t colors[APP_COLOR_COUNT]);
uint16_t appcolors_get(app_color_slot_t slot);

/* 由五级色派生出的深色面板色，避免文字被壁纸遮挡 */
uint16_t appcolors_surface(void);

/* 浅色模式默认配色（无壁纸时也保持高对比） */
void appcolors_apply_light_defaults(void);

#ifdef __cplusplus
}
#endif

#endif
