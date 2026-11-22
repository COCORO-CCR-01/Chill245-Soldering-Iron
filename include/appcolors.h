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

/* 基准调色板和生效调色板是两份：浅色模式只把叠加层打在生效板上，基准板不动。
 * 存盘只能存基准板 —— 存生效板的话，切过一次浅色模式再存盘，浅色的近黑文字色就被
 * 存成了默认值，下次开机哪怕 ui_light=0 也会拿回一套浅色板，暗色 UI 上白字变成近黑
 * 看不见（用户实测：白色切回黑色必坏，只有恢复出厂能救）。 */
void appcolors_get_base_all(uint16_t colors[APP_COLOR_COUNT]);
void appcolors_restore_base(void);

/* 自愈：暗色模式下如果发现基准板本身就是一套浅色板（被上面那个老逻辑污染过的存档），
 * 丢掉它退回出厂暗色板，不用再靠恢复出厂设置。只在 ui_light == 0 时调用。 */
void appcolors_heal_dark_base(void);

/* 由五级色派生出的深色面板色，避免文字被壁纸遮挡 */
uint16_t appcolors_surface(void);

/* 浅色模式默认配色（无壁纸时也保持高对比） */
void appcolors_apply_light_defaults(void);

#ifdef __cplusplus
}
#endif

#endif
