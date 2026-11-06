#ifndef __WALLPAPER_H
#define __WALLPAPER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  是否存在可用壁纸（与启动 Logo 共用同一张 8 KB 图片）
  */
uint8_t wallpaper_is_available(void);

/**
  * @brief  把图片解码到 RAM 缓存，供脏刷新复用
  */
uint8_t wallpaper_prepare(void);

/**
  * @brief  以压暗方式绘制 160x80 壁纸
  */
void wallpaper_draw(void);

/**
  * @brief  重绘一个壁纸矩形，用于脏刷新前恢复背景
  */
void wallpaper_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

#ifdef __cplusplus
}
#endif

#endif
