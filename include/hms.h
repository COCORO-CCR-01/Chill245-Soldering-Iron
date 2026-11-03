#ifndef __HMS_H
#define __HMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
  * @brief  HMS 故障码
  */
typedef enum
{
  HMS_CODE_NONE = 0,
  HMS_CODE_EEPROM_CRC,
  HMS_CODE_EEPROM_WRITE,
  HMS_CODE_HEATER_WD
} hms_code_t;

/**
  * @brief  初始化 HMS 状态
  * @param  none
  * @retval none
  */
void hms_init(void);

/**
  * @brief  设置不可恢复故障，进入持续 HMS 屏幕
  * @param  code: 故障码
  * @retval none
  * @note   一旦设置不会自动清除；所有正常 UI 被 HMS 覆盖，加热被强制关闭。
  */
void hms_set_fatal(hms_code_t code);

/**
  * @brief  查询是否存在不可恢复故障
  * @param  none
  * @retval 1 是；0 否
  */
uint8_t hms_has_fatal(void);

/**
  * @brief  查询 HMS 是否占用屏幕
  * @param  none
  * @retval 1 占用；0 空闲
  */
uint8_t hms_is_active(void);

/**
  * @brief  获取 HMS 故障码
  * @param  none
  * @retval 故障码
  */
hms_code_t hms_get_code(void);

/**
  * @brief  在设置中调出 HMS 自检屏
  * @param  none
  * @retval none
  * @note   无故障显示 ALL OK；有可恢复故障显示 FAULT ACTIVE。约 3 s 后自动返回。
  *         存在不可恢复故障时忽略。
  */
void hms_show_all_ok(void);

/**
  * @brief  显示 EEPROM 恢复提示
  * @param  none
  * @retval none
  * @note   EEPROM CRC 损坏并成功写回默认值后显示约 3 s。
  */
void hms_show_eeprom_recovered(void);

/**
  * @brief  HMS 周期任务
  * @param  now_ms: 毫秒时间戳
  * @retval 1 当前 HMS 占用屏幕；0 可显示正常 UI
  */
uint8_t hms_task(uint32_t now_ms);

/**
  * @brief  强制重画 HMS 屏幕
  * @param  none
  * @retval none
  */
void hms_force_redraw(void);

#ifdef __cplusplus
}
#endif

#endif
