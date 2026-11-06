/**
  **************************************************************************
  * @file     eeprom.c
  * @brief    Flash 参数区：双扇区交替保存 + CRC + 提交标记
  **************************************************************************
  * 参数区：
  *   sector 62: 0x0800F800 ~ 0x0800FBFF
  *   sector 63: 0x0800FC00 ~ 0x0800FFFF
  *
  * 保存策略：
  *   - 始终保留一份旧记录，擦除/写入另一扇区；
  *   - 记录含 magic/version/size/sequence/CRC/commit；
  *   - 写入完成后回读校验；掉电时至少保留上一份有效记录；
  *   - 保存前 heater_force_off()，并短暂关闭中断，避免 Flash 操作期间加热。
  **************************************************************************
  */

#include "eeprom.h"
#include "at32f421_conf.h"
#include "statemachine.h"
#include "verifyheater.h"
#include "powermanagement.h"
#include "autoroll.h"
#include "buzzer.h"
#include "hms.h"
#include "heater.h"
#include "display.h"
#include "bootlogo.h"
#include "appcolors.h"
#include <stddef.h>
#include <string.h>

#define EEPROM_COMMIT_MARK              (0x5AA5C33CUL)
#define EEPROM_ERASE_TIMEOUT            (0x40000000UL)
#define EEPROM_PROGRAM_TIMEOUT          (0x40000000UL)

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t sequence;
  uint32_t crc;
  eeprom_params_t params;
  uint32_t commit;
} eeprom_record_t;

_Static_assert(sizeof(eeprom_record_t) <= EEPROM_SECTOR_SIZE,
               "eeprom record larger than sector");
_Static_assert((sizeof(eeprom_record_t) % 4U) == 0U,
               "eeprom record not word aligned");

eeprom_params_t g_eeprom_params;

static eeprom_record_t eeprom_work;
static uint8_t eeprom_loaded_valid;
static eeprom_error_t eeprom_error;

typedef enum
{
  EEPROM_RECORD_BLANK = 0,
  EEPROM_RECORD_VALID,
  EEPROM_RECORD_OLD_VERSION,
  EEPROM_RECORD_CORRUPT
} eeprom_record_status_t;

static const float eeprom_ch224a_table[6] =
{
  5.0f, 9.0f, 12.0f, 15.0f, 20.0f, 28.0f
};

static uint8_t eeprom_ch224a_index_from_voltage(float voltage)
{
  uint8_t best = 4U;
  float best_delta = 1000.0f;

  for(uint8_t i = 0U; i < 6U; i++)
  {
    float delta = voltage - eeprom_ch224a_table[i];

    if(delta < 0.0f)
    {
      delta = -delta;
    }

    if(delta < best_delta)
    {
      best_delta = delta;
      best = i;
    }
  }

  return best;
}

static uint32_t eeprom_crc32(const void *data, uint32_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFUL;

  while(length-- != 0U)
  {
    crc ^= *bytes++;

    for(uint8_t bit = 0U; bit < 8U; bit++)
    {
      if((crc & 1U) != 0U)
      {
        crc = (crc >> 1U) ^ 0xEDB88320UL;
      }
      else
      {
        crc >>= 1U;
      }
    }
  }

  return ~crc;
}

static const eeprom_record_t *eeprom_record_at(uint32_t address)
{
  return (const eeprom_record_t *)address;
}

static uint8_t eeprom_record_valid(const eeprom_record_t *record)
{
  if(record == NULL)
  {
    return 0U;
  }

  if((record->magic != EEPROM_MAGIC) ||
     (record->version != EEPROM_FORMAT_VERSION) ||
     (record->size != sizeof(eeprom_params_t)) ||
     (record->commit != EEPROM_COMMIT_MARK))
  {
    return 0U;
  }

  return (record->crc ==
          eeprom_crc32(&record->params, sizeof(eeprom_params_t))) ? 1U : 0U;
}

static uint8_t eeprom_region_blank(uint32_t address)
{
  const uint32_t *words = (const uint32_t *)address;
  uint32_t count = EEPROM_SECTOR_SIZE / 4U;

  for(uint32_t i = 0U; i < count; i++)
  {
    if(words[i] != 0xFFFFFFFFUL)
    {
      return 0U;
    }
  }

  return 1U;
}

static eeprom_record_status_t eeprom_record_status(
  const eeprom_record_t *record, uint32_t address)
{
  if(eeprom_region_blank(address) != 0U)
  {
    return EEPROM_RECORD_BLANK;
  }

  if(record->magic != EEPROM_MAGIC)
  {
    return EEPROM_RECORD_CORRUPT;
  }

  if(record->version != EEPROM_FORMAT_VERSION)
  {
    return EEPROM_RECORD_OLD_VERSION;
  }

  if((record->size != sizeof(eeprom_params_t)) ||
     (record->commit != EEPROM_COMMIT_MARK) ||
     (record->crc != eeprom_crc32(&record->params,
                                 sizeof(eeprom_params_t))))
  {
    return EEPROM_RECORD_CORRUPT;
  }

  return EEPROM_RECORD_VALID;
}

static uint8_t eeprom_params_sane(const eeprom_params_t *params)
{
  if(params == NULL)
  {
    return 0U;
  }

  if((params->setpoint_c < 0.0f) || (params->setpoint_c > 450.0f) ||
     (params->pid_mode >= PID_MODE_COUNT) ||
     (params->ch224a_index > 5U) ||
     (params->maindash_alt > 1U) ||
     (params->ui_light > 1U) ||
     (params->cal_enabled > 1U) ||
     (params->autoroll_enabled > 1U) ||
     (params->buzzer_enabled > 1U) ||
     (params->buzzer_volume < 0.0f) ||
     (params->buzzer_volume > 100.0f) ||
     (params->boost_offset_c < 0.0f) ||
     (params->boost_offset_c > 200.0f) ||
     (params->boost_drop_c < 5.0f) ||
     (params->boost_drop_c > 100.0f) ||
     (params->boost_timeout_s < 10.0f) ||
     (params->boost_timeout_s > 1800.0f) ||
     (params->setpoint_step_c < 1.0f) ||
     (params->setpoint_step_c > 100.0f) ||
     (params->time_step_s < 1.0f) ||
     (params->time_step_s > 600.0f))
  {
    return 0U;
  }

  if((params->stationary_s < 1.0f) ||
     (params->sleep_power_pct < 0.0f) ||
     (params->sleep_power_pct > 100.0f) ||
     (params->bootlogo_delay_s < 0.0f) ||
     (params->bootlogo_delay_s > 10.0f) ||
     (params->reduced_off_s < 1.0f) ||
     (params->backlight_off_s < 1.0f) ||
     (params->autoroll_s < 1.0f))
  {
    return 0U;
  }

  return 1U;
}

eeprom_params_t *eeprom_get_params(void)
{
  return &g_eeprom_params;
}

uint8_t eeprom_has_valid_record(void)
{
  return eeprom_loaded_valid;
}

eeprom_error_t eeprom_get_error(void)
{
  return eeprom_error;
}

void eeprom_capture_current(void)
{
  temp_tc_cal_point_t cal_points[TEMP_TC_CAL_POINTS];
  verifyheater_config_t verify;
  float low_v;
  float high_v;
  float boost_offset_c = g_eeprom_params.boost_offset_c;
  float boost_drop_c = g_eeprom_params.boost_drop_c;
  float boost_timeout_s = g_eeprom_params.boost_timeout_s;
  float setpoint_step_c = g_eeprom_params.setpoint_step_c;
  float time_step_s = g_eeprom_params.time_step_s;
  uint8_t maindash_alt = g_eeprom_params.maindash_alt;
  uint8_t ui_light = g_eeprom_params.ui_light;
  float bootlogo_delay_s = g_eeprom_params.bootlogo_delay_s;

  memset(&g_eeprom_params, 0, sizeof(g_eeprom_params));

  /* OneKeyBoost/设置步进不是运行模块状态，捕获时保留当前配置 */
  g_eeprom_params.boost_offset_c = boost_offset_c;
  g_eeprom_params.boost_drop_c = boost_drop_c;
  g_eeprom_params.boost_timeout_s = boost_timeout_s;
  g_eeprom_params.setpoint_step_c = setpoint_step_c;
  g_eeprom_params.time_step_s = time_step_s;
  g_eeprom_params.maindash_alt = maindash_alt;
  g_eeprom_params.ui_light = ui_light;
  g_eeprom_params.bootlogo_delay_s = bootlogo_delay_s;

  g_eeprom_params.setpoint_c = sm_get_setpoint();
  g_eeprom_params.pid_mode = (uint8_t)sm_get_pid_mode();

  pid_get_curve(PID_MODE_STANDARD, &g_eeprom_params.standard_curve);
  pid_get_custom_points(g_eeprom_params.custom_points);

  temp_get_cal_curve(cal_points, &g_eeprom_params.cal_enabled,
                     &g_eeprom_params.cal_ref_c);

  for(uint8_t i = 0U; i < TEMP_TC_CAL_POINTS; i++)
  {
    g_eeprom_params.cal_adc[i] = (float)cal_points[i].adc;
    g_eeprom_params.cal_temp[i] = cal_points[i].temp_c;
  }

  g_eeprom_params.stationary_s =
    (float)power_get_stationary_timeout_ms() / 1000.0f;
  g_eeprom_params.sleep_power_pct =
    (float)power_get_low_power_threshold();
  appcolors_get_all(g_eeprom_params.app_colors);
  g_eeprom_params.reduced_temp_c = power_get_reduced_temperature();
  g_eeprom_params.reduced_off_s =
    (float)power_get_reduced_off_timeout_ms() / 1000.0f;
  g_eeprom_params.backlight_off_s =
    (float)power_get_backlight_off_timeout_ms() / 1000.0f;

  power_get_voltage_limits(&low_v, &high_v);
  g_eeprom_params.low_voltage_v = low_v;
  g_eeprom_params.high_voltage_v = high_v;
  g_eeprom_params.voltage_hysteresis_v = power_get_voltage_hysteresis();
  g_eeprom_params.voltage_trip_ms =
    (float)power_get_voltage_trip_time_ms();
  g_eeprom_params.voltage_recovery_ms =
    (float)power_get_voltage_recovery_time_ms();

  g_eeprom_params.ch224a_index =
    eeprom_ch224a_index_from_voltage(power_get_ch224a_voltage());

  g_eeprom_params.autoroll_s =
    (float)autoroll_get_interval() / 1000.0f;
  g_eeprom_params.autoroll_enabled = autoroll_is_enabled();
  g_eeprom_params.buzzer_enabled = buzzer_is_enabled();
  g_eeprom_params.buzzer_volume = (float)buzzer_get_volume();

  verifyheater_get_config(&verify);
  g_eeprom_params.verify_overtemp_c = verify.overtemp_c;
  g_eeprom_params.verify_jump_c = verify.temp_jump_delta_c;
  g_eeprom_params.verify_jump_rate = verify.temp_jump_rate_c_per_s;
  g_eeprom_params.verify_no_rise_s =
    (float)verify.no_rise_timeout_ms / 1000.0f;
  g_eeprom_params.verify_no_rise_delta_c = verify.no_rise_delta_c;
  g_eeprom_params.verify_full_power_s =
    (float)verify.full_power_timeout_ms / 1000.0f;
  g_eeprom_params.verify_drop_c = verify.temp_drop_delta_c;
}

void eeprom_apply(void)
{
  temp_tc_cal_point_t cal_points[TEMP_TC_CAL_POINTS];
  verifyheater_config_t verify;
  sm_status_t status;
  uint8_t ch224a_current_index;

  sm_get_status(&status);

  display_set_light_theme(g_eeprom_params.ui_light);
  appcolors_set(g_eeprom_params.app_colors);

  /* 整定过程中不要改设定温度/PID/标定，避免破坏整定 */
  if(status.autotune_state != PID_AUTOTUNE_RUNNING)
  {
    sm_set_setpoint(g_eeprom_params.setpoint_c);
    pid_set_curve(PID_MODE_STANDARD, &g_eeprom_params.standard_curve);
    (void)pid_set_custom_points(g_eeprom_params.custom_points);
    (void)sm_set_pid_mode((pid_mode_t)g_eeprom_params.pid_mode);

    for(uint8_t i = 0U; i < TEMP_TC_CAL_POINTS; i++)
    {
      cal_points[i].adc = (uint16_t)(g_eeprom_params.cal_adc[i] + 0.5f);
      cal_points[i].temp_c = g_eeprom_params.cal_temp[i];
    }

    (void)temp_set_cal_curve(cal_points, g_eeprom_params.cal_enabled,
                             g_eeprom_params.cal_ref_c);
  }

  power_set_stationary_timeout_ms(
    (uint32_t)((g_eeprom_params.stationary_s * 1000.0f) + 0.5f));
  power_set_low_power_threshold(
    (uint8_t)(g_eeprom_params.sleep_power_pct + 0.5f));
  power_set_reduced_temperature(g_eeprom_params.reduced_temp_c);
  power_set_reduced_off_timeout_ms(
    (uint32_t)((g_eeprom_params.reduced_off_s * 1000.0f) + 0.5f));
  power_set_backlight_off_timeout_ms(
    (uint32_t)((g_eeprom_params.backlight_off_s * 1000.0f) + 0.5f));

  power_set_voltage_limits(g_eeprom_params.low_voltage_v,
                           g_eeprom_params.high_voltage_v);
  power_set_voltage_hysteresis(g_eeprom_params.voltage_hysteresis_v);
  power_set_voltage_trip_time_ms(
    (uint32_t)(g_eeprom_params.voltage_trip_ms + 0.5f));
  power_set_voltage_recovery_time_ms(
    (uint32_t)(g_eeprom_params.voltage_recovery_ms + 0.5f));

  ch224a_current_index = eeprom_ch224a_index_from_voltage(
    power_get_ch224a_voltage());

  if((g_eeprom_params.ch224a_index <= 5U) &&
     (g_eeprom_params.ch224a_index != ch224a_current_index))
  {
    (void)power_ch224a_set_voltage_index(g_eeprom_params.ch224a_index);
  }

  autoroll_set_interval(
    (uint32_t)((g_eeprom_params.autoroll_s * 1000.0f) + 0.5f));
  autoroll_enable(g_eeprom_params.autoroll_enabled);
  buzzer_set_enabled(g_eeprom_params.buzzer_enabled);
  buzzer_set_volume((uint8_t)(g_eeprom_params.buzzer_volume + 0.5f));

  verify.overtemp_c = g_eeprom_params.verify_overtemp_c;
  verify.temp_jump_delta_c = g_eeprom_params.verify_jump_c;
  verify.temp_jump_rate_c_per_s = g_eeprom_params.verify_jump_rate;
  verify.no_rise_timeout_ms =
    (uint32_t)((g_eeprom_params.verify_no_rise_s * 1000.0f) + 0.5f);
  verify.no_rise_delta_c = g_eeprom_params.verify_no_rise_delta_c;
  verify.full_power_timeout_ms =
    (uint32_t)((g_eeprom_params.verify_full_power_s * 1000.0f) + 0.5f);
  verify.temp_drop_delta_c = g_eeprom_params.verify_drop_c;
  (void)verifyheater_set_config(&verify);
}

void eeprom_init(void)
{
  const eeprom_record_t *record0 = eeprom_record_at(EEPROM_SECTOR0_ADDR);
  const eeprom_record_t *record1 = eeprom_record_at(EEPROM_SECTOR1_ADDR);
  eeprom_record_status_t status0 =
    eeprom_record_status(record0, EEPROM_SECTOR0_ADDR);
  eeprom_record_status_t status1 =
    eeprom_record_status(record1, EEPROM_SECTOR1_ADDR);
  uint8_t valid0 = (status0 == EEPROM_RECORD_VALID) ? 1U : 0U;
  uint8_t valid1 = (status1 == EEPROM_RECORD_VALID) ? 1U : 0U;
  const eeprom_record_t *newest = NULL;

  eeprom_error = EEPROM_ERROR_NONE;

  if((valid0 != 0U) && (valid1 != 0U))
  {
    newest = (record0->sequence >= record1->sequence) ? record0 : record1;
  }
  else if(valid0 != 0U)
  {
    newest = record0;
  }
  else if(valid1 != 0U)
  {
    newest = record1;
  }

  if((newest != NULL) && (eeprom_params_sane(&newest->params) != 0U))
  {
    g_eeprom_params = newest->params;
    eeprom_loaded_valid = 1U;
    eeprom_error = EEPROM_ERROR_NONE;
  }
  else
  {
    eeprom_capture_current();
    eeprom_loaded_valid = 0U;

    /* 无有效记录：OneKeyBoost/步进默认值 */
    g_eeprom_params.boost_offset_c = 100.0f;
    g_eeprom_params.boost_drop_c = 20.0f;
    g_eeprom_params.boost_timeout_s = 300.0f;
    g_eeprom_params.setpoint_step_c = 5.0f;
    g_eeprom_params.time_step_s = 5.0f;
    g_eeprom_params.maindash_alt = 0U;
    g_eeprom_params.ui_light = 0U;
    g_eeprom_params.sleep_power_pct = 13.0f;
    g_eeprom_params.bootlogo_delay_s = 0.0f;
    g_eeprom_params.app_colors[0] = 0xFC9FU;
    g_eeprom_params.app_colors[1] = 0x05FFU;
    g_eeprom_params.app_colors[2] = 0x9CF3U;
    g_eeprom_params.app_colors[3] = 0xFFFFU;
    g_eeprom_params.app_colors[4] = 0x0861U;

    if((newest != NULL) && (eeprom_params_sane(&newest->params) == 0U))
    {
      eeprom_error = EEPROM_ERROR_CORRUPT;
    }
    else if((status0 == EEPROM_RECORD_CORRUPT) ||
            (status1 == EEPROM_RECORD_CORRUPT))
    {
      eeprom_error = EEPROM_ERROR_CORRUPT;
    }
    else if((status0 == EEPROM_RECORD_OLD_VERSION) ||
            (status1 == EEPROM_RECORD_OLD_VERSION))
    {
      eeprom_error = EEPROM_ERROR_VERSION;
    }
    else
    {
      eeprom_error = EEPROM_ERROR_NO_RECORD;
    }

    /*
     * CRC 损坏但 Flash 仍可写：立即把默认值写回，属于可恢复错误。
     * 只有写回也失败时才进入 HMS EEPROM WRITE。
     */
    if(eeprom_error == EEPROM_ERROR_CORRUPT)
    {
      if(eeprom_save() != 0U)
      {
        eeprom_error = EEPROM_ERROR_NONE;
        hms_show_eeprom_recovered();
      }
    }
  }
}

static uint8_t eeprom_flash_write_record(uint32_t address,
                                         const eeprom_record_t *record)
{
  const uint32_t *words = (const uint32_t *)record;
  uint32_t word_count = sizeof(eeprom_record_t) / 4U;
  flash_status_type status = FLASH_OPERATE_DONE;
  uint32_t primask;
  uint8_t result = 0U;

  heater_force_off();

  primask = __get_PRIMASK();
  __disable_irq();

  heater_force_off();

  flash_unlock();
  flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);

  status = flash_sector_erase(address);

  if(status == FLASH_OPERATE_DONE)
  {
    for(uint32_t i = 0U; i < word_count; i++)
    {
      status = flash_word_program(address + (i * 4U), words[i]);

      if(status != FLASH_OPERATE_DONE)
      {
        break;
      }
    }
  }

  if(status != FLASH_OPERATE_DONE)
  {
    flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
  }

  flash_lock();

  if(primask == 0U)
  {
    __enable_irq();
  }

  if(status == FLASH_OPERATE_DONE)
  {
    const eeprom_record_t *written = eeprom_record_at(address);

    if((memcmp(written, record, sizeof(eeprom_record_t)) == 0) &&
       (eeprom_record_valid(written) != 0U))
    {
      result = 1U;
    }
  }

  return result;
}

uint8_t eeprom_save(void)
{
  const eeprom_record_t *record0 = eeprom_record_at(EEPROM_SECTOR0_ADDR);
  const eeprom_record_t *record1 = eeprom_record_at(EEPROM_SECTOR1_ADDR);
  uint8_t valid0 = eeprom_record_valid(record0);
  uint8_t valid1 = eeprom_record_valid(record1);
  uint32_t sequence = 1U;
  uint32_t target_address;
  uint32_t crc;
  sm_status_t status;

  sm_get_status(&status);

  if(status.autotune_state == PID_AUTOTUNE_RUNNING)
  {
    return 0U;
  }

  crc = eeprom_crc32(&g_eeprom_params, sizeof(eeprom_params_t));

  /* 参数没有变化时不擦写 Flash */
  if((valid0 != 0U) && (valid1 != 0U))
  {
    const eeprom_record_t *newest =
      (record0->sequence >= record1->sequence) ? record0 : record1;

    if((newest->crc == crc) &&
       (memcmp(&newest->params, &g_eeprom_params,
               sizeof(eeprom_params_t)) == 0))
    {
      eeprom_loaded_valid = 1U;
      eeprom_error = EEPROM_ERROR_NONE;
      return 1U;
    }
  }
  else if((valid0 != 0U) &&
          (record0->crc == crc) &&
          (memcmp(&record0->params, &g_eeprom_params,
                  sizeof(eeprom_params_t)) == 0))
  {
    eeprom_loaded_valid = 1U;
    eeprom_error = EEPROM_ERROR_NONE;
    return 1U;
  }
  else if((valid1 != 0U) &&
          (record1->crc == crc) &&
          (memcmp(&record1->params, &g_eeprom_params,
                  sizeof(eeprom_params_t)) == 0))
  {
    eeprom_loaded_valid = 1U;
    eeprom_error = EEPROM_ERROR_NONE;
    return 1U;
  }

  if((valid0 != 0U) && (valid1 != 0U))
  {
    target_address = (record0->sequence <= record1->sequence) ?
                     EEPROM_SECTOR0_ADDR : EEPROM_SECTOR1_ADDR;
    sequence = ((record0->sequence > record1->sequence) ?
                record0->sequence : record1->sequence) + 1U;
  }
  else if(valid0 != 0U)
  {
    target_address = EEPROM_SECTOR1_ADDR;
    sequence = record0->sequence + 1U;
  }
  else if(valid1 != 0U)
  {
    target_address = EEPROM_SECTOR0_ADDR;
    sequence = record1->sequence + 1U;
  }
  else
  {
    target_address = EEPROM_SECTOR0_ADDR;
    sequence = 1U;
  }

  if(sequence == 0U)
  {
    sequence = 1U;
  }

  memset(&eeprom_work, 0, sizeof(eeprom_work));
  eeprom_work.magic = EEPROM_MAGIC;
  eeprom_work.version = EEPROM_FORMAT_VERSION;
  eeprom_work.size = sizeof(eeprom_params_t);
  eeprom_work.sequence = sequence;
  eeprom_work.crc = crc;
  eeprom_work.params = g_eeprom_params;
  eeprom_work.commit = EEPROM_COMMIT_MARK;

  if(eeprom_flash_write_record(target_address, &eeprom_work) != 0U)
  {
    eeprom_loaded_valid = 1U;
    eeprom_error = EEPROM_ERROR_NONE;
    return 1U;
  }

  eeprom_error = EEPROM_ERROR_WRITE;
  hms_set_fatal(HMS_CODE_EEPROM_WRITE);
  return 0U;
}

void eeprom_factory_reset(void)
{
  flash_status_type status0;
  flash_status_type status1;
  uint32_t primask;

  heater_force_off();

  primask = __get_PRIMASK();
  __disable_irq();

  heater_force_off();
  flash_unlock();
  flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);

  status0 = flash_sector_erase(EEPROM_SECTOR0_ADDR);
  status1 = flash_sector_erase(EEPROM_SECTOR1_ADDR);

  if((status0 != FLASH_OPERATE_DONE) || (status1 != FLASH_OPERATE_DONE))
  {
    flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
  }

  flash_lock();

  if(primask == 0U)
  {
    __enable_irq();
  }

  if((status0 != FLASH_OPERATE_DONE) || (status1 != FLASH_OPERATE_DONE))
  {
    eeprom_error = EEPROM_ERROR_WRITE;
    hms_set_fatal(HMS_CODE_EEPROM_WRITE);
    return;
  }

  eeprom_loaded_valid = 0U;
  eeprom_error = EEPROM_ERROR_NO_RECORD;

  NVIC_SystemReset();
}

void eeprom_task(uint32_t now_ms)
{
  static pid_autotune_state_t last_state = PID_AUTOTUNE_IDLE;
  static uint32_t last_check_ms;
  sm_status_t status;

  if((uint32_t)(now_ms - last_check_ms) < 1000U)
  {
    return;
  }

  last_check_ms = now_ms;
  sm_get_status(&status);

  if((last_state == PID_AUTOTUNE_RUNNING) &&
     (status.autotune_state == PID_AUTOTUNE_DONE))
  {
    eeprom_capture_current();
    if(eeprom_save() == 0U)
    {
      buzzer_play(BUZZER_SOUND_ERROR);
    }
  }

  last_state = status.autotune_state;
}
