/**
  **************************************************************************
  * @file     statemachine.c
  * @brief    20 kHz 控制状态机与模块统一调度
  **************************************************************************
  * 控制环（TMR1 更新中断，20 kHz / 50 µs）：
  *   ADC -> 温度 -> PID -> 保护(1 kHz) -> 20 kHz PWM -> 快照
  *
  * 后台环（主循环，1 kHz）：
  *   读取控制快照 -> 调试串口命令/数据流
  *
  * 分工：
  *   - 20 kHz：ADC、热电偶温度、PID、PWM、状态切换、故障切断
  *   - 1 kHz：verifyheater 时间型保护、自动整定步骤、调试快照
  *   - 100 Hz：NTC 冷端温度（物理变化慢）
  *   - 1 kHz：串口控制台
  *
  * 并发：
  *   - TMR1 更新中断优先级 0，负责全部控制状态；
  *   - 主循环只读快照，命令通过带临界区的 API 修改控制状态；
  *   - USART1 中断只访问调试环形缓冲。
  **************************************************************************
  */

#include "statemachine.h"
#include "adc.h"
#include "temp.h"
#include "verifyheater.h"
#include "pid.h"
#include "heater.h"
#include "debug.h"
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
 *   测量周期 10 ms（100 Hz），其中 0.5 ms 消隐，9.5 ms 加热。
 */
#define SM_HEATER_BLANK_MS              (0.5f)
#define SM_HEATER_BLANK_TICKS           ((uint8_t)(SM_HEATER_BLANK_MS * (float)SM_TICKS_PER_MS))
#define SM_TEMP_SAMPLE_MS               (10U)
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

/* ============================ 内部状态 ============================ */
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

/* 控制中断写，主循环在临界区内复制 */
static volatile sm_snapshot_t sm_snapshot;

/* 主循环调试副本 */
static sm_snapshot_t sm_debug_data;

/* ============================ 内部函数声明 ============================ */
static void sm_fast_update(void);
static void sm_timer_init(void);
static void sm_capture_snapshot(void);
static uint32_t sm_critical_enter(void);
static void sm_critical_exit(uint32_t primask);
static void sm_register_debug(void);
static void sm_debug_trigger_pidreset(void);
static void sm_debug_trigger_faultreset(void);
static void sm_debug_trigger_boost(void);
static void sm_debug_trigger_standard(void);
static void sm_debug_trigger_heater_off(void);
static void sm_debug_trigger_heater_on(void);
static void sm_debug_trigger_autotune_stop(void);
static void sm_debug_cmd_setpoint(int argc, char **argv);
static void sm_debug_cmd_mode(int argc, char **argv);
static void sm_debug_cmd_autotune(int argc, char **argv);
static void sm_debug_cmd_pid(int argc, char **argv);
static void sm_debug_cmd_state(int argc, char **argv);
static void sm_debug_cmd_perf(int argc, char **argv);
static uint8_t sm_streq(const char *a, const char *b);

/* ============================ 工具函数 ============================ */
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

static uint8_t sm_streq(const char *a, const char *b)
{
  char ca;
  char cb;

  if((a == NULL) || (b == NULL))
  {
    return 0U;
  }

  while((*a != '\0') && (*b != '\0'))
  {
    ca = *a;
    cb = *b;

    if((ca >= 'A') && (ca <= 'Z'))
    {
      ca = (char)(ca + ('a' - 'A'));
    }

    if((cb >= 'A') && (cb <= 'Z'))
    {
      cb = (char)(cb + ('a' - 'A'));
    }

    if(ca != cb)
    {
      return 0U;
    }

    a++;
    b++;
  }

  return ((*a == '\0') && (*b == '\0')) ? 1U : 0U;
}

const char *sm_state_name(sm_state_t state)
{
  switch(state)
  {
    case SM_STATE_INIT:
      return "INIT";
    case SM_STATE_HEATING:
      return "HEATING";
    case SM_STATE_AUTOTUNE:
      return "AUTOTUNE";
    case SM_STATE_DISABLED:
      return "DISABLED";
    case SM_STATE_FAULT:
      return "FAULT";
    default:
      return "UNKNOWN";
  }
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

/** 20 kHz 控制中断，必须小于 50 µs */
static void sm_fast_update(void)
{
  float pid_output;
  uint8_t power = 0U;
  uint32_t start_cycles;
  uint32_t end_cycles;
  uint32_t delta_cycles;
  uint32_t isr_us;

  sm_fast_sequence++;
  start_cycles = SysTick->VAL;

  /*
   * C245 消隐采样：
   *   phase 0             -> 强制关加热
   *   phase BLANK_TICKS   -> 已关 0.5 ms，读取 ADC/PID/恢复 PWM
   *   其余 phase          -> 保持上一拍 PWM
   */
  if(sm_sample_phase == 0U)
  {
    heater_force_off();
  }

  if(sm_sample_phase == SM_HEATER_BLANK_TICKS)
  {
    /* 1. 三路 ADC + 温度（加热已关闭 0.5 ms） */
    adc_read_all(&sm_adc);
    temp_update_fast(&sm_adc, &sm_temp, 1U);

    /* 2. 状态同步 */
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

    /* 3. 100 Hz PID / 自动整定 */
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

    /* 4. 采样结束后恢复 20 kHz PWM */
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

  /* 5. 保护 1 kHz；故障时下一拍立即切断 */
  if(sm_tick_in_ms == SM_PROT_PHASE)
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

  /* 6. 1 kHz 快照（放在耗时统计之前，把快照开销也算进中断时间） */
  if(sm_tick_in_ms == SM_SNAPSHOT_PHASE)
  {
    sm_capture_snapshot();
  }

  /* 7. 中断耗时统计 */
  end_cycles = SysTick->VAL;
  delta_cycles = (start_cycles >= end_cycles) ?
                 (start_cycles - end_cycles) :
                 ((SysTick->LOAD - end_cycles) + start_cycles + 1U);

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

    if(sm_tick_in_ms == SM_SNAPSHOT_PHASE)
    {
      sm_snapshot.isr_last_us = sm_isr_last_us;
      sm_snapshot.isr_max_us = sm_isr_max_us;
    }
  }

  /* 8. 采样周期计数 */
  sm_sample_phase++;
  if(sm_sample_phase >= SM_TEMP_SAMPLE_TICKS)
  {
    sm_sample_phase = 0U;
  }

  sm_fast_count++;

  sm_tick_in_ms++;
  if(sm_tick_in_ms >= SM_TICKS_PER_MS)
  {
    sm_tick_in_ms = 0U;
    sm_now_ms++;
  }
}

/**
  * @brief  TMR1 更新中断：20 kHz 控制节拍
  */
void TMR1_BRK_OVF_TRG_HALL_IRQHandler(void)
{
  if(tmr_interrupt_flag_get(TMR1, TMR_OVF_FLAG) != RESET)
  {
    tmr_flag_clear(TMR1, TMR_OVF_FLAG);
    sm_fast_update();
  }
}

static void sm_timer_init(void)
{
  /* TMR1 PWM 已在 heater_init() 启动，这里只使能 20 kHz 更新中断 */
  tmr_flag_clear(TMR1, TMR_OVF_FLAG);
  tmr_interrupt_enable(TMR1, TMR_OVF_INT, TRUE);
  nvic_irq_enable(TMR1_BRK_OVF_TRG_HALL_IRQn, 0, 0);
}

/* ============================ 对外 API ============================ */

void sm_init(void)
{
  adc_init();
  heater_init();
  verifyheater_init();

  pid_init();
  pid_set_setpoint(SM_DEFAULT_SETPOINT_C);
  pid_set_mode(PID_MODE_STANDARD);

  sm_register_debug();

  sm_state = SM_STATE_HEATING;
  sm_power = 0U;
  sm_now_ms = 0U;
  sm_fast_count = 0U;
  sm_tick_in_ms = 0U;
  sm_sample_phase = 0U;
  sm_autotune_output = SM_AUTOTUNE_SEED_POWER;
  sm_isr_last_us = 0U;
  sm_isr_max_us = 0U;

  sm_capture_snapshot();

#if SM_AUTOTUNE_ON_BOOT
  if(pid_autotune_start(SM_DEFAULT_SETPOINT_C) != 0U)
  {
    sm_state = SM_STATE_AUTOTUNE;
    sm_autotune_output = SM_AUTOTUNE_SEED_POWER;
  }
#endif

  sm_timer_init();
}

void sm_heater_watchdog_init(void)
{
  sm_wd_last_sequence = sm_fast_sequence;
  sm_wd_stall_loops = 0U;
  sm_wd_tripped = 0U;

#if SM_ENABLE_HARDWARE_WDT
  wdt_register_write_enable(TRUE);
  wdt_divider_set(WDT_CLK_DIV_64);
  wdt_reload_value_set(SM_WDT_RELOAD_VALUE);
  wdt_counter_reload();
  wdt_enable();
  wdt_counter_reload();
#endif
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

  if((sm_wd_stall_loops >= SM_HEATER_WD_STALL_LOOPS) &&
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

  /*
   * 主循环始终重装 WDT：
   * - 若存在上一次运行遗留的硬件 WDT，避免启动后再次复位
   * - 默认未启用新 WDT，重装命令无副作用
   */
  wdt_counter_reload();
}

void sm_slow_update(void)
{
  uint32_t primask = sm_critical_enter();

  sm_debug_data = sm_snapshot;

  sm_critical_exit(primask);

  debug_update(&sm_debug_data.adc, &sm_debug_data.temp, sm_debug_data.now_ms);
}

sm_state_t sm_get_state(void)
{
  return sm_state;
}

void sm_get_status(sm_status_t *status)
{
  uint32_t primask;

  if(status == NULL)
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
    sm_autotune_output = SM_AUTOTUNE_SEED_POWER;
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
    sm_autotune_output = SM_AUTOTUNE_SEED_POWER;
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

/* ============================ 串口调试注册 ============================ */

static void sm_register_debug(void)
{
  (void)debug_register_trigger("pidreset", "reset PID runtime", sm_debug_trigger_pidreset);
  (void)debug_register_trigger("faultreset", "clear fault, heater disabled", sm_debug_trigger_faultreset);
  (void)debug_register_trigger("boost", "use boost curve", sm_debug_trigger_boost);
  (void)debug_register_trigger("standard", "use standard curve", sm_debug_trigger_standard);
  (void)debug_register_trigger("heateroff", "disable heater output", sm_debug_trigger_heater_off);
  (void)debug_register_trigger("heateron", "enable heater output", sm_debug_trigger_heater_on);
  (void)debug_register_trigger("autotunestop", "cancel auto tune", sm_debug_trigger_autotune_stop);

  (void)debug_register_command("setpoint", "setpoint <C>", sm_debug_cmd_setpoint);
  (void)debug_register_command("mode", "mode std|boost|custom", sm_debug_cmd_mode);
  (void)debug_register_command("autotune", "autotune [setpoint C]", sm_debug_cmd_autotune);
  (void)debug_register_command("pid", "print PID status", sm_debug_cmd_pid);
  (void)debug_register_command("state", "print system state", sm_debug_cmd_state);
  (void)debug_register_command("perf", "perf [reset]", sm_debug_cmd_perf);
}

static void sm_debug_trigger_pidreset(void)
{
  sm_reset_pid();
  debug_write("pid reset\r\n");
}

static void sm_debug_trigger_faultreset(void)
{
  (void)sm_clear_fault();
  debug_write("fault cleared, heater disabled (use heateron)\r\n");
}

static void sm_debug_trigger_boost(void)
{
  if(sm_set_pid_mode(PID_MODE_BOOST) != 0U)
  {
    debug_write("mode=boost\r\n");
  }
  else
  {
    debug_write("busy: autotune running\r\n");
  }
}

static void sm_debug_trigger_standard(void)
{
  if(sm_set_pid_mode(PID_MODE_STANDARD) != 0U)
  {
    debug_write("mode=standard\r\n");
  }
  else
  {
    debug_write("busy: autotune running\r\n");
  }
}

static void sm_debug_trigger_heater_off(void)
{
  (void)sm_set_heater_enabled(0U);
  debug_write("heater disabled\r\n");
}

static void sm_debug_trigger_heater_on(void)
{
  if(sm_set_heater_enabled(1U) != 0U)
  {
    debug_write("heater enabled\r\n");
  }
  else
  {
    debug_write("heater enable rejected: fault latched (use faultreset)\r\n");
  }
}

static void sm_debug_trigger_autotune_stop(void)
{
  sm_cancel_autotune();
  debug_write("autotune cancelled\r\n");
}

static void sm_debug_cmd_setpoint(int argc, char **argv)
{
  if(argc < 2)
  {
    debug_write("usage: setpoint <C>\r\n");
    return;
  }

  if(sm_set_setpoint((float)atoi(argv[1])) != 0U)
  {
    debug_printf("setpoint=%d C\r\n", (int)sm_get_setpoint());
  }
  else
  {
    debug_write("busy: autotune running\r\n");
  }
}

static void sm_debug_cmd_mode(int argc, char **argv)
{
  if(argc < 2)
  {
    debug_write("usage: mode std|boost|custom\r\n");
    return;
  }

  if((sm_streq(argv[1], "std") != 0U) || (sm_streq(argv[1], "standard") != 0U))
  {
    if(sm_set_pid_mode(PID_MODE_STANDARD) != 0U)
    {
      debug_write("mode=standard\r\n");
    }
    else
    {
      debug_write("busy: autotune running\r\n");
    }
  }
  else if(sm_streq(argv[1], "boost") != 0U)
  {
    if(sm_set_pid_mode(PID_MODE_BOOST) != 0U)
    {
      debug_write("mode=boost\r\n");
    }
    else
    {
      debug_write("busy: autotune running\r\n");
    }
  }
  else if(sm_streq(argv[1], "custom") != 0U)
  {
    if(sm_set_pid_mode(PID_MODE_CUSTOM) != 0U)
    {
      debug_write("mode=custom\r\n");
    }
    else
    {
      debug_write("busy: autotune running\r\n");
    }
  }
  else
  {
    debug_write("usage: mode std|boost|custom\r\n");
  }
}

static void sm_debug_cmd_autotune(int argc, char **argv)
{
  float setpoint = sm_get_setpoint();

  if(argc >= 2)
  {
    setpoint = (float)atoi(argv[1]);
  }

  if(sm_start_autotune(setpoint) != 0U)
  {
    debug_printf("autotune start, sp=%d C\r\n", (int)setpoint);
  }
  else
  {
    debug_write("autotune rejected (state/fault/temp)\r\n");
  }
}

static void sm_debug_cmd_pid(int argc, char **argv)
{
  float error_c;

  (void)argc;
  (void)argv;

  error_c = sm_debug_data.pid.setpoint_c - sm_debug_data.temp.tip_c;

  debug_printf("state=%s sp=%d temp=", sm_state_name(sm_debug_data.state),
               (int)sm_debug_data.pid.setpoint_c);
  debug_write_float(sm_debug_data.temp.tip_c, 2);
  debug_printf("C out=%d%% mode=%d at=%d err=",
               (int)sm_debug_data.pid.output,
               (int)sm_debug_data.pid.mode,
               (int)sm_debug_data.pid.autotune_state);
  debug_write_float(error_c, 2);
  debug_write("C\r\n");
}

static void sm_debug_cmd_state(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  debug_printf("state=%s power=%u heater=%u fault=%u at=%d temp=",
               sm_state_name(sm_debug_data.state),
               (unsigned)sm_debug_data.power,
               (unsigned)sm_debug_data.heater_on,
               (unsigned)sm_debug_data.fault,
               (int)sm_debug_data.pid.autotune_state);
  debug_write_float(sm_debug_data.temp.tip_c, 2);
  debug_write("C\r\n");
}

static void sm_debug_cmd_perf(int argc, char **argv)
{
  uint16_t max_us;

  if((argc >= 2) && (sm_streq(argv[1], "reset") != 0U))
  {
    uint32_t primask = sm_critical_enter();
    sm_isr_max_us = 0U;
    sm_critical_exit(primask);
    sm_debug_data.isr_max_us = 0U;
  }

  max_us = sm_debug_data.isr_max_us;

  debug_printf("perf: tick=%u ms=%u last=%u us max=%u us budget=%u us\r\n",
               (unsigned)sm_debug_data.fast_count,
               (unsigned)sm_debug_data.now_ms,
               (unsigned)sm_debug_data.isr_last_us,
               (unsigned)max_us,
               (unsigned)SM_FAST_PERIOD_US);

  if(max_us > SM_FAST_PERIOD_US)
  {
    debug_write("WARN: 20 kHz over budget, reduce ISR work\r\n");
  }
  else if(max_us > (SM_FAST_PERIOD_US - 2U))
  {
    debug_write("WARN: 20 kHz margin < 2 us\r\n");
  }
}
