#ifndef __DEBUG_H
#define __DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "adc.h"
#include "temp.h"

/*
 * 调试串口：
 *   PA9  -> USART1_TX
 *   PA10 -> USART1_RX
 *   115200, 8N1
 *
 * 控制台命令（内置）：
 *   help / ?                 显示帮助
 *   list / triggers          列出已注册的可触发函数
 *   commands                 列出已注册的带参数命令
 *   raw / adc                读取一次 ADC 原始值和温度
 *   temp                     读取一次温度
 *   stream on [ms]           周期打印原始数据
 *   stream off               关闭周期打印
 *   console on [ms]          同 stream on
 *   console off              同 stream off
 *   echo on|off              本地回显
 *   uptime                   打印毫秒时间戳
 *   run <trigger>            触发已注册函数
 *   tcal start|cancel|status|actual <C>  热端 7 点校准
 *   boost start|stop|status  OneKeyBoost
 *
 * 注册的触发函数/命令：
 *   - 名称和帮助字符串必须是静态生命周期字符串
 *   - 触发函数为无参数回调
 *   - 命令回调接收 argc/argv，用于带参数操作
 */

typedef void (*debug_trigger_fn_t)(void);
typedef void (*debug_command_fn_t)(int argc, char **argv);

/**
  * @brief  初始化调试串口和控制台
  * @param  none
  * @retval none
  * @note   PA9/PA10 配置为 USART1，115200 8N1，开启 RX 中断。
  *         本函数会清空触发函数/命令注册表，注册需在 debug_init() 之后进行。
  */
void debug_init(void);

/**
  * @brief  控制台周期处理
  * @param  adc_raw: 最新 ADC 原始数据
  * @param  temp_data: 最新温度数据
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   建议每 10 ms 调用一次；内部处理串口命令和周期数据流。
  */
void debug_update(const adc_raw_data_t *adc_raw,
                  const temp_data_t *temp_data,
                  uint32_t now_ms);

/**
  * @brief  注册无参数可触发函数
  * @param  name: 命令名，例如 "autotune"
  * @param  help: 帮助文本
  * @param  fn: 回调
  * @retval 1 成功；0 失败（表满、重名或参数无效）
  */
uint8_t debug_register_trigger(const char *name,
                               const char *help,
                               debug_trigger_fn_t fn);

/**
  * @brief  注册带参数命令
  * @param  name: 命令名
  * @param  help: 帮助文本
  * @param  fn: 回调，argc/argv 由控制台解析
  * @retval 1 成功；0 失败
  */
uint8_t debug_register_command(const char *name,
                               const char *help,
                               debug_command_fn_t fn);

/**
  * @brief  按名称触发已注册函数
  * @param  name: 触发函数名
  * @retval 1 找到并执行；0 未找到
  */
uint8_t debug_run_trigger(const char *name);

/**
  * @brief  向调试串口输出字符串
  * @param  text: 字符串
  * @retval none
  * @note   发送为中断驱动、非阻塞；发送缓冲满时丢弃新字节。
  *         只允许在主循环上下文调用，不要在中断中调用。
  */
void debug_write(const char *text);

/**
  * @brief  格式化输出
  * @param  fmt: 格式字符串
  * @param  ...: 参数
  * @retval none
  * @note   支持 %s/%c/%d/%u 等整数格式；浮点数请使用 debug_write_float()。
  *         只允许在主循环上下文调用，不要在中断中调用。
  */
void debug_printf(const char *fmt, ...);

/**
  * @brief  输出浮点数
  * @param  value: 数值
  * @param  decimals: 小数位数，0~3
  * @retval none
  */
void debug_write_float(float value, uint8_t decimals);

#ifdef __cplusplus
}
#endif

#endif
