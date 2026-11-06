#ifndef __EEPROM_H
#define __EEPROM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "pid.h"
#include "temp.h"

/*
 * 参数区使用最后 2 KB：
 *   sector 54: 0x0800D800 ~ 0x0800DBFF
 *   sector 55: 0x0800DC00 ~ 0x0800DFFF
 *   sector 56..63: 0x0800E000 ~ 0x0800FFFF  boot logo RLE storage
 * 链接脚本 AT32F421x8_62K.ld 把程序区限制为 62 KB，
 * 避免代码/常量占用这两个参数扇区。
 */
#define EEPROM_FLASH_BASE               (0x0800D800UL)
#define EEPROM_SECTOR_SIZE              (1024UL)
#define EEPROM_SECTOR0_ADDR             (EEPROM_FLASH_BASE)
#define EEPROM_SECTOR1_ADDR             (EEPROM_FLASH_BASE + EEPROM_SECTOR_SIZE)
#define EEPROM_MAGIC                    (0x4A424345UL)   /* "JBCE" */
#define EEPROM_FORMAT_VERSION           (0x0008U)

/**
  * @brief  所有掉电保存参数
  * @note   字段与菜单设置项一一对应；新增字段时递增 EEPROM_FORMAT_VERSION。
  */
typedef struct
{
  float setpoint_c;
  uint8_t pid_mode;
  pid_curve_t standard_curve;
  pid_custom_point_t custom_points[PID_CUSTOM_POINTS];

  float cal_adc[TEMP_TC_CAL_POINTS];
  float cal_temp[TEMP_TC_CAL_POINTS];
  uint8_t cal_enabled;
  float cal_ref_c;

  float stationary_s;
  float sleep_power_pct;
  float bootlogo_delay_s;
  uint16_t app_colors[5];
  float reduced_temp_c;
  float reduced_off_s;
  float backlight_off_s;

  float low_voltage_v;
  float high_voltage_v;
  float voltage_hysteresis_v;
  float voltage_trip_ms;
  float voltage_recovery_ms;

  uint8_t ch224a_index;
  uint8_t maindash_alt;
  uint8_t ui_light;

  float autoroll_s;
  uint8_t autoroll_enabled;
  uint8_t buzzer_enabled;
  float buzzer_volume;

  float boost_offset_c;
  float boost_drop_c;
  float boost_timeout_s;
  float setpoint_step_c;
  float time_step_s;

  float verify_overtemp_c;
  float verify_jump_c;
  float verify_jump_rate;
  float verify_no_rise_s;
  float verify_no_rise_delta_c;
  float verify_full_power_s;
  float verify_drop_c;
} eeprom_params_t;

extern eeprom_params_t g_eeprom_params;

/**
  * @brief  EEPROM 状态
  */
typedef enum
{
  EEPROM_ERROR_NONE = 0,
  EEPROM_ERROR_NO_RECORD,
  EEPROM_ERROR_VERSION,
  EEPROM_ERROR_CORRUPT,
  EEPROM_ERROR_WRITE
} eeprom_error_t;

/**
  * @brief  初始化参数区并加载参数
  * @param  none
  * @retval none
  * @note   必须在 sm_init()、power_init()、verifyheater_init() 等模块初始化后调用。
  *         没有有效记录时，从当前模块默认值生成一份 RAM 参数，不立即写 Flash。
  */
void eeprom_init(void);

/**
  * @brief  从当前各模块读取参数到 g_eeprom_params
  * @param  none
  * @retval none
  * @note   菜单打开时调用，保证显示的是当前运行值。
  */
void eeprom_capture_current(void);

/**
  * @brief  把 g_eeprom_params 应用到各模块
  * @param  none
  * @retval none
  * @note   自动整定运行中会跳过设定温度/PID/标定，避免破坏整定。
  */
void eeprom_apply(void);

/**
  * @brief  保存 g_eeprom_params 到 Flash
  * @param  none
  * @retval 1 成功或参数未变化；0 失败/整定中跳过
  * @note   双扇区交替写入；写入前关闭加热器并短暂关中断。
  *         自动整定运行中不保存，避免打断整定。
  */
uint8_t eeprom_save(void);

/**
  * @brief  EEPROM 后台任务
  * @param  now_ms: 毫秒时间戳
  * @retval none
  * @note   自动整定完成后自动保存一次 PID 结果；菜单打开时可不调用。
  */
void eeprom_task(uint32_t now_ms);

/**
  * @brief  获取参数区指针
  * @param  none
  * @retval 参数指针
  */
eeprom_params_t *eeprom_get_params(void);

/**
  * @brief  是否从 Flash 成功加载过有效记录
  * @param  none
  * @retval 1 是；0 否（当前为默认值）
  */
uint8_t eeprom_has_valid_record(void);

/**
  * @brief  获取 EEPROM 状态
  * @param  none
  * @retval 状态
  */
eeprom_error_t eeprom_get_error(void);

/**
  * @brief  恢复默认设置
  * @param  none
  * @retval none
  * @note   擦除两个参数扇区后复位；失败时置 HMS EEPROM WRITE。
  */
void eeprom_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif
