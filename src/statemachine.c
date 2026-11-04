#pragma GCC optimize("O2")
/**
  **************************************************************************
  * @file     statemachine.c
  * @brief    20 kHz 控制状态机与模块统一调度
  **************************************************************************
  * 控制环（TMR1 更新中断，20 kHz / 50 µs）：
  *   ADC -> 温度 -> PID -> 保护(1 kHz) -> 20 kHz PWM -> 快照
  *
  * 后台环（主循环，1 kHz）：
  *   读取控制快照 -> UI / 按键 / 保护任务
  *
  * 分工：
  *   - 20 kHz：PWM、状态切换、故障切断
  *   - 200 Hz：ADC、热电偶温度、PID
  *   - 1 kHz：verifyheater 时间型保护、自动整定步骤、快照
  *   - 1 kHz：主循环调度
  *
  * 并发：
  *   - TMR1 更新中断优先级 0，负责全部控制状态；
  *   - 主循环只读快照，命令通过带临界区的 API 修改控制状态。
  **************************************************************************
  */

#include "statemachine.h"
#include "adc.h"
#include "temp.h"
#include "verifyheater.h"
#include "pid.h"
#include "heater.h"
#include "hms.h"
#include "at32f421_conf.h"
#include "wk_system.h"
#include <stdlib.h>
#include <stddef.h>

#define SM_DEFAULT_SETPOINT_C           (300.0f)
#define SM_AUTOTUNE_ON_BOOT             (0)

#define SM_TICKS_PER_MS                 (SM_FAST_RATE_HZ / 1000U)   /* 20 */

/*
 * C245 热电偶与加热驱动存在耦合：
 *   每个测量周期先关加热 0.5 ms，再采样，然后恢复 PWM。
 *   测量周期 5 ms（200 Hz），其中 0.5 ms 消隐，4.5 ms 加热。
 */
#define SM_HEATER_BLANK_MS              (0.5f)
#define SM_HEATER_BLANK_TICKS           ((uint8_t)(SM_HEATER_BLANK_MS * (float)SM_TICKS_PER_MS))
#define SM_TEMP_SAMPLE_MS               (5U)
#define SM_TEMP_SAMPLE_TICKS            (SM_TEMP_SAMPLE_MS * SM_TICKS_PER_MS)
#define SM_SAMPLE_DT_S                  ((float)SM_TEMP_SAMPLE_MS / 1000.0f)

/* 与 pid.c 的 PID_AT_PWM_HIGH 保持一致，用于首个整定周期的占空比 */
#define SM_AUTOTUNE_SEED_POWER          (90.0f)

/* 加热独立看门狗：主循环约 1 kHz，控制序列 80 次不变即判定停滞 */
#define SM_HEATER_WD_STALL_LOOPS        (80U)

/*
 * 硬件 WDT 先关闭，保留软件加热看门狗。
 * 上电无反应时优先排除 WDT 复位循环；确认主循环稳定后改成 1 再启用。
 * 启用时使用约 4 s 超时，给显示初始化/CH224A 探测留足时间。
 */
#define SM_ENABLE_HARDWARE_WDT          (1U)
#define SM_WDT_RELOAD_VALUE             (2499U)  /* LICK 40k/64, 约 4 s */

/* 把 1 kHz 慢任务错开，避免 NTC、保护、整定、快照落在同一个 50 µs 中断里 */
#define SM_PROT_PHASE                   (0U)
#define SM_SNAPSHOT_PHASE               (10U)

typedef struct
{
  adc_raw_data_t adc;
  temp_data_t temp;
  sm_state_t state;
  uint8_t power;
  uint8_t heater_on;
  uint16_t fault;
  uint32_t now_ms;
  uint32_t fast_count;
  uint16_t isr_last_us;
  uint16_t isr_max_us;
  pid_status_t pid;
} sm_snapshot_t;

static sm_state_t sm_state;
static adc_raw_data_t sm_adc;
static temp_data_t sm_temp;
static uint8_t sm_power;
static volatile uint32_t sm_fast_sequence;
static uint32_t sm_wd_last_sequence;
static uint32_t sm_wd_stall_loops;
static uint8_t sm_wd_tripped;
static uint32_t sm_now_ms;
static uint32_t sm_fast_count;
static uint8_t sm_tick_in_ms;
static uint8_t sm_sample_phase;
static float sm_autotune_output;

static volatile uint16_t sm_isr_last_us;
static volatile uint16_t sm_isr_max_us;


static volatile sm_snapshot_t sm_snapshot;


static void sm_fast_update(void);
static void sm_timer_init(void);
static void sm_capture_snapshot(void);
static uint32_t sm_critical_enter(void);
static void sm_critical_exit(uint32_t primask);
static uint32_t sm_critical_enter(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void sm_critical_exit(uint32_t primask)
{
  if(primask == 0U)
  {
    __enable_irq();
  }
}

static uint8_t sm_round_power(float power)
{
  if(power <= 0.0f)
  {
    return 0U;
  }

  if(power >= 100.0f)
  {
    return 100U;
  }

  return (uint8_t)(power + 0.5f);
}

static void sm_capture_snapshot(void)
{
  pid_status_t pid_status;

  sm_snapshot.adc = sm_adc;
  sm_snapshot.temp = sm_temp;
  sm_snapshot.state = sm_state;
  sm_snapshot.power = sm_power;
  sm_snapshot.heater_on = heater_is_on();
  sm_snapshot.fault = (uint16_t)verifyheater_get_fault();
  sm_snapshot.now_ms = sm_now_ms;
  sm_snapshot.fast_count = sm_fast_count;
  sm_snapshot.isr_last_us = sm_isr_last_us;
  sm_snapshot.isr_max_us = sm_isr_max_us;

  pid_get_status(&pid_status);
  sm_snapshot.pid = pid_status;
}


static void sm_fast_update(void)
{
  float pid_output;
  uint8_t power = 0U;
  uint32_t start_cycles;
  uint32_t end_cycles;
  uint32_t delta_cycles;
  uint32_t isr_us;

  sm_fast_sequence++;
  start_cycles = ((SysTick_Type *) ((0xE000E000UL) + 0x0010UL) )->VAL;







  if(sm_sample_phase == 0U)
  {
    heater_force_off();
  }

  if(sm_sample_phase == ((uint8_t)((0.5f) * (float)((20000U) / 1000U))))
  {

    adc_read_all(&sm_adc);
    temp_update_fast(&sm_adc, &sm_temp, 1U);


    if((sm_state == SM_STATE_HEATING) && (pid_is_autotuning() != 0U))
    {
      sm_state = SM_STATE_AUTOTUNE;
    }
    else if((sm_state == SM_STATE_AUTOTUNE) && (pid_is_autotuning() == 0U))
    {
      sm_state = SM_STATE_HEATING;
    }

    if((sm_state == SM_STATE_FAULT) && (verifyheater_is_fault() == 0U))
    {
      sm_state = SM_STATE_DISABLED;
    }


    switch(sm_state)
    {
      case SM_STATE_HEATING:
      case SM_STATE_AUTOTUNE:
        if((sm_temp.valid != 0U) && (verifyheater_is_fault() == 0U))
        {
          if(pid_is_autotuning() != 0U)
          {
            sm_autotune_output = pid_update(sm_temp.tip_c, 1U, sm_now_ms);
            pid_output = sm_autotune_output;
          }
          else
          {
            pid_output = pid_update_fixed(sm_temp.tip_c, 1U, SM_SAMPLE_DT_S);
          }

          power = sm_round_power(pid_output);
        }
        else
        {
          power = 0U;
          pid_reset();
        }
        break;

      case SM_STATE_DISABLED:
      case SM_STATE_FAULT:
      case SM_STATE_INIT:
      default:
        power = 0U;
        pid_reset();
        break;
    }

    if(verifyheater_is_fault() != 0U)
    {
      sm_state = SM_STATE_FAULT;
      power = 0U;
    }


    if(verifyheater_is_fault() != 0U)
    {
      sm_state = SM_STATE_FAULT;
      sm_power = 0U;
      heater_force_off();
      pid_reset();
    }
    else
    {
      sm_power = power;
      heater_set_power(power);
    }
  }


  if(sm_tick_in_ms == (0U))
  {
    verifyheater_set_heater_power(sm_power);
    verifyheater_update(&sm_temp, &sm_adc, sm_now_ms);
  }

  if(verifyheater_is_fault() != 0U)
  {
    sm_state = SM_STATE_FAULT;
    sm_power = 0U;
    heater_force_off();
    pid_reset();
  }


  if(sm_tick_in_ms == (10U))
  {
    sm_capture_snapshot();
  }


  end_cycles = ((SysTick_Type *) ((0xE000E000UL) + 0x0010UL) )->VAL;
  delta_cycles = (start_cycles >= end_cycles) ?
                 (start_cycles - end_cycles) :
                 ((((SysTick_Type *) ((0xE000E000UL) + 0x0010UL) )->LOAD - end_cycles) + start_cycles + 1U);

  if(ticks_count_us != 0U)
  {
    isr_us = delta_cycles / ticks_count_us;

    if(isr_us > 65535U)
    {
      isr_us = 65535U;
    }

    sm_isr_last_us = (uint16_t)isr_us;

    if(isr_us > sm_isr_max_us)
    {
      sm_isr_max_us = (uint16_t)isr_us;
    }

    if(sm_tick_in_ms == (10U))
    {
      sm_snapshot.isr_last_us = sm_isr_last_us;
      sm_snapshot.isr_max_us = sm_isr_max_us;
    }
  }


  sm_sample_phase++;
  if(sm_sample_phase >= (SM_TEMP_SAMPLE_MS * SM_TICKS_PER_MS))
  {
    sm_sample_phase = 0U;
  }

  sm_fast_count++;

  sm_tick_in_ms++;
  if(sm_tick_in_ms >= ((20000U) / 1000U))
  {
    sm_tick_in_ms = 0U;
    sm_now_ms++;
  }
}




void TMR1_BRK_OVF_TRG_HALL_IRQHandler(void)
{
  if(tmr_interrupt_flag_get(((tmr_type *) ((((uint32_t)0x40000000) + 0x10000) + 0x2C00)), ((uint32_t)0x000001)) != RESET)
  {
    tmr_flag_clear(((tmr_type *) ((((uint32_t)0x40000000) + 0x10000) + 0x2C00)), ((uint32_t)0x000001));
    sm_fast_update();
  }
}

static void sm_timer_init(void)
{

  tmr_flag_clear(((tmr_type *) ((((uint32_t)0x40000000) + 0x10000) + 0x2C00)), ((uint32_t)0x000001));
  tmr_interrupt_enable(((tmr_type *) ((((uint32_t)0x40000000) + 0x10000) + 0x2C00)), ((uint32_t)0x000001), TRUE);
  nvic_irq_enable(TMR1_BRK_OVF_TRG_HALL_IRQn, 0, 0);
}



void sm_init(void)
{
  adc_init();
  heater_init();
  verifyheater_init();

  pid_init();
  pid_set_setpoint((300.0f));
  pid_set_mode(PID_MODE_STANDARD);

  sm_state = SM_STATE_HEATING;
  sm_power = 0U;
  sm_now_ms = 0U;
  sm_fast_count = 0U;
  sm_tick_in_ms = 0U;
  sm_sample_phase = 0U;
  sm_autotune_output = (90.0f);
  sm_isr_last_us = 0U;
  sm_isr_max_us = 0U;

  sm_capture_snapshot();
  sm_timer_init();
}

void sm_heater_watchdog_init(void)
{
  sm_wd_last_sequence = sm_fast_sequence;
  sm_wd_stall_loops = 0U;
  sm_wd_tripped = 0U;


  wdt_register_write_enable(TRUE);
  wdt_divider_set(WDT_CLK_DIV_64);
  wdt_reload_value_set((2499U));
  wdt_counter_reload();
  wdt_enable();
  wdt_counter_reload();

}

void sm_heater_watchdog_task(void)
{
  uint32_t primask;

  if(sm_fast_sequence != sm_wd_last_sequence)
  {
    sm_wd_last_sequence = sm_fast_sequence;
    sm_wd_stall_loops = 0U;
  }
  else if(sm_wd_stall_loops < 0xFFFFFFFFUL)
  {
    sm_wd_stall_loops++;
  }

  if((sm_wd_stall_loops >= (80U)) &&
     (sm_wd_tripped == 0U))
  {
    sm_wd_tripped = 1U;

    primask = sm_critical_enter();
    sm_power = 0U;
    heater_force_off();
    sm_state = SM_STATE_FAULT;
    sm_critical_exit(primask);

    hms_set_fatal(HMS_CODE_HEATER_WD);
  }






  wdt_counter_reload();
}

sm_state_t sm_get_state(void)
{
  return sm_state;
}

void sm_get_status(sm_status_t *status)
{
  uint32_t primask;

  if(status == 
              ((void *)0)
                  )
  {
    return;
  }

  primask = sm_critical_enter();

  status->tip_c = sm_snapshot.temp.tip_c;
  status->room_c = sm_snapshot.temp.room_c;
  status->setpoint_c = sm_snapshot.pid.setpoint_c;
  status->temp_valid = sm_snapshot.temp.valid;
  status->adc_valid = sm_snapshot.adc.valid;
  status->power = sm_snapshot.power;
  status->heater_on = sm_snapshot.heater_on;
  status->vin_raw = sm_snapshot.adc.vin;
  status->tc_raw = sm_snapshot.adc.thermocouple;
  status->ntc_raw = sm_snapshot.adc.ntc;
  status->state = sm_snapshot.state;
  status->mode = sm_snapshot.pid.mode;
  status->fault = (verifyheater_fault_t)sm_snapshot.fault;
  status->autotune_state = sm_snapshot.pid.autotune_state;
  status->now_ms = sm_snapshot.now_ms;

  sm_critical_exit(primask);
}

uint32_t sm_get_uptime_ms(void)
{
  uint32_t now_ms;
  uint32_t primask = sm_critical_enter();

  now_ms = sm_snapshot.now_ms;

  sm_critical_exit(primask);
  return now_ms;
}

uint8_t sm_set_heater_enabled(uint8_t enabled)
{
  uint32_t primask = sm_critical_enter();

  if(enabled != 0U)
  {
    if(verifyheater_is_fault() != 0U)
    {
      sm_state = SM_STATE_FAULT;
      sm_power = 0U;
      heater_force_off();
      sm_critical_exit(primask);
      return 0U;
    }

    sm_state = SM_STATE_HEATING;
    sm_power = 0U;
    sm_autotune_output = (90.0f);
    pid_reset();
    sm_critical_exit(primask);
    return 1U;
  }

  sm_power = 0U;
  pid_reset();
  heater_force_off();

  sm_state = (verifyheater_is_fault() != 0U) ? SM_STATE_FAULT : SM_STATE_DISABLED;

  sm_critical_exit(primask);
  return 1U;
}

uint8_t sm_is_heater_enabled(void)
{
  return ((sm_state == SM_STATE_HEATING) || (sm_state == SM_STATE_AUTOTUNE)) ? 1U : 0U;
}

uint8_t sm_clear_fault(void)
{
  uint32_t primask = sm_critical_enter();

  verifyheater_reset();
  pid_reset();
  heater_force_off();

  sm_state = SM_STATE_DISABLED;
  sm_power = 0U;

  sm_critical_exit(primask);
  return 1U;
}

uint8_t sm_start_autotune(float setpoint_c)
{
  uint32_t primask = sm_critical_enter();
  uint8_t result = 0U;

  if((sm_state == SM_STATE_FAULT) ||
     (sm_state == SM_STATE_DISABLED) ||
     (sm_state == SM_STATE_AUTOTUNE) ||
     (sm_state == SM_STATE_INIT))
  {
    sm_critical_exit(primask);
    return 0U;
  }

  if(verifyheater_is_fault() != 0U)
  {
    sm_state = SM_STATE_FAULT;
    sm_critical_exit(primask);
    return 0U;
  }

  if(pid_is_autotuning() != 0U)
  {
    sm_critical_exit(primask);
    return 0U;
  }

  if(sm_temp.valid == 0U)
  {
    sm_critical_exit(primask);
    return 0U;
  }

  if(pid_autotune_start(setpoint_c) != 0U)
  {
    sm_state = SM_STATE_AUTOTUNE;
    sm_autotune_output = (90.0f);
    result = 1U;
  }

  sm_critical_exit(primask);
  return result;
}

void sm_cancel_autotune(void)
{
  uint32_t primask = sm_critical_enter();

  pid_autotune_cancel();

  if(sm_state == SM_STATE_AUTOTUNE)
  {
    sm_state = SM_STATE_HEATING;
  }

  sm_critical_exit(primask);
}

uint8_t sm_set_setpoint(float setpoint_c)
{
  uint32_t primask = sm_critical_enter();

  if(sm_state == SM_STATE_AUTOTUNE)
  {
    sm_critical_exit(primask);
    return 0U;
  }

  pid_set_setpoint(setpoint_c);

  sm_critical_exit(primask);
  return 1U;
}

float sm_get_setpoint(void)
{
  float setpoint;
  uint32_t primask = sm_critical_enter();

  setpoint = pid_get_setpoint();

  sm_critical_exit(primask);
  return setpoint;
}

uint8_t sm_set_pid_mode(pid_mode_t mode)
{
  uint32_t primask = sm_critical_enter();

  if((mode >= PID_MODE_COUNT) || (sm_state == SM_STATE_AUTOTUNE))
  {
    sm_critical_exit(primask);
    return 0U;
  }

  pid_set_mode(mode);

  sm_critical_exit(primask);
  return 1U;
}

pid_mode_t sm_get_pid_mode(void)
{
  pid_mode_t mode;
  uint32_t primask = sm_critical_enter();

  mode = pid_get_mode();

  sm_critical_exit(primask);
  return mode;
}

void sm_reset_pid(void)
{
  uint32_t primask = sm_critical_enter();

  pid_reset();

  if(sm_state == SM_STATE_AUTOTUNE)
  {
    sm_state = SM_STATE_HEATING;
  }

  sm_critical_exit(primask);
}

uint8_t sm_get_heater_power(void)
{
  return sm_power;
}

verifyheater_fault_t sm_get_fault(void)
{
  verifyheater_fault_t fault;
  uint32_t primask = sm_critical_enter();

  fault = verifyheater_get_fault();

  sm_critical_exit(primask);
  return fault;
}
