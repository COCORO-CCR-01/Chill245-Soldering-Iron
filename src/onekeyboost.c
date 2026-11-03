/**
  **************************************************************************
  * @file     onekeyboost.c
  * @brief    长按 NEXT 的 OneKeyBoost + 掉温自动 Boost
  **************************************************************************
  * - NEXT 长按：切 BOOST 曲线，设定温度 + boost_offset_c
  * - 非 Boost 时温度 1 s 内突降超过 boost_drop_c：自动进入 Boost
  * - PREV 短按：退出 Boost，恢复原设定
  * - 超时 boost_timeout_s 自动退出
  **************************************************************************
  */

#include "onekeyboost.h"
#include "eeprom.h"
#include "statemachine.h"
#include "powermanagement.h"
#include "menu.h"
#include "hms.h"
#include "tempcal.h"
#include "buttons.h"
#include "buzzer.h"
#include "debug.h"
#include <stddef.h>

#define ONEKEYBOOST_DROP_WINDOW_MS      (1000U)
#define ONEKEYBOOST_PEAK_HOLD_MS        (1200U)
#define ONEKEYBOOST_TEMP_EPS_C          (0.5f)

static uint8_t onekeyboost_active;
static float onekeyboost_orig_setpoint_c;
static float onekeyboost_boost_setpoint_c;
static pid_mode_t onekeyboost_orig_mode;
static uint8_t onekeyboost_orig_heater_enabled;
static uint32_t onekeyboost_start_ms;

static float onekeyboost_peak_c;
static uint32_t onekeyboost_peak_ms;
static float onekeyboost_last_setpoint_c;

static float onekeyboost_absf(float value)
{
  return (value < 0.0f) ? -value : value;
}

static float onekeyboost_clampf(float value, float min_value, float max_value)
{
  if(value < min_value)
  {
    return min_value;
  }

  if(value > max_value)
  {
    return max_value;
  }

  return value;
}

static uint8_t onekeyboost_is_blocked(void)
{
  if((menu_is_open() != 0U) ||
     (hms_is_active() != 0U) ||
     (tempcal_is_active() != 0U))
  {
    return 1U;
  }

  return 0U;
}

static uint32_t onekeyboost_timeout_ms(void)
{
  float timeout_s = g_eeprom_params.boost_timeout_s;

  if(timeout_s < 10.0f)
  {
    timeout_s = 10.0f;
  }

  if(timeout_s > 1800.0f)
  {
    timeout_s = 1800.0f;
  }

  return (uint32_t)((timeout_s * 1000.0f) + 0.5f);
}

static float onekeyboost_drop_threshold_c(void)
{
  float drop_c = g_eeprom_params.boost_drop_c;

  if(drop_c < 5.0f)
  {
    drop_c = 20.0f;
  }

  if(drop_c > 100.0f)
  {
    drop_c = 100.0f;
  }

  return drop_c;
}

static float onekeyboost_offset_c(void)
{
  float offset_c = g_eeprom_params.boost_offset_c;

  if(offset_c < 0.0f)
  {
    offset_c = 0.0f;
  }

  if(offset_c > 200.0f)
  {
    offset_c = 200.0f;
  }

  return offset_c;
}

static void onekeyboost_reset_peak(float tip_c, uint32_t now_ms)
{
  onekeyboost_peak_c = tip_c;
  onekeyboost_peak_ms = now_ms;
}

static uint8_t onekeyboost_start_internal(uint8_t from_drop, uint32_t now_ms)
{
  sm_status_t status;

  if(onekeyboost_is_blocked() != 0U)
  {
    return 0U;
  }

  if(onekeyboost_active != 0U)
  {
    /* 重复长按只重新计时，不重复叠加温度 */
    onekeyboost_start_ms = now_ms;
    return 1U;
  }

  sm_get_status(&status);

  if((status.state == SM_STATE_FAULT) ||
     (status.fault != VERIFY_HEATER_FAULT_NONE) ||
     (status.autotune_state == PID_AUTOTUNE_RUNNING) ||
     (power_get_fault() != POWER_FAULT_NONE))
  {
    return 0U;
  }

  if(from_drop == 0U)
  {
    /* 长按 NEXT 先唤醒来保证能加热 */
    power_wake();
    sm_get_status(&status);
  }

  if((status.state != SM_STATE_HEATING) &&
     (status.state != SM_STATE_AUTOTUNE) &&
     (status.state != SM_STATE_DISABLED))
  {
    return 0U;
  }

  onekeyboost_orig_heater_enabled = sm_is_heater_enabled();
  onekeyboost_orig_setpoint_c = sm_get_setpoint();
  onekeyboost_orig_mode = sm_get_pid_mode();

  if(onekeyboost_orig_heater_enabled == 0U)
  {
    if(sm_set_heater_enabled(1U) == 0U)
    {
      return 0U;
    }
  }

  onekeyboost_boost_setpoint_c =
    onekeyboost_clampf(onekeyboost_orig_setpoint_c +
                       onekeyboost_offset_c(), 50.0f, 450.0f);

  (void)sm_set_pid_mode(PID_MODE_BOOST);
  (void)sm_set_setpoint(onekeyboost_boost_setpoint_c);

  onekeyboost_active = 1U;
  onekeyboost_start_ms = now_ms;
  onekeyboost_last_setpoint_c = onekeyboost_boost_setpoint_c;
  onekeyboost_reset_peak(status.tip_c, now_ms);

  buzzer_play(BUZZER_SOUND_MODE);
  return 1U;
}

uint8_t onekeyboost_start_manual(void)
{
  if(onekeyboost_is_blocked() != 0U)
  {
    return 0U;
  }

  return onekeyboost_start_internal(0U, sm_get_uptime_ms());
}

void onekeyboost_cancel(void)
{
  if(onekeyboost_active == 0U)
  {
    return;
  }

  onekeyboost_active = 0U;

  (void)sm_set_setpoint(onekeyboost_orig_setpoint_c);
  (void)sm_set_pid_mode(onekeyboost_orig_mode);

  if(onekeyboost_orig_heater_enabled == 0U)
  {
    (void)sm_set_heater_enabled(0U);
  }

  buzzer_play(BUZZER_SOUND_MODE);
}

uint8_t onekeyboost_is_active(void)
{
  return onekeyboost_active;
}

float onekeyboost_get_setpoint_step(void)
{
  float step_c = g_eeprom_params.setpoint_step_c;

  if(step_c < 1.0f)
  {
    step_c = 5.0f;
  }

  if(step_c > 100.0f)
  {
    step_c = 100.0f;
  }

  return step_c;
}

static void onekeyboost_on_button(button_id_t button, button_event_t event)
{
  if(onekeyboost_is_blocked() != 0U)
  {
    return;
  }

  if((button == BUTTON_NEXT) && (event == BUTTON_EVENT_LONG_PRESS))
  {
    (void)onekeyboost_start_manual();
  }
  else if((button == BUTTON_PREV) && (event == BUTTON_EVENT_CLICK))
  {
    if(onekeyboost_active != 0U)
    {
      onekeyboost_cancel();
    }
  }
}

static void onekeyboost_debug_command(int argc, char **argv)
{
  if(argc < 2)
  {
    debug_write("usage: boost start|stop|status\r\n");
    return;
  }

  if((argv[1][0] == 's') && (argv[1][1] == 't') &&
     (argv[1][2] == 'a') && (argv[1][3] == 'r') &&
     (argv[1][4] == 't') && (argv[1][5] == '\0'))
  {
    if(onekeyboost_start_manual() == 0U)
    {
      debug_write("boost cannot start\r\n");
    }
  }
  else if((argv[1][0] == 's') && (argv[1][1] == 't') &&
          (argv[1][2] == 'o') && (argv[1][3] == 'p') &&
          (argv[1][4] == '\0'))
  {
    onekeyboost_cancel();
  }
  else if((argv[1][0] == 's') && (argv[1][1] == 't') &&
          (argv[1][2] == 'a') && (argv[1][3] == 't') &&
          (argv[1][4] == 'u') && (argv[1][5] == 's') &&
          (argv[1][6] == '\0'))
  {
    debug_printf("boost=%u setpoint=",
                 (unsigned)onekeyboost_active);
    debug_write_float(sm_get_setpoint(), 1U);
    debug_write("C\r\n");
  }
  else
  {
    debug_write("usage: boost start|stop|status\r\n");
  }
}

void onekeyboost_init(void)
{
  onekeyboost_active = 0U;
  onekeyboost_peak_c = 0.0f;
  onekeyboost_peak_ms = 0U;
  onekeyboost_last_setpoint_c = 0.0f;

  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_LONG_PRESS,
                                  onekeyboost_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_CLICK,
                                  onekeyboost_on_button);

  (void)debug_register_command("boost",
                               "one-key boost: boost start|stop|status",
                               onekeyboost_debug_command);
}

void onekeyboost_task(uint32_t now_ms)
{
  sm_status_t status;
  float drop_c;
  float tip_c;

  sm_get_status(&status);
  tip_c = status.tip_c;

  if(onekeyboost_active != 0U)
  {
    if((onekeyboost_is_blocked() != 0U) ||
       (sm_get_state() != SM_STATE_HEATING) ||
       (status.fault != VERIFY_HEATER_FAULT_NONE) ||
       (status.autotune_state == PID_AUTOTUNE_RUNNING) ||
       (power_get_fault() != POWER_FAULT_NONE) ||
       (power_get_state() != POWER_STATE_ACTIVE) ||
       (sm_is_heater_enabled() == 0U) ||
       (sm_get_pid_mode() != PID_MODE_BOOST) ||
       (onekeyboost_absf(sm_get_setpoint() -
                         onekeyboost_boost_setpoint_c) >
        ONEKEYBOOST_TEMP_EPS_C))
    {
      onekeyboost_cancel();
      return;
    }

    if((uint32_t)(now_ms - onekeyboost_start_ms) >=
       onekeyboost_timeout_ms())
    {
      onekeyboost_cancel();
      return;
    }

    onekeyboost_reset_peak(tip_c, now_ms);
    return;
  }

  if((sm_get_state() != SM_STATE_HEATING) ||
     (status.temp_valid == 0U) ||
     (status.fault != VERIFY_HEATER_FAULT_NONE) ||
     (status.autotune_state == PID_AUTOTUNE_RUNNING) ||
     (power_get_fault() != POWER_FAULT_NONE) ||
     (power_get_state() != POWER_STATE_ACTIVE) ||
     (sm_is_heater_enabled() == 0U) ||
     (onekeyboost_is_blocked() != 0U))
  {
    onekeyboost_reset_peak(tip_c, now_ms);
    onekeyboost_last_setpoint_c = sm_get_setpoint();
    return;
  }

  if(onekeyboost_absf(sm_get_setpoint() -
                      onekeyboost_last_setpoint_c) >
     ONEKEYBOOST_TEMP_EPS_C)
  {
    onekeyboost_last_setpoint_c = sm_get_setpoint();
    onekeyboost_reset_peak(tip_c, now_ms);
    return;
  }

  if((tip_c >= (onekeyboost_peak_c - 0.2f)) ||
     ((uint32_t)(now_ms - onekeyboost_peak_ms) >
      ONEKEYBOOST_PEAK_HOLD_MS))
  {
    /* 温度持平/上升时保持峰值时间新鲜；缓慢下降超过保持时间则重置 */
    onekeyboost_reset_peak(tip_c, now_ms);
  }

  drop_c = onekeyboost_drop_threshold_c();

  if((onekeyboost_peak_c - tip_c) >= drop_c)
  {
    if((uint32_t)(now_ms - onekeyboost_peak_ms) <=
       ONEKEYBOOST_DROP_WINDOW_MS)
    {
      (void)onekeyboost_start_internal(1U, now_ms);
    }
    else
    {
      onekeyboost_reset_peak(tip_c, now_ms);
    }
  }
}
