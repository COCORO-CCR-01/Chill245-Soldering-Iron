#pragma GCC optimize("O2")
/**
  **************************************************************************
  * @file     heater.c
  * @brief    加热器 20 kHz 硬件 PWM（PA8 / TMR1_CH1，高电平导通）
  **************************************************************************
  * 硬件（Chill245 原理图）：
  *   功率管 Q6 = S-LP7413DT3WG，P 沟道 40V/47A，源极接 VCC，漏极接加热端
  *   栅极驱动：Q8 = 2N7002W（N 沟道，仅作驱动），Q5 = MMBT3904（NPN）
  *   PA8 -> TMR1_CH1 -> 栅极驱动网络 -> Q6 栅极
  *   PWM 高电平 -> Q6 导通
  *   PWM 低电平 -> Q6 关断
  *
  * TMR1 配置：
  *   - 计数时钟 120 MHz
  *   - ARR = 120 MHz / 20 kHz - 1 = 5999
  *   - CH1 PWM 模式 A，高电平有效
  *   - 0%   -> CCR = 0    -> PA8 恒低，Q6 关断
  *   - 100% -> CCR = 6000 -> PA8 恒高，Q6 导通
  **************************************************************************
  */

#include "at32f421_conf.h"
#include "heater.h"

#define HEATER_TMR                      TMR1
#define HEATER_TMR_CHANNEL              TMR_SELECT_CHANNEL_1
#define HEATER_GPIO_PORT                GPIOA
#define HEATER_GPIO_PIN                 GPIO_PINS_8
#define HEATER_GPIO_PIN_SOURCE          GPIO_PINS_SOURCE8
#define HEATER_GPIO_MUX                 GPIO_MUX_2

#define HEATER_PWM_FREQUENCY_HZ         (20000U)

static uint16_t heater_pwm_period;
static uint8_t heater_duty_percent;
static uint8_t heater_output_on;

/**
  * @brief  写 TMR1 CH1 比较值
  * @param  percent: 0~100
  */
static void heater_write_duty(uint8_t percent)
{
  uint32_t pulse;

  if(percent > 100U)
  {
    percent = 100U;
  }

  /* 0% -> 0，100% -> ARR+1，保证满占空比 */
  pulse = ((uint32_t)percent * ((uint32_t)heater_pwm_period + 1U)) / 100U;

  tmr_channel_value_set(HEATER_TMR, HEATER_TMR_CHANNEL, (uint16_t)pulse);

  heater_duty_percent = percent;
  heater_output_on = (percent > 0U) ? 1U : 0U;
}

void heater_init(void)
{
  gpio_init_type gpio_init_struct;
  tmr_output_config_type tmr_output_struct;
  crm_clocks_freq_type crm_clocks;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);

  /* 先把 PA8 置低，保证切换为复用功能前加热器关断 */
  gpio_bits_reset(HEATER_GPIO_PORT, HEATER_GPIO_PIN);

  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = HEATER_GPIO_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(HEATER_GPIO_PORT, &gpio_init_struct);
  gpio_bits_reset(HEATER_GPIO_PORT, HEATER_GPIO_PIN);

  crm_clocks_freq_get(&crm_clocks);
  heater_pwm_period = (uint16_t)((crm_clocks.sclk_freq / HEATER_PWM_FREQUENCY_HZ) - 1U);

  tmr_base_init(HEATER_TMR, heater_pwm_period, 0);
  tmr_cnt_dir_set(HEATER_TMR, TMR_COUNT_UP);
  tmr_clock_source_div_set(HEATER_TMR, TMR_CLOCK_DIV1);

  tmr_output_default_para_init(&tmr_output_struct);
  tmr_output_struct.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  tmr_output_struct.oc_output_state = TRUE;
  tmr_output_struct.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.oc_idle_state = FALSE;
  tmr_output_struct.occ_output_state = FALSE;
  tmr_output_struct.occ_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  tmr_output_struct.occ_idle_state = FALSE;

  tmr_output_channel_config(HEATER_TMR, HEATER_TMR_CHANNEL, &tmr_output_struct);
  tmr_channel_value_set(HEATER_TMR, HEATER_TMR_CHANNEL, 0U);
  tmr_output_enable(HEATER_TMR, TRUE);

  heater_duty_percent = 0U;
  heater_output_on = 0U;

  tmr_counter_enable(HEATER_TMR, TRUE);

  /* 定时器 CH1 先输出 0% 的低电平，再把 PA8 切到复用功能，
     避免复用瞬间出现高电平误导通。
     注意：gpio_pin_mux_config() 只写 MUX 寄存器，
     必须同时把 GPIO 模式改为 GPIO_MODE_MUX，TMR1_CH1 才会真正输出到 PA8。 */
  gpio_init_struct.gpio_pins = HEATER_GPIO_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(HEATER_GPIO_PORT, &gpio_init_struct);
  gpio_pin_mux_config(HEATER_GPIO_PORT, HEATER_GPIO_PIN_SOURCE, HEATER_GPIO_MUX);
}

void heater_set_power(uint8_t percent)
{
  heater_write_duty(percent);
}

uint8_t heater_get_power(void)
{
  return heater_duty_percent;
}

void heater_force_off(void)
{
  heater_write_duty(0U);
}

uint8_t heater_is_on(void)
{
  return heater_output_on;
}
