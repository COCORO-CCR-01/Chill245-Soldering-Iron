/**
  **************************************************************************
  * @file     buzzer.c
  * @brief   非阻塞蜂鸣器音效
  **************************************************************************
  * 硬件：
  *   PB5 -> TMR3_CH2（MUX_1）-> Q7 MMBT3904 NPN -> 4 kHz 蜂鸣器
  *   PB5 高电平 -> Q7 导通 -> 蜂鸣器响
  *
  * 所有音效由 buzzer_task() 以 1 ms 节拍推进，不使用阻塞延时。
  **************************************************************************
  */

#include "buzzer.h"
#include "at32f421_conf.h"
#include "statemachine.h"
#include "powermanagement.h"
#include "menu.h"
#include "buttons.h"
#include "led.h"
#include <stddef.h>

#define BUZZER_TMR                      TMR3
#define BUZZER_TMR_CHANNEL              TMR_SELECT_CHANNEL_2
#define BUZZER_PIN                      GPIO_PINS_5
#define BUZZER_PIN_SOURCE               GPIO_PINS_SOURCE5
#define BUZZER_PIN_MUX                  GPIO_MUX_1
#define BUZZER_TMR_CLOCK                CRM_TMR3_PERIPH_CLOCK
#define BUZZER_PRESCALER                (119U)       /* 120 MHz / 120 = 1 MHz */
#define BUZZER_POLL_MS                  (20U)
#define BUZZER_FAULT_REPEAT_MS          (5000U)

typedef struct
{
  uint16_t freq_hz;
  uint16_t on_ms;
  uint16_t off_ms;
} buzzer_note_t;

typedef struct
{
  const buzzer_note_t *notes;
  uint8_t count;
  uint8_t priority;
} buzzer_sound_def_t;

static const buzzer_note_t buzzer_boot_notes[] =
{
  { 2000U, 70U, 35U },
  { 2600U, 70U, 35U },
  { 3200U, 70U, 35U },
  { 4000U, 180U, 0U }
};

static const buzzer_note_t buzzer_key_notes[] =
{
  { 4000U, 18U, 0U }
};

static const buzzer_note_t buzzer_setpoint_notes[] =
{
  { 2600U, 55U, 35U },
  { 3200U, 55U, 35U },
  { 4000U, 110U, 0U }
};

static const buzzer_note_t buzzer_sleep_notes[] =
{
  { 3000U, 120U, 60U },
  { 2200U, 240U, 0U }
};

static const buzzer_note_t buzzer_deep_sleep_notes[] =
{
  { 1800U, 170U, 100U },
  { 1800U, 170U, 100U },
  { 1200U, 320U, 0U }
};

static const buzzer_note_t buzzer_shutdown_notes[] =
{
  { 2500U, 220U, 70U },
  { 1900U, 220U, 70U },
  { 1200U, 500U, 0U }
};

static const buzzer_note_t buzzer_wake_notes[] =
{
  { 1800U, 70U, 30U },
  { 2400U, 70U, 30U },
  { 3200U, 120U, 0U }
};

static const buzzer_note_t buzzer_fault_notes[] =
{
  { 3800U, 90U, 50U },
  { 1900U, 90U, 50U },
  { 3800U, 90U, 50U },
  { 1900U, 90U, 50U },
  { 3800U, 120U, 70U },
  { 1500U, 250U, 0U }
};

static const buzzer_note_t buzzer_mode_notes[] =
{
  { 2800U, 55U, 30U },
  { 3600U, 90U, 0U }
};

static const buzzer_note_t buzzer_menu_open_notes[] =
{
  { 2200U, 45U, 25U },
  { 3000U, 45U, 25U },
  { 3800U, 80U, 0U }
};

static const buzzer_note_t buzzer_menu_close_notes[] =
{
  { 3200U, 55U, 30U },
  { 2200U, 90U, 0U }
};

static const buzzer_note_t buzzer_at_start_notes[] =
{
  { 2000U, 90U, 40U },
  { 2600U, 90U, 40U },
  { 3200U, 140U, 0U }
};

static const buzzer_note_t buzzer_at_done_notes[] =
{
  { 3000U, 90U, 35U },
  { 4000U, 180U, 0U }
};

static const buzzer_note_t buzzer_at_fail_notes[] =
{
  { 1500U, 150U, 70U },
  { 900U, 300U, 0U }
};

static const buzzer_note_t buzzer_error_notes[] =
{
  { 1200U, 180U, 60U },
  { 800U, 350U, 0U }
};

static const buzzer_sound_def_t buzzer_sounds[BUZZER_SOUND_COUNT] =
{
  [BUZZER_SOUND_BOOT] =
    { buzzer_boot_notes, (uint8_t)(sizeof(buzzer_boot_notes) / sizeof(buzzer_boot_notes[0])), 25U },
  [BUZZER_SOUND_KEY] =
    { buzzer_key_notes, (uint8_t)(sizeof(buzzer_key_notes) / sizeof(buzzer_key_notes[0])), 10U },
  [BUZZER_SOUND_SETPOINT] =
    { buzzer_setpoint_notes, (uint8_t)(sizeof(buzzer_setpoint_notes) / sizeof(buzzer_setpoint_notes[0])), 55U },
  [BUZZER_SOUND_SLEEP] =
    { buzzer_sleep_notes, (uint8_t)(sizeof(buzzer_sleep_notes) / sizeof(buzzer_sleep_notes[0])), 60U },
  [BUZZER_SOUND_DEEP_SLEEP] =
    { buzzer_deep_sleep_notes, (uint8_t)(sizeof(buzzer_deep_sleep_notes) / sizeof(buzzer_deep_sleep_notes[0])), 70U },
  [BUZZER_SOUND_SHUTDOWN] =
    { buzzer_shutdown_notes, (uint8_t)(sizeof(buzzer_shutdown_notes) / sizeof(buzzer_shutdown_notes[0])), 80U },
  [BUZZER_SOUND_WAKE] =
    { buzzer_wake_notes, (uint8_t)(sizeof(buzzer_wake_notes) / sizeof(buzzer_wake_notes[0])), 50U },
  [BUZZER_SOUND_FAULT] =
    { buzzer_fault_notes, (uint8_t)(sizeof(buzzer_fault_notes) / sizeof(buzzer_fault_notes[0])), 100U },
  [BUZZER_SOUND_MODE] =
    { buzzer_mode_notes, (uint8_t)(sizeof(buzzer_mode_notes) / sizeof(buzzer_mode_notes[0])), 35U },
  [BUZZER_SOUND_MENU_OPEN] =
    { buzzer_menu_open_notes, (uint8_t)(sizeof(buzzer_menu_open_notes) / sizeof(buzzer_menu_open_notes[0])), 30U },
  [BUZZER_SOUND_MENU_CLOSE] =
    { buzzer_menu_close_notes, (uint8_t)(sizeof(buzzer_menu_close_notes) / sizeof(buzzer_menu_close_notes[0])), 30U },
  [BUZZER_SOUND_AUTOTUNE_START] =
    { buzzer_at_start_notes, (uint8_t)(sizeof(buzzer_at_start_notes) / sizeof(buzzer_at_start_notes[0])), 45U },
  [BUZZER_SOUND_AUTOTUNE_DONE] =
    { buzzer_at_done_notes, (uint8_t)(sizeof(buzzer_at_done_notes) / sizeof(buzzer_at_done_notes[0])), 45U },
  [BUZZER_SOUND_AUTOTUNE_FAIL] =
    { buzzer_at_fail_notes, (uint8_t)(sizeof(buzzer_at_fail_notes) / sizeof(buzzer_at_fail_notes[0])), 65U },
  [BUZZER_SOUND_ERROR] =
    { buzzer_error_notes, (uint8_t)(sizeof(buzzer_error_notes) / sizeof(buzzer_error_notes[0])), 70U }
};

static uint8_t buzzer_initialized;
static uint8_t buzzer_enabled;
static uint8_t buzzer_volume = 100U;
static uint8_t buzzer_busy;

static const buzzer_sound_def_t *buzzer_current;
static uint8_t buzzer_note_index;
static uint8_t buzzer_phase;              /* 0 = tone, 1 = gap */
static uint32_t buzzer_phase_start_ms;
static uint16_t buzzer_tone_freq;

static uint32_t buzzer_last_poll_ms;
static uint32_t buzzer_last_fault_alert_ms;
static uint16_t buzzer_last_fault_code;
static power_state_t buzzer_last_power_state;
static pid_mode_t buzzer_last_mode;
static pid_autotune_state_t buzzer_last_tune_state;
static uint8_t buzzer_last_menu_open;
static float buzzer_last_setpoint_c;
static uint8_t buzzer_setpoint_armed;
static uint8_t buzzer_setpoint_reached;

static uint32_t buzzer_duty_from_arr(uint32_t arr)
{
  uint32_t compare;

  compare = ((arr + 1U) * (uint32_t)buzzer_volume) / 200U;

  if(compare > (arr + 1U))
  {
    compare = arr + 1U;
  }

  return compare;
}

static void buzzer_tone_off(void)
{
  if(buzzer_initialized == 0U)
  {
    return;
  }

  tmr_channel_value_set(BUZZER_TMR, BUZZER_TMR_CHANNEL, 0U);
  buzzer_tone_freq = 0U;
}

static void buzzer_tone_on(uint16_t freq_hz)
{
  uint32_t arr;

  if((buzzer_initialized == 0U) || (freq_hz == 0U))
  {
    return;
  }

  if((freq_hz >= 500U) && (freq_hz <= 5000U) &&
     (freq_hz == buzzer_tone_freq))
  {
    return;
  }

  if(freq_hz < 500U)
  {
    freq_hz = 500U;
  }

  if(freq_hz > 5000U)
  {
    freq_hz = 5000U;
  }

  arr = 1000000UL / (uint32_t)freq_hz;

  if(arr < 2U)
  {
    arr = 2U;
  }

  arr--;

  if(arr > 65535U)
  {
    arr = 65535U;
  }

  tmr_base_init(BUZZER_TMR, arr, BUZZER_PRESCALER);
  tmr_channel_value_set(BUZZER_TMR, BUZZER_TMR_CHANNEL,
                        (uint16_t)buzzer_duty_from_arr(arr));
  led_refresh_pwm();
  buzzer_tone_freq = freq_hz;
}

static void buzzer_start_note(uint32_t now_ms)
{
  if((buzzer_current == NULL) ||
     (buzzer_note_index >= buzzer_current->count))
  {
    buzzer_busy = 0U;
    buzzer_current = NULL;
    buzzer_tone_off();
    return;
  }

  buzzer_tone_on(buzzer_current->notes[buzzer_note_index].freq_hz);
  buzzer_phase = 0U;
  buzzer_phase_start_ms = now_ms;
}

void buzzer_play(buzzer_sound_t sound)
{
  const buzzer_sound_def_t *def;

  if((buzzer_initialized == 0U) || (buzzer_enabled == 0U) ||
     (sound >= BUZZER_SOUND_COUNT))
  {
    return;
  }

  def = &buzzer_sounds[sound];

  if((def->notes == NULL) || (def->count == 0U))
  {
    return;
  }

  if((buzzer_busy != 0U) && (buzzer_current != NULL) &&
     (def->priority <= buzzer_current->priority))
  {
    return;
  }

  buzzer_tone_off();

  buzzer_current = def;
  buzzer_note_index = 0U;
  buzzer_busy = 1U;
  buzzer_start_note(sm_get_uptime_ms());
}

void buzzer_stop(void)
{
  buzzer_busy = 0U;
  buzzer_current = NULL;
  buzzer_note_index = 0U;
  buzzer_tone_off();
}

void buzzer_set_enabled(uint8_t enable)
{
  uint8_t was_enabled = buzzer_enabled;

  buzzer_enabled = (enable != 0U) ? 1U : 0U;

  if(buzzer_enabled == 0U)
  {
    buzzer_stop();
  }
  else if((was_enabled == 0U) && (buzzer_initialized != 0U))
  {
    /* 重新启用时同步状态，避免把旧的休眠/模式状态当成新事件 */
    buzzer_sync_state();
  }
}

uint8_t buzzer_is_enabled(void)
{
  return buzzer_enabled;
}

void buzzer_set_volume(uint8_t volume)
{
  if(volume > 100U)
  {
    volume = 100U;
  }

  buzzer_volume = volume;

  if((buzzer_initialized != 0U) && (buzzer_tone_freq != 0U))
  {
    uint32_t arr = (uint32_t)BUZZER_TMR->pr;
    tmr_channel_value_set(BUZZER_TMR, BUZZER_TMR_CHANNEL,
                          (uint16_t)buzzer_duty_from_arr(arr));
  }
}

uint8_t buzzer_get_volume(void)
{
  return buzzer_volume;
}

uint8_t buzzer_is_busy(void)
{
  return buzzer_busy;
}

void buzzer_sync_state(void)
{
  sm_status_t status;

  sm_get_status(&status);

  buzzer_last_poll_ms = sm_get_uptime_ms();
  buzzer_last_fault_alert_ms = 0U;
  buzzer_last_fault_code = 0U;
  buzzer_last_power_state = power_get_state();
  buzzer_last_menu_open = menu_is_open();
  buzzer_last_mode = status.mode;
  buzzer_last_tune_state = status.autotune_state;
  buzzer_last_setpoint_c = status.setpoint_c;
  buzzer_setpoint_reached = 0U;
  buzzer_setpoint_armed =
    ((status.state == SM_STATE_HEATING) &&
     (status.temp_valid != 0U) &&
     (status.setpoint_c >= 50.0f) &&
     (status.tip_c < (status.setpoint_c - 10.0f))) ? 1U : 0U;
}

static void buzzer_on_button(button_id_t button, button_event_t event)
{
  (void)button;

  if(event == BUTTON_EVENT_PRESS)
  {
    buzzer_play(BUZZER_SOUND_KEY);
  }
}

static void buzzer_poll_power(void)
{
  power_state_t state = power_get_state();

  if(state == buzzer_last_power_state)
  {
    return;
  }

  switch(state)
  {
    case POWER_STATE_REDUCED:
      buzzer_play(BUZZER_SOUND_SLEEP);
      break;

    case POWER_STATE_HEATER_OFF:
      buzzer_play(BUZZER_SOUND_DEEP_SLEEP);
      break;

    case POWER_STATE_BACKLIGHT_OFF:
      buzzer_play(BUZZER_SOUND_SHUTDOWN);
      break;

    case POWER_STATE_ACTIVE:
    default:
      if(buzzer_last_power_state != POWER_STATE_ACTIVE)
      {
        buzzer_play(BUZZER_SOUND_WAKE);
      }
      break;
  }

  buzzer_last_power_state = state;
}

static void buzzer_poll_fault(const sm_status_t *status, uint32_t now_ms)
{
  uint16_t fault_code;

  fault_code = (uint16_t)status->fault |
               ((uint16_t)power_get_fault() << 8);

  if(fault_code != buzzer_last_fault_code)
  {
    buzzer_last_fault_code = fault_code;

    if(fault_code != 0U)
    {
      buzzer_last_fault_alert_ms = now_ms;
      buzzer_play(BUZZER_SOUND_FAULT);
    }
  }
  else if((fault_code != 0U) && (buzzer_busy == 0U) &&
          ((uint32_t)(now_ms - buzzer_last_fault_alert_ms) >=
           BUZZER_FAULT_REPEAT_MS))
  {
    buzzer_last_fault_alert_ms = now_ms;
    buzzer_play(BUZZER_SOUND_FAULT);
  }
}

static void buzzer_poll_setpoint(const sm_status_t *status)
{
  float setpoint = status->setpoint_c;
  float tip = status->tip_c;

  if((status->state != SM_STATE_HEATING) ||
     (status->temp_valid == 0U) || (setpoint < 50.0f) ||
     (power_get_state() != POWER_STATE_ACTIVE))
  {
    buzzer_setpoint_armed = 0U;
    buzzer_setpoint_reached = 0U;
    buzzer_last_setpoint_c = setpoint;
    return;
  }

  if((setpoint < (buzzer_last_setpoint_c - 1.0f)) ||
     (setpoint > (buzzer_last_setpoint_c + 1.0f)))
  {
    buzzer_last_setpoint_c = setpoint;
    buzzer_setpoint_reached = 0U;
    buzzer_setpoint_armed = (tip < (setpoint - 10.0f)) ? 1U : 0U;
  }

  if(tip < (setpoint - 10.0f))
  {
    buzzer_setpoint_armed = 1U;
    buzzer_setpoint_reached = 0U;
  }

  if((buzzer_setpoint_armed != 0U) &&
     (buzzer_setpoint_reached == 0U) &&
     (tip >= (setpoint - 3.0f)))
  {
    buzzer_setpoint_armed = 0U;
    buzzer_setpoint_reached = 1U;
    buzzer_play(BUZZER_SOUND_SETPOINT);
  }
}

static void buzzer_poll_mode(const sm_status_t *status)
{
  if(status->mode != buzzer_last_mode)
  {
    buzzer_last_mode = status->mode;
    buzzer_play(BUZZER_SOUND_MODE);
  }
}

static void buzzer_poll_autotune(const sm_status_t *status)
{
  pid_autotune_state_t state = status->autotune_state;

  if(state == buzzer_last_tune_state)
  {
    return;
  }

  if(state == PID_AUTOTUNE_RUNNING)
  {
    buzzer_play(BUZZER_SOUND_AUTOTUNE_START);
  }
  else if(state == PID_AUTOTUNE_DONE)
  {
    buzzer_play(BUZZER_SOUND_AUTOTUNE_DONE);
  }
  else if(state == PID_AUTOTUNE_FAILED)
  {
    buzzer_play(BUZZER_SOUND_AUTOTUNE_FAIL);
  }
  else if((state == PID_AUTOTUNE_IDLE) &&
          (buzzer_last_tune_state == PID_AUTOTUNE_RUNNING))
  {
    buzzer_play(BUZZER_SOUND_AUTOTUNE_FAIL);
  }

  buzzer_last_tune_state = state;
}

static void buzzer_poll_menu(void)
{
  uint8_t menu_open = menu_is_open();

  if(menu_open != buzzer_last_menu_open)
  {
    buzzer_last_menu_open = menu_open;
    buzzer_play((menu_open != 0U) ?
                BUZZER_SOUND_MENU_OPEN : BUZZER_SOUND_MENU_CLOSE);
  }
}

static void buzzer_poll_events(uint32_t now_ms)
{
  sm_status_t status;

  sm_get_status(&status);

  buzzer_poll_power();
  buzzer_poll_fault(&status, now_ms);
  buzzer_poll_setpoint(&status);
  buzzer_poll_mode(&status);
  buzzer_poll_autotune(&status);
  buzzer_poll_menu();
}

void buzzer_init(void)
{
  gpio_init_type gpio_init_struct;
  tmr_output_config_type output_config;

  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(BUZZER_TMR_CLOCK, TRUE);

  /* 先把 PB5 拉低，确保 NPN 关断，再切到 TMR3 复用 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = BUZZER_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOB, &gpio_init_struct);
  gpio_bits_reset(GPIOB, BUZZER_PIN);

  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init(GPIOB, &gpio_init_struct);
  gpio_pin_mux_config(GPIOB, BUZZER_PIN_SOURCE, BUZZER_PIN_MUX);

  /* TMR3: 1 MHz counter, 4 kHz initial period */
  tmr_base_init(BUZZER_TMR, 249U, BUZZER_PRESCALER);
  tmr_cnt_dir_set(BUZZER_TMR, TMR_COUNT_UP);
  tmr_clock_source_div_set(BUZZER_TMR, TMR_CLOCK_DIV1);

  tmr_output_default_para_init(&output_config);
  output_config.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;
  output_config.oc_idle_state = FALSE;
  output_config.oc_polarity = TMR_OUTPUT_ACTIVE_HIGH;
  output_config.oc_output_state = TRUE;
  tmr_output_channel_config(BUZZER_TMR, BUZZER_TMR_CHANNEL, &output_config);
  tmr_channel_value_set(BUZZER_TMR, BUZZER_TMR_CHANNEL, 0U);
  tmr_counter_enable(BUZZER_TMR, TRUE);

  buzzer_initialized = 1U;
  buzzer_enabled = 1U;
  buzzer_volume = 100U;
  buzzer_busy = 0U;
  buzzer_current = NULL;
  buzzer_note_index = 0U;
  buzzer_tone_freq = 0U;

  buzzer_sync_state();

  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_PRESS,
                                  buzzer_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_PRESS,
                                  buzzer_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_PRESS,
                                  buzzer_on_button);
}

void buzzer_task(uint32_t now_ms)
{
  const buzzer_note_t *note;

  if(buzzer_initialized == 0U)
  {
    return;
  }

  if(buzzer_enabled == 0U)
  {
    return;
  }

  if((buzzer_busy != 0U) && (buzzer_current != NULL))
  {
    note = &buzzer_current->notes[buzzer_note_index];

    if(buzzer_phase == 0U)
    {
      if((uint32_t)(now_ms - buzzer_phase_start_ms) >= note->on_ms)
      {
        buzzer_tone_off();
        buzzer_phase = 1U;
        buzzer_phase_start_ms = now_ms;
      }
    }
    else
    {
      if((uint32_t)(now_ms - buzzer_phase_start_ms) >= note->off_ms)
      {
        buzzer_note_index++;

        if(buzzer_note_index >= buzzer_current->count)
        {
          buzzer_busy = 0U;
          buzzer_current = NULL;
          buzzer_tone_off();
        }
        else
        {
          buzzer_start_note(now_ms);
        }
      }
    }
  }

  if((uint32_t)(now_ms - buzzer_last_poll_ms) >= BUZZER_POLL_MS)
  {
    buzzer_last_poll_ms = now_ms;
    buzzer_poll_events(now_ms);
  }
}
