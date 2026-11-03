#ifndef __UI_ICONS_H
#define __UI_ICONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum
{
  UI_ICON_THERMO = 0,
  UI_ICON_BOLT,
  UI_ICON_PLUG,
  UI_ICON_SNOW,
  UI_ICON_TARGET,
  UI_ICON_MODE,
  UI_ICON_CHECK,
  UI_ICON_WARN,
  UI_ICON_GEAR,
  UI_ICON_SPEAKER,
  UI_ICON_CLOCK,
  UI_ICON_ARROW_RIGHT,
  UI_ICON_UP_DOWN,
  UI_ICON_POWER,
  UI_ICON_FLAME,
  UI_ICON_CURVE,
  UI_ICON_COUNT
} ui_icon_t;

/**
  * @brief  绘制 8x8 单色图标
  * @param  icon: 图标
  * @param  x/y: 左上角坐标
  * @param  color: 颜色
  * @retval none
  * @note   不填充背景，调用者负责先画底色。
  */
void ui_icon_draw(ui_icon_t icon, uint16_t x, uint16_t y, uint16_t color);

#ifdef __cplusplus
}
#endif

#endif
