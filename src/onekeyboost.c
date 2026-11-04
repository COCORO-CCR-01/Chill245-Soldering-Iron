/**
  **************************************************************************
  * @file     onekeyboost.c
  * @brief    长按 NEXT 的 OneKeyBoost + 掉温自动 Boost
  **************************************************************************
  * - NEXT 长按：切 BOOST 曲线，设定温度 + boost_offset_c
  * - 非 Boost 时温度 1 s 内突降超过 boost_drop_c：自动进入 Boost
  * - NEXT 短按：退出 Boost，恢复原设定
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
#include <stddef.h>

#define ONEKEYBOOST_DROP_WINDOW_MS      (1000U)
#define ONEKEYBOOST_PEAK_HOLD_MS        (1200U)
#define ONEKEYBOOST_TEMP_EPS_C          (0.5f)
#define ONEKEYBOOST_REARM_DELAY_MS      (5000U)
#define ONEKEYBOOST_DROP_CONFIRM_MS     (150U)

static uint8_t onekeyboost_active;
static float onekeyboost_orig_setpoint_c;
static float onekeyboost_boost_setpoint_c;
static pid_mode_t onekeyboost_orig_mode;
static uint8_t onekeyboost_orig_heater_enabled;
static uint32_t onekeyboost_start_ms;

static float onekeyboost_peak_c;
static uint32_t onekeyboost_peak_ms;
static float onekeyboost_last_setpoint_c;
static uint32_t onekeyboost_cooldown_until_ms;
static uint32_t onekeyboost_drop_pending_ms;

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

  /* 刚退出 Boost 后给温度一个回落/稳定的冷却期，禁止掉温自动重入 */
  if((from_drop != 0U) &&
     ((int32_t)(onekeyboost_cooldown_until_ms - now_ms) > 0))
  {
    return 0U;
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
  onekeyboost_cooldown_until_ms = 0U;
  onekeyboost_drop_pending_ms = 0U;
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
  uint32_t now_ms;
  sm_status_t status;

  if(onekeyboost_active == 0U)
  {
    return;
  }

  onekeyboost_active = 0U;
  now_ms = sm_get_uptime_ms();

  (void)sm_set_setpoint(onekeyboost_orig_setpoint_c);
  (void)sm_set_pid_mode(onekeyboost_orig_mode);

  if(onekeyboost_orig_heater_enabled == 0U)
  {
    (void)sm_set_heater_enabled(0U);
  }

  sm_get_status(&status);
  onekeyboost_reset_peak(status.tip_c, now_ms);
  onekeyboost_last_setpoint_c = onekeyboost_orig_setpoint_c;
  onekeyboost_drop_pending_ms = 0U;
  onekeyboost_cooldown_until_ms = now_ms + ONEKEYBOOST_REARM_DELAY_MS;

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
  else if((button == BUTTON_NEXT) && (event == BUTTON_EVENT_CLICK))
  {
    if(onekeyboost_active != 0U)
    {
      onekeyboost_cancel();
    }
  }
}

void onekeyboost_init(void)
{
  onekeyboost_active = 0U;
  onekeyboost_peak_c = 0.0f;
  onekeyboost_peak_ms = 0U;
  onekeyboost_last_setpoint_c = 0.0f;
  onekeyboost_cooldown_until_ms = 0U;
  onekeyboost_drop_pending_ms = 0U;

  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_LONG_PRESS,
                                  onekeyboost_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_CLICK,
                                  onekeyboost_on_button);
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

  /* 退出 Boost 后至少冷却 5 s，期间不响应掉温自动 Boost */
  if((int32_t)(onekeyboost_cooldown_until_ms - now_ms) > 0)
  {
    onekeyboost_reset_peak(tip_c, now_ms);
    onekeyboost_drop_pending_ms = 0U;
    return;
  }

  /*
   * 当前温度已经达到/高于设定温度时，即使从峰值回落也不自动 Boost。
   * 自动 Boost 只用于“低于目标且被焊点拉低”的场景。
   */
  if(tip_c >= sm_get_setpoint())
  {
    onekeyboost_reset_peak(tip_c, now_ms);
    onekeyboost_drop_pending_ms = 0U;
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
      /* 掉温需持续 150 ms，过滤单次采样尖峰 */
      if(onekeyboost_drop_pending_ms == 0U)
      {
        onekeyboost_drop_pending_ms = now_ms;
      }
      else if((uint32_t)(now_ms - onekeyboost_drop_pending_ms) >=
              ONEKEYBOOST_DROP_CONFIRM_MS)
      {
        onekeyboost_drop_pending_ms = 0U;

        if(onekeyboost_start_internal(1U, now_ms) == 0U)
        {
          onekeyboost_reset_peak(tip_c, now_ms);
        }
      }
    }
    else
    {
      onekeyboost_drop_pending_ms = 0U;
      onekeyboost_reset_peak(tip_c, now_ms);
    }
  }
  else
  {
    onekeyboost_drop_pending_ms = 0U;
  }
}
