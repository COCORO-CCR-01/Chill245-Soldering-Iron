#pragma GCC optimize("O2")
/**
  **************************************************************************
  * @file     verifyheater.c
  * @brief    加热器异常状态保护
  **************************************************************************
  * 保护内容：
  *   1. 热电偶开路
  *   2. 热电偶短路
  *   3. 30s 内温度无法上升
  *   4. 温度突变太快
  *   5. 严重失温（结合加热器功率判断）
  *   6. 温度超过 500°C
  *   7. 持续 100% 满功率运行 30s
  *
  * 所有故障都会锁存，必须调用 verifyheater_reset() 才能恢复。
  * 加热器功率通过 verifyheater_set_heater_power() 注册，
  * 供“短路 / 不升温 / 严重失温”判断使用。
  **************************************************************************
  */

#include "verifyheater.h"
#include "at32f421.h"
#include "preset.h"
#include "temp.h"
#include <stddef.h>
#include <string.h>

/* ============================ 保护参数 ============================ */
/* 热电偶开路：温度达到 R7 对应理论失效温度，持续 200ms */
#define VERIFY_TC_OPEN_DEBOUNCE_MS          (50U)

/* 热电偶短路：烙铁头曾经热过（>=60°C）后，PA1 回到运放零点，
   且加热功率 >= 30%，持续 100ms。
   冷启动时热电偶本来就在零点，不能据此判短路，交给 30s 不升温保护。 */
#define VERIFY_TC_SHORT_ADC_WINDOW          (8)
#define VERIFY_TC_SHORT_POWER_MIN           (30U)
#define VERIFY_TC_SHORT_HOT_C               (60.0f)
#define VERIFY_TC_SHORT_DEBOUNCE_MS         (100U)

/* 温度数据无效：NTC 开路/短路或 ADC 异常，持续 200ms */
#define VERIFY_TEMP_INVALID_DEBOUNCE_MS     (200U)

/* 30s 不升温：功率 >= 20% 且温度 < 150°C 时监控，30s 内升温 < 10°C */
#define VERIFY_NO_RISE_POWER_MIN            (20U)
#define VERIFY_NO_RISE_ARM_MAX_C            (150.0f)
#define VERIFY_NO_RISE_TIMEOUT_MS           (30000U)
#define VERIFY_NO_RISE_DELTA_C              (10.0f)

/* 温度突变太快：单次采样跳变 > 20°C，或持续上升率 > 300°C/s */
#define VERIFY_TEMP_JUMP_DELTA_C            (20.0f)
#define VERIFY_TEMP_JUMP_RATE_C_PER_S       (300.0f)
#define VERIFY_TEMP_JUMP_DEBOUNCE_MS        (100U)

/* 严重失温：功率 >= 30%，温度比近 2s 峰值低 150°C，持续 100ms */
#define VERIFY_TEMP_DROP_POWER_MIN          (30U)
#define VERIFY_TEMP_DROP_DELTA_C            (150.0f)
#define VERIFY_TEMP_PEAK_HOLD_MS            (2000U)
#define VERIFY_TEMP_DROP_DEBOUNCE_MS        (100U)

/* 过温：温度 > 500°C，持续 100ms */
#define VERIFY_OVERTEMP_C                   (500.0f)
#define VERIFY_OVERTEMP_DEBOUNCE_MS         (100U)

/* 持续满功率：功率 >= 100%，连续 30s */
#define VERIFY_FULL_POWER_THRESHOLD         (100U)
#define VERIFY_FULL_POWER_TIMEOUT_MS        (30000U)

/* 单次 update 允许累计的最大时间，防止调试暂停后误触发去抖 */
#define VERIFY_MAX_DT_MS                    (1000U)

/* 热上电：上电前 5s 不做温度跳变/过温判断，避免冷端/热电偶建立过程误报 */
#define VERIFY_STARTUP_GRACE_MS             (5000U)

/* ============================ 内部状态 ============================ */
typedef struct
{
  verifyheater_fault_t fault;
  uint8_t heater_power;
  uint8_t initialized;
  uint8_t temp_was_invalid;
  uint8_t tip_was_hot;
  uint8_t rise_active;
  uint8_t rise_ok;
  uint8_t full_power_active;

  uint32_t last_ms;
  uint32_t rise_start_ms;
  uint32_t peak_ms;
  uint32_t full_power_start_ms;

  float last_tip_c;
  float peak_tip_c;
  float rise_start_c;

  uint32_t open_ms;
  uint32_t short_ms;
  uint32_t invalid_ms;
  uint32_t overtemp_ms;
  uint32_t jump_ms;
  uint32_t drop_ms;
} verifyheater_state_t;

static verifyheater_state_t verifyheater_state;
static verifyheater_config_t verifyheater_config;
static uint8_t verifyheater_config_initialized;

static void verifyheater_config_defaults(void)
{
  if(verifyheater_config_initialized != 0U)
  {
    return;
  }

  verifyheater_config.overtemp_c = VERIFY_OVERTEMP_C;
  verifyheater_config.temp_jump_delta_c = VERIFY_TEMP_JUMP_DELTA_C;
  verifyheater_config.temp_jump_rate_c_per_s = VERIFY_TEMP_JUMP_RATE_C_PER_S;
  verifyheater_config.no_rise_timeout_ms = VERIFY_NO_RISE_TIMEOUT_MS;
  verifyheater_config.no_rise_delta_c = VERIFY_NO_RISE_DELTA_C;
  verifyheater_config.full_power_timeout_ms = VERIFY_FULL_POWER_TIMEOUT_MS;
  verifyheater_config.temp_drop_delta_c = VERIFY_TEMP_DROP_DELTA_C;
  verifyheater_config_initialized = 1U;
}

/**
  * @brief  锁存故障
  * @param  fault: 故障码
  * @retval none
  */
static void verifyheater_latch(verifyheater_fault_t fault)
{
  verifyheater_state.fault = fault;
  /* 保护动作：故障后功率请求清零，直到 reset */
  verifyheater_state.heater_power = 0U;
}

/* ============================ 对外函数 ============================ */

void verifyheater_init(void)
{
  verifyheater_config_defaults();
  memset(&verifyheater_state, 0, sizeof(verifyheater_state));
  verifyheater_state.fault = VERIFY_HEATER_FAULT_NONE;
}

void verifyheater_reset(void)
{
  verifyheater_init();
}

void verifyheater_get_config(verifyheater_config_t *config)
{
  uint32_t primask;

  if(config == NULL)
  {
    return;
  }

  verifyheater_config_defaults();

  primask = __get_PRIMASK();
  __disable_irq();
  *config = verifyheater_config;
  if(primask == 0U)
  {
    __enable_irq();
  }
}

uint8_t verifyheater_set_config(const verifyheater_config_t *config)
{
  uint32_t primask;

  if(config == NULL)
  {
    return 0U;
  }

  if((config->overtemp_c < 100.0f) || (config->overtemp_c > 600.0f) ||
     (config->temp_jump_delta_c < 1.0f) ||
     (config->temp_jump_delta_c > 200.0f) ||
     (config->temp_jump_rate_c_per_s < 10.0f) ||
     (config->temp_jump_rate_c_per_s > 2000.0f) ||
     (config->no_rise_timeout_ms < 1000U) ||
     (config->no_rise_timeout_ms > 300000U) ||
     (config->no_rise_delta_c < 1.0f) ||
     (config->no_rise_delta_c > 100.0f) ||
     (config->full_power_timeout_ms < 1000U) ||
     (config->full_power_timeout_ms > 300000U) ||
     (config->temp_drop_delta_c < 10.0f) ||
     (config->temp_drop_delta_c > 300.0f))
  {
    return 0U;
  }

  primask = __get_PRIMASK();
  __disable_irq();
  verifyheater_config = *config;
  verifyheater_config_initialized = 1U;
  if(primask == 0U)
  {
    __enable_irq();
  }

  return 1U;
}

void verifyheater_set_heater_power(uint8_t percent)
{
  /* 故障锁存期间强制为 0，避免保护后再次输出功率 */
  if(verifyheater_state.fault != VERIFY_HEATER_FAULT_NONE)
  {
    verifyheater_state.heater_power = 0U;
    return;
  }

  if(percent > 100U)
  {
    percent = 100U;
  }

  verifyheater_state.heater_power = percent;
}

uint8_t verifyheater_get_heater_power(void)
{
  return verifyheater_state.heater_power;
}

uint8_t verifyheater_is_fault(void)
{
  return (verifyheater_state.fault != VERIFY_HEATER_FAULT_NONE) ? 1U : 0U;
}

verifyheater_fault_t verifyheater_get_fault(void)
{
  return verifyheater_state.fault;
}

void verifyheater_update(const temp_data_t *temp,
                         const adc_raw_data_t *adc_raw,
                         uint32_t now_ms)
{
  uint32_t dt_ms;
  float delta_c;
  float abs_delta_c;
  float rate_c_s;
  int32_t short_diff;
  uint8_t open_cond;
  uint8_t short_cond;
  uint8_t drop_cond;

  if((temp == NULL) || (adc_raw == NULL))
  {
    return;
  }

  /* 故障锁存后不再更新，必须 reset */
  if(verifyheater_state.fault != VERIFY_HEATER_FAULT_NONE)
  {
    return;
  }

  /* 第一次调用：记录温度和时间基准 */
  if(verifyheater_state.initialized == 0U)
  {
    verifyheater_state.initialized = 1U;
    verifyheater_state.last_ms = now_ms;
    verifyheater_state.last_tip_c = temp->tip_c;
    verifyheater_state.peak_tip_c = temp->tip_c;
    verifyheater_state.peak_ms = now_ms;
    verifyheater_state.temp_was_invalid = (temp->valid == 0U) ? 1U : 0U;
  }

  dt_ms = now_ms - verifyheater_state.last_ms;
  verifyheater_state.last_ms = now_ms;

  if(dt_ms > VERIFY_MAX_DT_MS)
  {
    dt_ms = VERIFY_MAX_DT_MS;
  }

  /* 0. 持续满功率保护：100% 连续运行 30s 直接锁存故障 */
  if(verifyheater_state.heater_power >= VERIFY_FULL_POWER_THRESHOLD)
  {
    if(verifyheater_state.full_power_active == 0U)
    {
      verifyheater_state.full_power_active = 1U;
      verifyheater_state.full_power_start_ms = now_ms;
    }
    else if((uint32_t)(now_ms - verifyheater_state.full_power_start_ms) >=
            verifyheater_config.full_power_timeout_ms)
    {
      verifyheater_latch(VERIFY_HEATER_FAULT_FULL_POWER);
      return;
    }
  }
  else
  {
    verifyheater_state.full_power_active = 0U;
    verifyheater_state.full_power_start_ms = 0U;
  }

  /* 1. 热电偶开路：温度 >= R7 对应理论失效温度 */
  open_cond = 0U;

  if((adc_raw->valid != 0U) && (temp->valid != 0U))
  {
    /* 用未滤波的瞬时热电偶温度，避免滤波滞后让 OVERTEMP 抢先锁存 */
    if(temp_thermocouple_default_celsius(adc_raw->thermocouple,
                                         temp->room_c) >=
       PRESET_TC_OPEN_TEMP_C)
    {
      open_cond = 1U;
      verifyheater_state.open_ms += dt_ms;

      if(verifyheater_state.open_ms >= VERIFY_TC_OPEN_DEBOUNCE_MS)
      {
        verifyheater_latch(VERIFY_HEATER_FAULT_TC_OPEN);
        return;
      }
    }
    else
    {
      verifyheater_state.open_ms = 0U;
    }
  }
  else
  {
    verifyheater_state.open_ms = 0U;
  }

  /* 记录本次上电后烙铁头是否曾经热过。
     冷启动时热电偶本来就在零点，不能仅凭零点判短路。 */
  if(verifyheater_state.heater_power == 0U)
  {
    verifyheater_state.tip_was_hot = 0U;
  }
  else if((temp->valid != 0U) && (temp->tip_c >= VERIFY_TC_SHORT_HOT_C))
  {
    verifyheater_state.tip_was_hot = 1U;
  }

  /* 2. 热电偶短路：烙铁头曾经热过，PA1 又回到运放零点 */
  short_diff = (int32_t)adc_raw->thermocouple - (int32_t)temp_get_thermocouple_zero_adc();

  if(short_diff < 0)
  {
    short_diff = -short_diff;
  }

  short_cond = 0U;

  if((adc_raw->valid != 0U) &&
     (verifyheater_state.heater_power >= VERIFY_TC_SHORT_POWER_MIN) &&
     (verifyheater_state.tip_was_hot != 0U) &&
     (short_diff <= VERIFY_TC_SHORT_ADC_WINDOW))
  {
    short_cond = 1U;
    verifyheater_state.short_ms += dt_ms;

    if(verifyheater_state.short_ms >= VERIFY_TC_SHORT_DEBOUNCE_MS)
    {
      verifyheater_latch(VERIFY_HEATER_FAULT_TC_SHORT);
      return;
    }
  }
  else
  {
    verifyheater_state.short_ms = 0U;
  }

  /* 3. 温度数据无效：NTC 开路/短路或 ADC 异常 */
  if(temp->valid == 0U)
  {
    verifyheater_state.invalid_ms += dt_ms;

    if(verifyheater_state.invalid_ms >= VERIFY_TEMP_INVALID_DEBOUNCE_MS)
    {
      verifyheater_latch(VERIFY_HEATER_FAULT_TEMP_INVALID);
      return;
    }

    verifyheater_state.temp_was_invalid = 1U;
    verifyheater_state.rise_active = 0U;
    verifyheater_state.rise_ok = 0U;
    return;
  }

  verifyheater_state.invalid_ms = 0U;

  /* 4. 过温保护：上电 5s 内屏蔽，避免热上电瞬间误报 */
  if((now_ms >= VERIFY_STARTUP_GRACE_MS) &&
     (temp->tip_c > verifyheater_config.overtemp_c))
  {
    verifyheater_state.overtemp_ms += dt_ms;

    if(verifyheater_state.overtemp_ms >= VERIFY_OVERTEMP_DEBOUNCE_MS)
    {
      verifyheater_latch(VERIFY_HEATER_FAULT_OVERTEMP);
      return;
    }
  }
  else
  {
    verifyheater_state.overtemp_ms = 0U;
  }

  /* 5. 从无效数据恢复：重置温度历史，避免把恢复瞬间误判为跳变 */
  if(verifyheater_state.temp_was_invalid != 0U)
  {
    verifyheater_state.temp_was_invalid = 0U;
    verifyheater_state.last_tip_c = temp->tip_c;
    verifyheater_state.peak_tip_c = temp->tip_c;
    verifyheater_state.peak_ms = now_ms;
    verifyheater_state.jump_ms = 0U;
    verifyheater_state.drop_ms = 0U;
    verifyheater_state.rise_active = 0U;
    verifyheater_state.rise_ok = 0U;
    return;
  }

  /* 6. 30s 温度无法上升 */
  if((verifyheater_state.heater_power >= VERIFY_NO_RISE_POWER_MIN) &&
     (temp->tip_c < VERIFY_NO_RISE_ARM_MAX_C))
  {
    if(verifyheater_state.rise_active == 0U)
    {
      verifyheater_state.rise_active = 1U;
      verifyheater_state.rise_ok = 0U;
      verifyheater_state.rise_start_c = temp->tip_c;
      verifyheater_state.rise_start_ms = now_ms;
    }
    else if(verifyheater_state.rise_ok == 0U)
    {
      if((temp->tip_c - verifyheater_state.rise_start_c) >=
     verifyheater_config.no_rise_delta_c)
      {
        verifyheater_state.rise_ok = 1U;
      }
      else if((uint32_t)(now_ms - verifyheater_state.rise_start_ms) >=
          verifyheater_config.no_rise_timeout_ms)
      {
        verifyheater_latch(VERIFY_HEATER_FAULT_NO_RISE);
        return;
      }
    }
  }
  else
  {
    verifyheater_state.rise_active = 0U;
    verifyheater_state.rise_ok = 0U;
  }

  /* 7. 温度变化率 */
  delta_c = temp->tip_c - verifyheater_state.last_tip_c;
  abs_delta_c = (delta_c < 0.0f) ? -delta_c : delta_c;

  if(dt_ms > 0U)
  {
    rate_c_s = (delta_c * 1000.0f) / (float)dt_ms;
  }
  else
  {
    rate_c_s = 0.0f;
  }

  /* 开路/短路确认期间跳过温度变化率判断，避免先被误判为温度突变 */
  if((short_cond == 0U) && (open_cond == 0U))
  {
    /* 8. 严重失温：结合加热器功率和近期温度峰值判断 */
    drop_cond = 0U;

    if(verifyheater_state.heater_power >= VERIFY_TEMP_DROP_POWER_MIN)
    {
      if(temp->tip_c > verifyheater_state.peak_tip_c)
      {
        verifyheater_state.peak_tip_c = temp->tip_c;
        verifyheater_state.peak_ms = now_ms;
      }
      else if((uint32_t)(now_ms - verifyheater_state.peak_ms) > VERIFY_TEMP_PEAK_HOLD_MS)
      {
        verifyheater_state.peak_tip_c = temp->tip_c;
        verifyheater_state.peak_ms = now_ms;
      }

      if((verifyheater_state.peak_tip_c - temp->tip_c) >
     verifyheater_config.temp_drop_delta_c)
      {
        drop_cond = 1U;
      }
    }
    else
    {
      verifyheater_state.peak_tip_c = temp->tip_c;
      verifyheater_state.peak_ms = now_ms;
    }

    if(drop_cond != 0U)
    {
      verifyheater_state.drop_ms += dt_ms;
      verifyheater_state.jump_ms = 0U;

      if(verifyheater_state.drop_ms >= VERIFY_TEMP_DROP_DEBOUNCE_MS)
      {
        verifyheater_latch(VERIFY_HEATER_FAULT_TEMP_DROP);
        return;
      }
    }
    else
    {
      verifyheater_state.drop_ms = 0U;

      /* 上电 5s 内不做温度突变判断；last_tip 仍在末尾持续更新，
         5s 后不会把热上电建立过程当成一次跳变。 */
      if(now_ms < VERIFY_STARTUP_GRACE_MS)
      {
        verifyheater_state.jump_ms = 0U;
      }
      else
      {
        /* 9. 温度突变太快：单次跳变立即判断，持续上升率做去抖 */
        if(abs_delta_c > verifyheater_config.temp_jump_delta_c)
        {
          verifyheater_latch(VERIFY_HEATER_FAULT_TEMP_JUMP);
          return;
        }

        if(rate_c_s > verifyheater_config.temp_jump_rate_c_per_s)
        {
          verifyheater_state.jump_ms += dt_ms;

          if(verifyheater_state.jump_ms >= VERIFY_TEMP_JUMP_DEBOUNCE_MS)
          {
            verifyheater_latch(VERIFY_HEATER_FAULT_TEMP_JUMP);
            return;
          }
        }
        else
        {
          verifyheater_state.jump_ms = 0U;
        }
      }
    }
  }
  else
  {
    verifyheater_state.drop_ms = 0U;
    verifyheater_state.jump_ms = 0U;
  }

  verifyheater_state.last_tip_c = temp->tip_c;
}
