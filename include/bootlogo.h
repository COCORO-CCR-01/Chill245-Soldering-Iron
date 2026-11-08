#ifndef __BOOTLOGO_H
#define __BOOTLOGO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define BOOTLOGO_FLASH_ADDR       (0x0800E400UL)
#define BOOTLOGO_REGION_SIZE      (7168UL)
#define BOOTLOGO_HEADER_SIZE      (64UL)
#define BOOTLOGO_DATA_OFFSET      (BOOTLOGO_HEADER_SIZE)
#define BOOTLOGO_MAX_DATA         (7100UL)
#define BOOTLOGO_WIDTH            (160U)
#define BOOTLOGO_HEIGHT           (80U)

#define BOOTLOGO_MAGIC            (0x474F4C42UL)
#define BOOTLOGO_VERSION          (1U)

/**
  * @brief  初始化启动图上传串口（USART1: PA9/PA10, 115200）
  */
void bootlogo_init(void);

/**
  * @brief  查询 Flash 中是否存在有效启动图
  */
uint8_t bootlogo_is_valid(void);

/**
  * @brief  开机显示启动图并等待指定时间
  * @param  delay_s: 显示秒数，0 表示关闭
  */
void bootlogo_boot(float delay_s);



/**
  * @brief  把图片解码成 4bpp 打包数据（6400 B）并复制调色板
  * @retval 1 成功；0 无有效图片或参数错误
  */
uint8_t bootlogo_copy_image(uint8_t *packed, uint16_t packed_len,
                            uint16_t palette[16]);

/**
  * @brief  主循环轮询上位机上传命令
  * @retval 1 表示刚刚完成一次上传；2 表示尝试上传但失败
  */
uint8_t bootlogo_poll(void);

#ifdef __cplusplus
}
#endif

#endif
