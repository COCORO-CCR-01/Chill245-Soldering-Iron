/**
  **************************************************************************
  * @file     powermanagement.c
  * @brief    输入电压、震动休眠、电源保护和 CH224A 电压诱骗
  **************************************************************************
  * 硬件：
  *   PA2  -> V-ADC，V-ADC = VCC * 1/(10+1)，VCC = V-ADC * 11
  *   PB1  -> 震动开关 SLEEP，3V3 -> 10k -> PB1 -> 开关 -> GND
  *   PB6  -> I2C1_SCL -> CH224A
  *   PB7  -> I2C1_SDA -> CH224A
  *
  * 休眠流程：
  *   ACTIVE --静止超时--> REDUCED --继续静止超时--> HEATER_OFF
  *          --继续静止超时--> BACKLIGHT_OFF
  *
  * REDUCED 状态检测到震动可唤醒；HEATER_OFF/BACKLIGHT_OFF 只能按键唤醒。
  **************************************************************************
  */

#include "powermanagement.h"
#include "at32f421_conf.h"
#include "buttons.h"
#include "display.h"
#include "statemachine.h"
#include "wk_system.h"
#include <stddef.h>

#define POWER_VOLTAGE_SAMPLE_MS         (10U)
#define POWER_VIBRATION_DEBOUNCE_MS     (30U)
#define POWER_ACTIVITY_POWER_MIN        (30U)
#define POWER_ACTIVITY_ERROR_C          (5.0f)
#define POWER_I2C_TIMEOUT               (100000U)
#define POWER_MIN_TIMEOUT_MS            (1000U)

static power_state_t power_state;
static power_fault_t power_fault;
static power_fault_callback_t power_fault_callback;

static uint32_t power_stationary_timeout_ms;
static float power_reduced_temp_c;
static uint32_t power_reduced_off_timeout_ms;
static uint32_t power_backlight_off_timeout_ms;

static uint16_t power_low_voltage_mv;
static uint16_t power_high_voltage_mv;
static uint16_t power_voltage_hysteresis_mv;
static uint32_t power_voltage_trip_ms;
static uint32_t power_voltage_recovery_ms;

static uint16_t power_voltage_mv;
static uint32_t power_last_voltage_sample_ms;
static uint32_t power_voltage_bad_since_ms;
static uint32_t power_voltage_good_since_ms;

static uint32_t power_last_motion_ms;
static uint32_t power_vibration_latch_ms;
static uint32_t power_vibration_raw_change_ms;
static uint32_t power_state_enter_ms;
static uint8_t power_vibration_raw_level;
static uint8_t power_vibration_stable_level;
static uint8_t power_sleep_heater_was_enabled;

static float power_saved_setpoint_c;
static uint8_t power_saved_setpoint_valid;

static uint8_t power_fault_heater_was_enabled;
static uint8_t power_wake_pending;
static uint8_t power_initialized;

static uint8_t power_ch224a_found;
static uint8_t power_ch224a_address;
static float power_ch224a_voltage_v;

static uint8_t power_read_vibration_level(void);

static const float power_ch224a_voltage_table[6] =
{
  5.0f, 9.0f, 12.0f, 15.0f, 20.0f, 28.0f
};

/* ------------------------------------------------------------------------- */
/* CH224A I2C                                                                 */
/* ------------------------------------------------------------------------- */

static uint8_t power_i2c_wait_flag(uint32_t flag, uint32_t timeout)
{
  while(i2c_flag_get(I2C1, flag) == RESET)
  {
    if(i2c_flag_get(I2C1, I2C_ACKFAIL_FLAG) != RESET)
    {
      i2c_stop_generate(I2C1);
      i2c_flag_clear(I2C1, I2C_ACKFAIL_FLAG);
      wk_delay_us(10U);
      return 0U;
    }

    if(timeout == 0U)
    {
      i2c_stop_generate(I2C1);
      wk_delay_us(10U);
      return 0U;
    }

    timeout--;
  }

  return 1U;
}

static uint8_t power_ch224a_write_reg(uint8_t address, uint8_t reg, uint8_t value)
{
  uint32_t timeout;

  if(power_initialized == 0U)
  {
    return 0U;
  }

  timeout = POWER_I2C_TIMEOUT;

  while(i2c_flag_get(I2C1, I2C_BUSYF_FLAG) != RESET)
  {
    if(timeout == 0U)
    {
      return 0U;
    }

    timeout--;
  }

  i2c_start_generate(I2C1);

  if(power_i2c_wait_flag(I2C_STARTF_FLAG, POWER_I2C_TIMEOUT) == 0U)
  {
    return 0U;
  }

  i2c_7bit_address_send(I2C1, address, I2C_DIRECTION_TRANSMIT);

  if(power_i2c_wait_flag(I2C_ADDR7F_FLAG, POWER_I2C_TIMEOUT) == 0U)
  {
    return 0U;
  }

  i2c_flag_clear(I2C1, I2C_ADDR7F_FLAG);

  if(power_i2c_wait_flag(I2C_TDBE_FLAG, POWER_I2C_TIMEOUT) == 0U)
  {
    return 0U;
  }

  i2c_data_send(I2C1, reg);

  if(power_i2c_wait_flag(I2C_TDBE_FLAG, POWER_I2C_TIMEOUT) == 0U)
  {
    return 0U;
  }

  i2c_data_send(I2C1, value);

  if(power_i2c_wait_flag(I2C_TDC_FLAG, POWER_I2C_TIMEOUT) == 0U)
  {
    return 0U;
  }

  i2c_stop_generate(I2C1);
  wk_delay_us(10U);
  return 1U;
}

static uint8_t power_ch224a_find(void)
{
  /* 先写 20V 作为探测，同时把默认电压设好 */
  if(power_ch224a_write_reg(POWER_CH224A_I2C_ADDR_PRIMARY,
                            POWER_CH224A_REG_VOLTAGE, 4U) != 0U)
  {
    power_ch224a_address = POWER_CH224A_I2C_ADDR_PRIMARY;
    power_ch224a_found = 1U;
    return 1U;
  }

  if(power_ch224a_write_reg(POWER_CH224A_I2C_ADDR_SECONDARY,
                            POWER_CH224A_REG_VOLTAGE, 4U) != 0U)
  {
    power_ch224a_address = POWER_CH224A_I2C_ADDR_SECONDARY;
    power_ch224a_found = 1U;
    return 1U;
  }

  power_ch224a_found = 0U;
  return 0U;
}

uint8_t power_ch224a_set_voltage_index(uint8_t index)
{
  if(index > 5U)
  {
    return 0U;
  }

  if(power_ch224a_found == 0U)
  {
    if(power_ch224a_find() == 0U)
    {
      return 0U;
    }
  }

  if(power_ch224a_write_reg(power_ch224a_address,
                            POWER_CH224A_REG_VOLTAGE, index) == 0U)
  {
    power_ch224a_found = 0U;
    return 0U;
  }

  power_ch224a_voltage_v = power_ch224a_voltage_table[index];
  return 1U;
}

uint8_t power_ch224a_set_voltage(float volts)
{
  uint8_t index;

  if(volts <= 5.0f)
  {
    index = 0U;   /* 5V */
  }
  else if(volts <= 9.0f)
  {
    index = 1U;   /* 9V */
  }
  else if(volts <= 12.0f)
  {
    index = 2U;   /* 12V */
  }
  else if(volts <= 15.0f)
  {
    index = 3U;   /* 15V */
  }
  else if(volts <= 20.0f)
  {
    index = 4U;   /* 20V */
  }
  else if(volts <= 28.0f)
  {
    index = 5U;   /* 28V */
  }
  else
  {
    return 0U;
  }

  /* CH224A 固定档位没有 24V；24V 请求会落到 20V 档 */
  return power_ch224a_set_voltage_index(index);
}

/* ------------------------------------------------------------------------- */
/* 电压、休眠、保护                                                           */
/* ------------------------------------------------------------------------- */

uint16_t power_vin_raw_to_mv(uint16_t raw)
{
  uint32_t mv;

  mv = ((uint32_t)raw * POWER_VIN_VREF_MV *
        (POWER_VIN_DIVIDER_R1 + POWER_VIN_DIVIDER_R2)) /
       (POWER_ADC_FULL_SCALE * POWER_VIN_DIVIDER_R2);

  return (uint16_t)mv;
}

uint16_t power_get_voltage_mv(void)
{
  return power_voltage_mv;
}

float power_get_ch224a_voltage(void)
{
  return power_ch224a_voltage_v;
}

void power_notify_activity(uint32_t now_ms)
{
  uint8_t level = power_read_vibration_level();

  power_last_motion_ms = now_ms;
  power_vibration_raw_level = level;
  power_vibration_stable_level = level;
  power_vibration_raw_change_ms = now_ms;
}

power_state_t power_get_state(void)
{
  return power_state;
}

uint8_t power_is_sleeping(void)
{
  return ((power_state == POWER_STATE_HEATER_OFF) ||
          (power_state == POWER_STATE_BACKLIGHT_OFF)) ? 1U : 0U;
}

uint8_t power_get_vibration_state(void)
{
  uint32_t now_ms = sm_get_uptime_ms();

  /* 只有稳定低电平才算震动，避免悬空/抖动误触发 */
  if(power_vibration_stable_level == 0U)
  {
    return 1U;
  }

  return ((int32_t)(power_vibration_latch_ms - now_ms) > 0) ? 1U : 0U;
}

uint32_t power_get_sleep_remaining_ms(void)
{
  uint32_t now_ms = sm_get_uptime_ms();
  uint32_t elapsed;
  uint32_t timeout;

  switch(power_state)
  {
    case POWER_STATE_ACTIVE:
      timeout = power_stationary_timeout_ms;
      elapsed = (uint32_t)(now_ms - power_last_motion_ms);
      break;

    case POWER_STATE_REDUCED:
      timeout = power_reduced_off_timeout_ms;
      elapsed = (uint32_t)(now_ms - power_state_enter_ms);
      break;

    case POWER_STATE_HEATER_OFF:
      timeout = power_backlight_off_timeout_ms;
      elapsed = (uint32_t)(now_ms - power_state_enter_ms);
      break;

    case POWER_STATE_BACKLIGHT_OFF:
    default:
      return 0U;
  }

  return (elapsed >= timeout) ? 0U : (timeout - elapsed);
}

power_fault_t power_get_fault(void)
{
  return power_fault;
}

void power_register_fault_callback(power_fault_callback_t callback)
{
  power_fault_callback = callback;
}

static uint8_t power_read_vibration_level(void)
{
  return (gpio_input_data_bit_read(GPIOB, GPIO_PINS_1) == SET) ? 1U : 0U;
}

static void power_restore_setpoint(void)
{
  if(power_saved_setpoint_valid != 0U)
  {
    (void)sm_set_setpoint(power_saved_setpoint_c);
    power_saved_setpoint_valid = 0U;
  }
}

static void power_enter_reduced(uint32_t now_ms)
{
  sm_status_t status;
  float saved;

  sm_get_status(&status);

  /* 整定过程中不要改目标温度 */
  if(status.autotune_state == PID_AUTOTUNE_RUNNING)
  {
    power_last_motion_ms = now_ms;
    return;
  }

  saved = sm_get_setpoint();
  power_saved_setpoint_c = saved;
  power_saved_setpoint_valid = 1U;

  if(power_reduced_temp_c < saved)
  {
    if(sm_set_setpoint(power_reduced_temp_c) == 0U)
    {
      power_saved_setpoint_valid = 0U;
      power_last_motion_ms = now_ms;
      return;
    }
  }

  power_state = POWER_STATE_REDUCED;
  power_state_enter_ms = now_ms;
}

static void power_enter_heater_off(uint32_t now_ms)
{
  power_sleep_heater_was_enabled = sm_is_heater_enabled();
  (void)sm_set_heater_enabled(0U);
  power_state = POWER_STATE_HEATER_OFF;
  power_state_enter_ms = now_ms;
}

static void power_enter_backlight_off(uint32_t now_ms)
{
  if(power_state != POWER_STATE_HEATER_OFF)
  {
    power_sleep_heater_was_enabled = sm_is_heater_enabled();
  }

  display_backlight_set(0U);
  power_state = POWER_STATE_BACKLIGHT_OFF;
  power_state_enter_ms = now_ms;
}

static void power_wake_now(uint32_t now_ms)
{
  display_backlight_set(1U);

  power_last_motion_ms = now_ms;
  power_state_enter_ms = now_ms;
  power_vibration_raw_level = power_read_vibration_level();
  power_vibration_stable_level = power_vibration_raw_level;
  power_vibration_raw_change_ms = now_ms;

  if(power_fault != POWER_FAULT_NONE)
  {
    power_wake_pending = 1U;
    power_restore_setpoint();
    power_state = POWER_STATE_ACTIVE;
    return;
  }

  if(power_state == POWER_STATE_REDUCED)
  {
    power_restore_setpoint();
  }
  else if((power_state == POWER_STATE_HEATER_OFF) ||
          (power_state == POWER_STATE_BACKLIGHT_OFF))
  {
    power_restore_setpoint();

    if(power_sleep_heater_was_enabled != 0U)
    {
      (void)sm_set_heater_enabled(1U);
    }

    power_sleep_heater_was_enabled = 0U;
  }

  power_state = POWER_STATE_ACTIVE;
}

void power_wake(void)
{
  power_wake_now(sm_get_uptime_ms());
}

static void power_sleep_common(uint8_t screen_off, uint8_t restore_heater)
{
  uint32_t now_ms;

  if(power_initialized == 0U)
  {
    return;
  }

  if(power_state == POWER_STATE_BACKLIGHT_OFF)
  {
    return;
  }

  now_ms = sm_get_uptime_ms();

  /* 手动休眠也要记住原设定温度，唤醒后恢复 */
  if(power_saved_setpoint_valid == 0U)
  {
    power_saved_setpoint_c = sm_get_setpoint();
    power_saved_setpoint_valid = 1U;
  }

  if(power_state != POWER_STATE_HEATER_OFF)
  {
    power_enter_heater_off(now_ms);
  }

  if(restore_heater == 0U)
  {
    power_sleep_heater_was_enabled = 0U;
  }

  if(screen_off != 0U)
  {
    power_enter_backlight_off(now_ms);
  }
}

void power_sleep(void)
{
  /* 菜单 SLEEP NOW：关加热+背光，唤醒恢复休眠前加热状态 */
  power_sleep_common(1U, 1U);
}

void power_heater_off(void)
{
  /* 短按 PREV：只关加热，屏幕保持，唤醒后不自动恢复加热 */
  power_sleep_common(0U, 0U);
}

void power_sleep_deep(void)
{
  /* 长按 PREV：关加热+屏幕，唤醒后加热保持关闭 */
  power_sleep_common(1U, 0U);
}

static void power_latch_fault(power_fault_t fault, uint16_t voltage_mv)
{
  power_fault = fault;
  power_voltage_bad_since_ms = 0U;
  power_voltage_good_since_ms = 0U;

  power_fault_heater_was_enabled = sm_is_heater_enabled();
  (void)sm_set_heater_enabled(0U);

  if(power_fault_callback != NULL)
  {
    power_fault_callback(fault, voltage_mv);
  }
}

uint8_t power_clear_fault(void)
{
  uint8_t restore_heater;

  if(power_fault == POWER_FAULT_NONE)
  {
    return 1U;
  }

  if((power_voltage_mv <=
      (uint16_t)(power_low_voltage_mv + power_voltage_hysteresis_mv)) ||
     (power_voltage_mv >=
      (uint16_t)(power_high_voltage_mv - power_voltage_hysteresis_mv)))
  {
    return 0U;
  }

  restore_heater = ((power_fault_heater_was_enabled != 0U) ||
                    (power_wake_pending != 0U)) ? 1U : 0U;

  power_fault = POWER_FAULT_NONE;
  power_voltage_bad_since_ms = 0U;
  power_voltage_good_since_ms = 0U;
  power_fault_heater_was_enabled = 0U;
  power_wake_pending = 0U;

  if((restore_heater != 0U) &&
     ((power_state == POWER_STATE_ACTIVE) ||
      (power_state == POWER_STATE_REDUCED)))
  {
    (void)sm_set_heater_enabled(1U);
  }

  return 1U;
}

static void power_voltage_task(uint32_t now_ms)
{
  sm_status_t status;

  if((uint32_t)(now_ms - power_last_voltage_sample_ms) <
     POWER_VOLTAGE_SAMPLE_MS)
  {
    return;
  }

  power_last_voltage_sample_ms = now_ms;

  sm_get_status(&status);

  if(status.adc_valid == 0U)
  {
    power_voltage_bad_since_ms = 0U;
    power_voltage_good_since_ms = 0U;
    return;
  }

  /* 使用状态判定：正在大功率加热或明显偏离设定温度，视为有人使用 */
  if(status.temp_valid != 0U)
  {
    float error_c = status.tip_c - status.setpoint_c;

    if(error_c < 0.0f)
    {
      error_c = -error_c;
    }

    if((status.power >= POWER_ACTIVITY_POWER_MIN) ||
       (error_c >= POWER_ACTIVITY_ERROR_C))
    {
      power_notify_activity(now_ms);
    }
  }

  power_voltage_mv = power_vin_raw_to_mv(status.vin_raw);

  /* 上电/CH224A 重新协商期间不触发保护 */
  if(now_ms < POWER_VOLTAGE_STARTUP_GRACE_MS)
  {
    power_voltage_bad_since_ms = 0U;
    power_voltage_good_since_ms = 0U;
    return;
  }

  if(power_fault == POWER_FAULT_NONE)
  {
    if((power_voltage_mv < power_low_voltage_mv) ||
       (power_voltage_mv > power_high_voltage_mv))
    {
      if(power_voltage_bad_since_ms == 0U)
      {
        power_voltage_bad_since_ms = now_ms;
      }
      else if((uint32_t)(now_ms - power_voltage_bad_since_ms) >=
              power_voltage_trip_ms)
      {
        if(power_voltage_mv < power_low_voltage_mv)
        {
          power_latch_fault(POWER_FAULT_UNDERVOLTAGE, power_voltage_mv);
        }
        else
        {
          power_latch_fault(POWER_FAULT_OVERVOLTAGE, power_voltage_mv);
        }
      }
    }
    else
    {
      power_voltage_bad_since_ms = 0U;
    }
  }
  else
  {
    if((power_voltage_mv >
        (uint16_t)(power_low_voltage_mv + power_voltage_hysteresis_mv)) &&
       (power_voltage_mv <
        (uint16_t)(power_high_voltage_mv - power_voltage_hysteresis_mv)))
    {
      if(power_voltage_good_since_ms == 0U)
      {
        power_voltage_good_since_ms = now_ms;
      }
      else if((uint32_t)(now_ms - power_voltage_good_since_ms) >=
              power_voltage_recovery_ms)
      {
        (void)power_clear_fault();
      }
    }
    else
    {
      power_voltage_good_since_ms = 0U;
    }
  }
}

static uint8_t power_next_wake_click;

static void power_on_button(button_id_t button, button_event_t event)
{
  (void)button;

  if(event == BUTTON_EVENT_PRESS)
  {
    power_wake();
  }
}

/* 休眠时短按 NEXT 唤醒；NEXT 不再用 PRESS 唤醒，避免按下即翻页 */
static void power_on_next_click(button_id_t button, button_event_t event)
{
  (void)button;

  if((event == BUTTON_EVENT_CLICK) && (power_is_sleeping() != 0U))
  {
    power_next_wake_click = 1U;
    power_wake();
  }
}

uint8_t power_take_wake_click(void)
{
  uint8_t pending = power_next_wake_click;

  power_next_wake_click = 0U;
  return pending;
}

void power_init(void)
{
  gpio_init_type gpio_init_struct;

  power_stationary_timeout_ms = POWER_DEFAULT_STATIONARY_TIMEOUT_MS;
  power_reduced_temp_c = POWER_DEFAULT_REDUCED_TEMP_C;
  power_reduced_off_timeout_ms = POWER_DEFAULT_REDUCED_OFF_TIMEOUT_MS;
  power_backlight_off_timeout_ms = POWER_DEFAULT_BACKLIGHT_OFF_TIMEOUT_MS;

  power_low_voltage_mv = POWER_DEFAULT_LOW_VOLTAGE_MV;
  power_high_voltage_mv = POWER_DEFAULT_HIGH_VOLTAGE_MV;
  power_voltage_hysteresis_mv = POWER_DEFAULT_VOLTAGE_HYSTERESIS_MV;
  power_voltage_trip_ms = POWER_DEFAULT_VOLTAGE_TRIP_MS;
  power_voltage_recovery_ms = POWER_DEFAULT_VOLTAGE_RECOVERY_MS;

  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

  /* PB1 震动开关，外部 10k 上拉，静止为高 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = GPIO_PINS_1;
  gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init(GPIOB, &gpio_init_struct);

  /* CH224A I2C1：PB6=SCL，PB7=SDA，MUX_1，外部 1k 上拉 */
  crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, TRUE);

  gpio_init_struct.gpio_pins = GPIO_PINS_6 | GPIO_PINS_7;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOB, &gpio_init_struct);

  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE6, GPIO_MUX_1);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE7, GPIO_MUX_1);

  i2c_init(I2C1, I2C_FSMODE_DUTY_2_1, 100000);
  i2c_own_address1_set(I2C1, I2C_ADDRESS_MODE_7BIT, 0xA0);
  i2c_enable(I2C1, TRUE);

  power_ch224a_found = 0U;
  power_ch224a_address = POWER_CH224A_I2C_ADDR_PRIMARY;
  power_ch224a_voltage_v = POWER_CH224A_DEFAULT_VOLTAGE_V;

  power_state = POWER_STATE_ACTIVE;
  power_fault = POWER_FAULT_NONE;
  power_voltage_mv = 0U;
  power_last_voltage_sample_ms = 0U;
  power_voltage_bad_since_ms = 0U;
  power_voltage_good_since_ms = 0U;
  power_last_motion_ms = sm_get_uptime_ms();
  power_state_enter_ms = power_last_motion_ms;
  power_vibration_raw_level = power_read_vibration_level();
  power_vibration_stable_level = power_vibration_raw_level;
  power_vibration_raw_change_ms = power_last_motion_ms;
  power_vibration_latch_ms = 0U;
  power_saved_setpoint_valid = 0U;
  power_fault_heater_was_enabled = 0U;
  power_wake_pending = 0U;

  power_initialized = 1U;

  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_PRESS,
                                  power_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_PRESS,
                                  power_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_CLICK,
                                  power_on_next_click);

  /* 默认 20V；CH224A 固定档位没有 24V，24V 请求会落到 20V */
  (void)power_ch224a_set_voltage(POWER_CH224A_DEFAULT_VOLTAGE_V);
}

void power_update(uint32_t now_ms)
{
  uint8_t level;
  uint8_t motion;

  if(power_initialized == 0U)
  {
    return;
  }

  power_voltage_task(now_ms);

  level = power_read_vibration_level();

  if(level != power_vibration_raw_level)
  {
    power_vibration_raw_level = level;
    power_vibration_raw_change_ms = now_ms;
  }

  motion = 0U;

  if(((uint32_t)(now_ms - power_vibration_raw_change_ms) >=
      POWER_VIBRATION_DEBOUNCE_MS) &&
     (power_vibration_stable_level != power_vibration_raw_level))
  {
    power_vibration_stable_level = power_vibration_raw_level;

    /* 只有稳定拉低才认为是一次真实震动 */
    if(power_vibration_stable_level == 0U)
    {
      motion = 1U;
      power_vibration_latch_ms = now_ms + 500U;
    }
  }

  if(power_fault != POWER_FAULT_NONE)
  {
    return;
  }

  switch(power_state)
  {
    case POWER_STATE_ACTIVE:
      if(motion != 0U)
      {
        power_last_motion_ms = now_ms;
      }

      if((uint32_t)(now_ms - power_last_motion_ms) >=
         power_stationary_timeout_ms)
      {
        power_enter_reduced(now_ms);
      }
      break;

    case POWER_STATE_REDUCED:
      if(motion != 0U)
      {
        power_wake_now(now_ms);
      }
      else if((uint32_t)(now_ms - power_state_enter_ms) >=
              power_reduced_off_timeout_ms)
      {
        power_enter_heater_off(now_ms);
      }
      break;

    case POWER_STATE_HEATER_OFF:
      if((uint32_t)(now_ms - power_state_enter_ms) >=
         power_backlight_off_timeout_ms)
      {
        power_enter_backlight_off(now_ms);
      }
      break;

    case POWER_STATE_BACKLIGHT_OFF:
    default:
      break;
  }
}

void power_set_stationary_timeout_ms(uint32_t ms)
{
  if(ms >= POWER_MIN_TIMEOUT_MS)
  {
    power_stationary_timeout_ms = ms;
  }
}

uint32_t power_get_stationary_timeout_ms(void)
{
  return power_stationary_timeout_ms;
}

float power_get_reduced_temperature(void)
{
  return power_reduced_temp_c;
}

void power_set_reduced_temperature(float celsius)
{
  if((celsius >= 50.0f) && (celsius <= 400.0f))
  {
    power_reduced_temp_c = celsius;

    if(power_state == POWER_STATE_REDUCED)
    {
      (void)sm_set_setpoint(celsius);
    }
  }
}

void power_set_reduced_off_timeout_ms(uint32_t ms)
{
  if(ms >= POWER_MIN_TIMEOUT_MS)
  {
    power_reduced_off_timeout_ms = ms;
  }
}

uint32_t power_get_reduced_off_timeout_ms(void)
{
  return power_reduced_off_timeout_ms;
}

uint32_t power_get_backlight_off_timeout_ms(void)
{
  return power_backlight_off_timeout_ms;
}

void power_set_backlight_off_timeout_ms(uint32_t ms)
{
  if(ms >= POWER_MIN_TIMEOUT_MS)
  {
    power_backlight_off_timeout_ms = ms;
  }
}

void power_set_voltage_limits(float low_v, float high_v)
{
  uint16_t low_mv;
  uint16_t high_mv;

  if((low_v <= 0.0f) || (high_v <= 0.0f) || (low_v >= high_v))
  {
    return;
  }

  low_mv = (uint16_t)(low_v * 1000.0f);
  high_mv = (uint16_t)(high_v * 1000.0f);

  if((low_mv < 5000U) || (high_mv > 40000U) ||
     ((uint16_t)(low_mv + 1000U) >= high_mv))
  {
    return;
  }

  power_low_voltage_mv = low_mv;
  power_high_voltage_mv = high_mv;

  if(power_voltage_hysteresis_mv >=
     (uint16_t)((high_mv - low_mv) / 2U))
  {
    power_voltage_hysteresis_mv =
      (uint16_t)((high_mv - low_mv) / 4U);
  }
}

void power_get_voltage_limits(float *low_v, float *high_v)
{
  if(low_v != NULL)
  {
    *low_v = (float)power_low_voltage_mv / 1000.0f;
  }

  if(high_v != NULL)
  {
    *high_v = (float)power_high_voltage_mv / 1000.0f;
  }
}

void power_set_voltage_hysteresis(float volts)
{
  uint16_t hysteresis_mv;

  if(volts < 0.0f)
  {
    return;
  }

  hysteresis_mv = (uint16_t)(volts * 1000.0f);

  if(hysteresis_mv <
     (uint16_t)((power_high_voltage_mv - power_low_voltage_mv) / 2U))
  {
    power_voltage_hysteresis_mv = hysteresis_mv;
  }
}

float power_get_voltage_hysteresis(void)
{
  return (float)power_voltage_hysteresis_mv / 1000.0f;
}

void power_set_voltage_trip_time_ms(uint32_t ms)
{
  if((ms >= 10U) && (ms <= 5000U))
  {
    power_voltage_trip_ms = ms;
  }
}

uint32_t power_get_voltage_trip_time_ms(void)
{
  return power_voltage_trip_ms;
}

void power_set_voltage_recovery_time_ms(uint32_t ms)
{
  if((ms >= 100U) && (ms <= 60000U))
  {
    power_voltage_recovery_ms = ms;
  }
}

uint32_t power_get_voltage_recovery_time_ms(void)
{
  return power_voltage_recovery_ms;
}
