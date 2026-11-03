/**
  **************************************************************************
  * @file     led.c
  * @brief    PA6 运行指示灯
  **************************************************************************
  * 硬件：PA6 -> R66 10k -> LED2 -> GND，高电平点亮。
  * PA6 = TMR3_CH1，与蜂鸣器 TMR3_CH2 共用时基：
  *   - 蜂鸣器改变 ARR 时调用 led_refresh_pwm() 保持亮度比例
  *   - 运行：呼吸；待机/休眠：慢闪；故障/HMS：快闪
  **************************************************************************
  */

#include "led.h"
#include "at32f421_conf.h"
#include "statemachine.h"
#include "powermanagement.h"
#include "hms.h"

#define LED_TMR                         TMR3
#define LED_TMR_CHANNEL                 TMR_SELECT_CHANNEL_1
#define LED_PIN                         GPIO_PINS_6
#define LED_PIN_SOURCE                  GPIO_PINS_SOURCE6
#define LED_PIN_MUX                     GPIO_MUX_1
#define LED_TMR_CLOCK                   CRM_TMR3_PERIPH_CLOCK
#define LED_PRESCALER                   (119U)       /* 120 MHz / 120 = 1 MHz */
#define LED_BREATH_STEP_MS              (50U)
#define LED_BREATH_MAX                   (60U)
#define LED_BLINK_ON_MS                 (100U)
#define LED_BLINK_PERIOD_MS             (1500U)
#define LED_FAULT_ON_MS                 (100U)
#define LED_FAULT_PERIOD_MS             (250U)

typedef enum
{
  LED_MODE_BREATHE = 0,
  LED_MODE_BLINK,
  LED_MODE_FAULT
} led_mode_t;

static const uint8_t led_breath_table[32] =
{
  0U, 1U, 3U, 6U, 10U, 15U, 21U, 27U,
  33U, 38U, 44U, 49U, 53U, 56U, 58U, 60U,
  60U, 58U, 56U, 53U, 49U, 44U, 38U, 33U,
  27U, 21U, 15U, 10U, 6U, 3U, 1U, 0U
};

static uint8_t led_initialized;
static uint8_t led_duty;
static uint8_t led_breath_index;
static uint32_t led_last_step_ms;
static led_mode_t led_mode;
static uint8_t led_mode_valid;

static void led_apply_duty(uint8_t duty)
{
  uint32_t period;
  uint32_t compare;

  if(led_initialized == 0U)
  {
    return;
  }

  if(duty > 100U)
  {
    duty = 100U;
  }

  if(duty == led_duty)
  {
    return;
  }

  led_duty = duty;
  period = (uint32_t)LED_TMR->pr + 1U;
  compare = (period * (uint32_t)duty) / 100U;

  if(compare > period)
  {
    compare = period;
  }

  tmr_channel_value_set(LED_TMR, LED_TMR_CHANNEL, (uint16_t)compare);
}

void led_refresh_pwm(void)
{
  if(led_initialized == 0U)
  {
    return;
  }

  /*
   * 直接按新 ARR 重算。先清零缓存再恢复，确保 led_apply_duty()
   * 不会因为“占空比没变”而跳过。
   */
  {
    uint8_t duty = led_duty;
    led_duty = 0xFFU;
    led_apply_duty(duty);
  }
}

void led_init(void)
{
  gpio_init_type gpio_init_struct;
  tmr_output_config_type output_config;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(LED_TMR_CLOCK, TRUE);

  /* PA6 先拉低，保证 LED 关闭，再切到 TMR3_CH1 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = LED_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOA, &gpio_init_struct);
  gpio_bits_reset(GPIOA, LED_PIN);

  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init(GPIOA, &gpio_init_struct);
  gpio_pin_mux_config(GPIOA, LED_PIN_SOURCE, LED_PIN_MUX);

  tmr_base_init(LED_TMR, 249U, LED_PRESCALER);
  tmr_cnt_dir_set(LED_TMR, TMR_COUNT_UP);
  tmr_clock_source_div_set(LED_TMR, TMR_CLOCK_DIV1);

  tmr_output_default_para_init(&output_config);
  output_config.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  output_config.oc_idle_state = FALSE;
  output_config.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  output_config.oc_output_state = TRUE;
  tmr_output_channel_config(LED_TMR, LED_TMR_CHANNEL, &output_config);
  tmr_channel_value_set(LED_TMR, LED_TMR_CHANNEL, 0U);
  tmr_counter_enable(LED_TMR, TRUE);

  led_initialized = 1U;
  led_duty = 0U;
  led_breath_index = 0U;
  led_last_step_ms = 0U;
  led_mode = LED_MODE_BLINK;
  led_mode_valid = 0U;
}

void led_task(uint32_t now_ms)
{
  led_mode_t mode;
  uint8_t duty;
  uint32_t phase;

  if(led_initialized == 0U)
  {
    return;
  }

  if((hms_has_fatal() != 0U) || (sm_get_state() == SM_STATE_FAULT))
  {
    mode = LED_MODE_FAULT;
  }
  else if((power_get_state() == POWER_STATE_ACTIVE) &&
          (sm_is_heater_enabled() != 0U))
  {
    mode = LED_MODE_BREATHE;
  }
  else
  {
    mode = LED_MODE_BLINK;
  }

  if((led_mode_valid == 0U) || (mode != led_mode))
  {
    led_mode = mode;
    led_mode_valid = 1U;
    led_breath_index = 0U;
    led_last_step_ms = now_ms;
    led_apply_duty(0U);
  }

  switch(led_mode)
  {
    case LED_MODE_BREATHE:
      if((uint32_t)(now_ms - led_last_step_ms) >= LED_BREATH_STEP_MS)
      {
        led_last_step_ms = now_ms;
        led_breath_index = (uint8_t)((led_breath_index + 1U) & 31U);
        duty = (uint8_t)((led_breath_table[led_breath_index] *
                          LED_BREATH_MAX) / 60U);
        led_apply_duty(duty);
      }
      break;

    case LED_MODE_BLINK:
      phase = now_ms % LED_BLINK_PERIOD_MS;
      led_apply_duty((phase < LED_BLINK_ON_MS) ? 80U : 0U);
      break;

    case LED_MODE_FAULT:
    default:
      phase = now_ms % LED_FAULT_PERIOD_MS;
      led_apply_duty((phase < LED_FAULT_ON_MS) ? 100U : 0U);
      break;
  }
}
