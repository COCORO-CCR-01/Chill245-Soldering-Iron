#ifndef __ADC_H
#define __ADC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief 三路 ADC 原始采样数据
  * @note  12 位右对齐，范围 0~4095，参考电压为 VDDA。
  */
typedef struct
{
  uint16_t ntc;           /* PA0 / ADC_IN0：T-NTC，NTC 温度补偿采样 */
  uint16_t thermocouple;  /* PA1 / ADC_IN1：T-ADC，热电偶放大后的温度信号 */
  uint16_t vin;           /* PA2 / ADC_IN2：V-ADC，输入电压采样 */
  uint8_t  valid;         /* 1：本次三路转换均完成；0：转换失败，数据保持上一次值 */
} adc_raw_data_t;

/**
  * @brief  初始化 PA0、PA1、PA2 为模拟输入，并配置 ADC1 扫描三路通道
  * @param  none
  * @retval none
  */
void adc_init(void);

/**
  * @brief  启动一次三路转换，读取 PA0、PA1、PA2 的 ADC 原始值
  * @param  raw: 输出数据指针
  * @retval none
  * @note   转换失败时 raw->valid 为 0，三路数据保持上一次的值。
  */
void adc_read_all(adc_raw_data_t *raw);

#ifdef __cplusplus
}
#endif

#endif
