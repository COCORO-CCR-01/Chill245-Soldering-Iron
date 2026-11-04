#pragma GCC optimize("O2")
/**
  **************************************************************************
  * @file     pid.c
  * @brief    烙铁温度 PID 控制 + PWM 双电平自动整定 + 双曲线
  **************************************************************************
  * 曲线：
  *   STANDARD  自动整定得到，采用 Tyreus-Luyben 规则，偏稳健；
  *   BOOST     由 STANDARD 自动放大得到，响应更激进；
  *   CUSTOM    用户通过接口自定义。
  *
  * 自动整定：
  *   通过 heater 驱动 PMOS，以 10%/90% 两个 PWM 电平激励，
  *   让温度围绕目标值振荡，测量临界增益 Ku 和临界周期 Pu，
  *   再计算标准曲线。整个过程中 PMOS 始终由 PWM 控制。
  *
  * 输出：
  *   pid_update() 返回 0~100% 的加热功率，
  *   可直接交给 heater.c 或 verifyheater_set_heater_power()。
  **************************************************************************
  */

#include "pid.h"
#include "at32f421.h"
#include <stddef.h>
#include <string.h>

/* ============================ PID 参数 ============================ */
#define PID_OUTPUT_MIN                  (0.0f)
#define PID_OUTPUT_MAX                  (100.0f)
#define PID_INTEGRAL_MIN                (-50.0f)
#define PID_INTEGRAL_MAX                (100.0f)

#define PID_SETPOINT_MIN_C              (0.0f)
#define PID_SETPOINT_MAX_C              (450.0f)
#define PID_DEFAULT_SETPOINT_C          (300.0f)

#define PID_DEFAULT_KP                  (1.2f)
#define PID_DEFAULT_KI                  (0.04f)
#define PID_DEFAULT_KD                  (1.2f)

#define PID_I_BAND_C                    (30.0f)
#define PID_BULK_BAND_C                 (15.0f)
#define PID_NEAR_BAND_C                 (10.0f)
#define PID_NEAR_FLOOR_PER_C            (4.0f)
#define PID_FF_PER_C                    (0.05f)
#define PID_FF_MAX                      (30.0f)
#define PID_FF_TAPER_C                  (5.0f)

#define PID_AT_KP_DIV                   (2.8f)
#define PID_AT_TI_MULT                  (2.8f)
#define PID_AT_INTEGRAL_INIT_MAX        (35.0f)

#define PID_BOOST_KP_SCALE              (1.6f)
#define PID_BOOST_KI_SCALE              (1.4f)
#define PID_BOOST_KD_SCALE              (1.2f)

#define PID_DERIVATIVE_FILTER           (0.10f)
#define PID_DERIVATIVE_REF_DT_S         (0.01f)

#define PID_MAX_DT_S                    (1.0f)

#define PID_AT_PWM_HIGH                 (90.0f)
#define PID_AT_PWM_LOW                  (10.0f)
#define PID_AT_HYSTERESIS_C             (10.0f)
#define PID_AT_CYCLES                   (3U)
#define PID_AT_TIMEOUT_MS               (180000U)
#define PID_AT_PHASE_TIMEOUT_MS         (90000U)
#define PID_AT_MAX_TEMP_C               (450.0f)
#define PID_AT_MIN_AMPLITUDE_C          (5.0f)
#define PID_AT_MIN_SETPOINT_C           (50.0f)
#define PID_AT_MAX_SETPOINT_C           (400.0f)
#define PID_AT_PI                       (3.14159265f)

/* ============================ 内部状态 ============================ */
typedef enum
{
  PID_AT_PHASE_IDLE = 0,
  PID_AT_PHASE_HIGH,
  PID_AT_PHASE_LOW
} pid_at_phase_t;

typedef struct
{
  pid_curve_t curves[PID_MODE_COUNT];
  pid_mode_t mode;
  float setpoint_c;

  float integral;
  float prev_temp_c;
  float last_temp_c;
  float d_filtered;
  float output;

  uint8_t time_initialized;
  uint32_t last_ms;
} pid_state_t;

typedef struct
{
  pid_autotune_state_t state;
  pid_autotune_error_t error;
  pid_at_phase_t phase;

  uint8_t started;
  float setpoint_c;
  uint32_t start_ms;
  uint32_t phase_start_ms;
  uint32_t last_low_to_high_ms;
  uint32_t period_accum_ms;
  uint32_t high_time_accum_ms;
  uint32_t low_time_accum_ms;
  uint8_t period_count;
  uint8_t duty_started;
  float amplitude_accum_c;
  float last_high_peak_c;
  float phase_max_c;
  float phase_min_c;

  pid_autotune_result_t result;
} pid_autotune_t;

static pid_state_t pid_state;
static pid_custom_point_t pid_custom_points[PID_CUSTOM_POINTS];
static pid_autotune_t pid_at;

/* ============================ 内部函数 ============================ */

static float pid_clampf(float value, float min_value, float max_value)
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

static uint8_t pid_curve_is_valid(const pid_curve_t *curve)
{
  if(curve == NULL)
  {
    return 0U;
  }

  if((curve->kp < 0.0f) || (curve->ki < 0.0f) || (curve->kd < 0.0f))
  {
    return 0U;
  }

  return 1U;
}

static pid_curve_t pid_custom_interpolate(float temp_c)
{
  uint32_t i;
  pid_curve_t result;

  if(temp_c <= pid_custom_points[0].temp_c)
  {
    return pid_custom_points[0].gain;
  }

  if(temp_c >= pid_custom_points[PID_CUSTOM_POINTS - 1U].temp_c)
  {
    return pid_custom_points[PID_CUSTOM_POINTS - 1U].gain;
  }

  for(i = 0U; i < (PID_CUSTOM_POINTS - 1U); i++)
  {
    if(temp_c <= pid_custom_points[i + 1U].temp_c)
    {
      float t0 = pid_custom_points[i].temp_c;
      float t1 = pid_custom_points[i + 1U].temp_c;
      float ratio = (temp_c - t0) / (t1 - t0);

      result.kp = pid_custom_points[i].gain.kp +
                  (ratio * (pid_custom_points[i + 1U].gain.kp -
                            pid_custom_points[i].gain.kp));
      result.ki = pid_custom_points[i].gain.ki +
                  (ratio * (pid_custom_points[i + 1U].gain.ki -
                            pid_custom_points[i].gain.ki));
      result.kd = pid_custom_points[i].gain.kd +
                  (ratio * (pid_custom_points[i + 1U].gain.kd -
                            pid_custom_points[i].gain.kd));
      return result;
    }
  }

  return pid_custom_points[PID_CUSTOM_POINTS - 1U].gain;
}

static void pid_reset_runtime(void)
{
  pid_state.integral = 0.0f;
  pid_state.prev_temp_c = 0.0f;
  pid_state.last_temp_c = 0.0f;
  pid_state.d_filtered = 0.0f;
  pid_state.output = 0.0f;
  pid_state.time_initialized = 0U;
  pid_state.last_ms = 0U;
}

static void pid_autotune_fail(pid_autotune_error_t error)
{
  pid_at.state = PID_AUTOTUNE_FAILED;
  pid_at.error = error;
  pid_at.phase = PID_AT_PHASE_IDLE;
  pid_reset_runtime();
}

/**
  * @brief  整定完成：计算 Ku/Pu，生成标准曲线和 boost 曲线
  * @param  temperature_c: 整定结束时的温度
  * @param  now_ms: 当前时间戳
  * @note   用高电平 PWM 占空比估算维持目标温度所需的平均功率，
  *         作为 PID 初始积分，避免整定完成后输出从 0 开始造成掉温。
  */
static void pid_autotune_finish(float temperature_c, uint32_t now_ms)
{
  float period_ms;
  float amplitude_c;
  float pu_s;
  float ku;
  float ti_s;
  float kp;
  float ki;
  float kd;
  float duty_percent;
  float high_fraction;
  float feedforward;
  uint32_t total_time_ms;
  pid_curve_t standard_curve;

  if(pid_at.period_count == 0U)
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_NO_OSCILLATION);
    return;
  }

  period_ms = (float)pid_at.period_accum_ms / (float)pid_at.period_count;
  amplitude_c = pid_at.amplitude_accum_c / (float)pid_at.period_count;
  pu_s = period_ms / 1000.0f;

  if((pu_s <= 0.0f) || (amplitude_c < PID_AT_MIN_AMPLITUDE_C))
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_NO_OSCILLATION);
    return;
  }

  /* PWM 双电平激励：Ku = 4d / (πa)，d 为 PWM 电平幅值，a 为温度幅值 */
  ku = (4.0f * ((PID_AT_PWM_HIGH - PID_AT_PWM_LOW) * 0.5f)) / (PID_AT_PI * amplitude_c);

  if(ku <= 0.0f)
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_NO_OSCILLATION);
    return;
  }

  /* Tyreus-Luyben 规则，并进一步放慢积分，抑制热惯性超调 */
  kp = ku / PID_AT_KP_DIV;
  ti_s = PID_AT_TI_MULT * pu_s;

  if(ti_s <= 0.0f)
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_NO_OSCILLATION);
    return;
  }

  ki = kp / ti_s;
  kd = kp * (pu_s / 6.3f);

  standard_curve.kp = kp;
  standard_curve.ki = ki;
  standard_curve.kd = kd;

  pid_state.curves[PID_MODE_STANDARD] = standard_curve;
  pid_generate_boost();

  /* 用高电平 PWM 占空比估算稳态功率，作为无扰切换的初始积分 */
  total_time_ms = pid_at.high_time_accum_ms + pid_at.low_time_accum_ms;

  if(total_time_ms == 0U)
  {
    high_fraction = 0.5f;
  }
  else
  {
    high_fraction = (float)pid_at.high_time_accum_ms / (float)total_time_ms;
  }

  duty_percent = PID_AT_PWM_LOW + ((PID_AT_PWM_HIGH - PID_AT_PWM_LOW) * high_fraction);
  duty_percent = pid_clampf(duty_percent, PID_OUTPUT_MIN, PID_AT_INTEGRAL_INIT_MAX);

  pid_at.result.ku = ku;
  pid_at.result.pu_s = pu_s;
  pid_at.result.amplitude_c = amplitude_c;
  pid_at.result.cycles = pid_at.period_count;
  pid_at.result.standard_curve = standard_curve;
  pid_at.result.boost_curve = pid_state.curves[PID_MODE_BOOST];
  pid_at.result.state = PID_AUTOTUNE_DONE;
  pid_at.result.error = PID_AUTOTUNE_ERR_NONE;

  pid_at.state = PID_AUTOTUNE_DONE;
  pid_at.error = PID_AUTOTUNE_ERR_NONE;
  pid_at.phase = PID_AT_PHASE_IDLE;

  /* 整定完成后默认回到标准曲线 */
  pid_state.mode = PID_MODE_STANDARD;

  /* 无扰切换：初始积分扣除设定温度前馈，避免重复叠加稳态功率 */
  pid_reset_runtime();
  feedforward = pid_clampf(pid_state.setpoint_c * PID_FF_PER_C,
                           0.0f, PID_FF_MAX);
  pid_state.integral = pid_clampf(duty_percent - feedforward,
                                  PID_INTEGRAL_MIN, PID_INTEGRAL_MAX);
  pid_state.output = pid_clampf(feedforward + pid_state.integral,
                                PID_OUTPUT_MIN, PID_OUTPUT_MAX);
  pid_state.prev_temp_c = temperature_c;
  pid_state.last_temp_c = temperature_c;
  pid_state.last_ms = now_ms;
  pid_state.time_initialized = 1U;
}

/**
  * @brief  PWM 双电平整定状态机
  * @param  temperature_c: 当前温度
  * @param  now_ms: 时间戳
  * @retval 当前 PWM 占空比 10% 或 90%
  */
static float pid_autotune_step(float temperature_c, uint32_t now_ms)
{
  float output;

  if(pid_at.started == 0U)
  {
    pid_at.started = 1U;
    pid_at.start_ms = now_ms;
    pid_at.phase_start_ms = now_ms;
    pid_at.phase = PID_AT_PHASE_HIGH;
    pid_at.phase_max_c = temperature_c;
    pid_at.phase_min_c = temperature_c;
    pid_at.last_high_peak_c = temperature_c;
    pid_at.last_low_to_high_ms = 0U;
    pid_at.period_accum_ms = 0U;
    pid_at.high_time_accum_ms = 0U;
    pid_at.low_time_accum_ms = 0U;
    pid_at.period_count = 0U;
    pid_at.duty_started = 0U;
    pid_at.amplitude_accum_c = 0.0f;
    pid_state.output = PID_AT_PWM_HIGH;
    return pid_state.output;
  }

  if((uint32_t)(now_ms - pid_at.start_ms) > PID_AT_TIMEOUT_MS)
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_TIMEOUT);
    return 0.0f;
  }

  if((uint32_t)(now_ms - pid_at.phase_start_ms) > PID_AT_PHASE_TIMEOUT_MS)
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_TIMEOUT);
    return 0.0f;
  }

  if(temperature_c > PID_AT_MAX_TEMP_C)
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_OVERTEMP);
    return 0.0f;
  }

  if(pid_at.phase == PID_AT_PHASE_HIGH)
  {
    if(temperature_c > pid_at.phase_max_c)
    {
      pid_at.phase_max_c = temperature_c;
    }

    if(temperature_c >= (pid_at.setpoint_c + PID_AT_HYSTERESIS_C))
    {
      if(pid_at.duty_started != 0U)
      {
        pid_at.high_time_accum_ms += (uint32_t)(now_ms - pid_at.phase_start_ms);
      }

      pid_at.last_high_peak_c = pid_at.phase_max_c;
      pid_at.phase = PID_AT_PHASE_LOW;
      pid_at.phase_min_c = temperature_c;
      pid_at.phase_start_ms = now_ms;
      output = PID_AT_PWM_LOW;
    }
    else
    {
      output = PID_AT_PWM_HIGH;
    }
  }
  else if(pid_at.phase == PID_AT_PHASE_LOW)
  {
    if(temperature_c < pid_at.phase_min_c)
    {
      pid_at.phase_min_c = temperature_c;
    }

    if(temperature_c <= (pid_at.setpoint_c - PID_AT_HYSTERESIS_C))
    {
      
      if(pid_at.duty_started == 0U)
      {
        pid_at.duty_started = 1U;
        pid_at.high_time_accum_ms = 0U;
        pid_at.low_time_accum_ms = 0U;
      }
      else
      {
        pid_at.low_time_accum_ms += (uint32_t)(now_ms - pid_at.phase_start_ms);
      }

      
      if(pid_at.last_low_to_high_ms != 0U)
      {
        uint32_t period_ms = (uint32_t)(now_ms - pid_at.last_low_to_high_ms);
        float amplitude_c = pid_at.last_high_peak_c - pid_at.phase_min_c;

        if((period_ms > 0U) && (amplitude_c > 0.0f))
        {
          pid_at.period_accum_ms += period_ms;
          pid_at.amplitude_accum_c += amplitude_c;
          pid_at.period_count++;
        }
      }

      pid_at.last_low_to_high_ms = now_ms;
      pid_at.phase = PID_AT_PHASE_HIGH;
      pid_at.phase_max_c = temperature_c;
      pid_at.phase_start_ms = now_ms;

      if(pid_at.period_count >= PID_AT_CYCLES)
      {
        pid_autotune_finish(temperature_c, now_ms);
        return pid_state.output;
      }

      output = PID_AT_PWM_HIGH;
    }
    else
    {
      output = PID_AT_PWM_LOW;
    }
  }
  else
  {
    pid_autotune_fail(PID_AUTOTUNE_ERR_NO_OSCILLATION);
    return 0.0f;
  }

  pid_state.output = output;
  return output;
}

/* ============================ 对外函数 ============================ */

void pid_init(void)
{
  memset(&pid_state, 0, sizeof(pid_state));
  memset(&pid_at, 0, sizeof(pid_at));

  pid_state.setpoint_c = PID_DEFAULT_SETPOINT_C;
  pid_state.mode = PID_MODE_STANDARD;

  pid_state.curves[PID_MODE_STANDARD].kp = PID_DEFAULT_KP;
  pid_state.curves[PID_MODE_STANDARD].ki = PID_DEFAULT_KI;
  pid_state.curves[PID_MODE_STANDARD].kd = PID_DEFAULT_KD;

  pid_state.curves[PID_MODE_CUSTOM] = pid_state.curves[PID_MODE_STANDARD];
  pid_generate_boost();

  for(uint32_t i = 0U; i < PID_CUSTOM_POINTS; i++)
  {
    pid_custom_points[i].temp_c = (float)i * 50.0f;
    pid_custom_points[i].gain = pid_state.curves[PID_MODE_STANDARD];
  }

  pid_at.state = PID_AUTOTUNE_IDLE;
  pid_at.error = PID_AUTOTUNE_ERR_NONE;
  pid_at.phase = PID_AT_PHASE_IDLE;

  pid_reset_runtime();
}

void pid_reset(void)
{
  pid_autotune_cancel();
  pid_reset_runtime();
}

void pid_set_setpoint(float setpoint_c)
{
  float new_setpoint = pid_clampf(setpoint_c, PID_SETPOINT_MIN_C, PID_SETPOINT_MAX_C);

  if(new_setpoint != pid_state.setpoint_c)
  {
    /* 任何设定温度变化都清掉当前 PID 运行状态：
       积分、微分滤波、上次温度、输出、时间基准全部重新开始。 */
    pid_reset_runtime();
    pid_state.setpoint_c = new_setpoint;
  }
}

float pid_get_setpoint(void)
{
  return pid_state.setpoint_c;
}

void pid_set_mode(pid_mode_t mode)
{
  if(mode < PID_MODE_COUNT)
  {
    pid_state.mode = mode;
  }
}

pid_mode_t pid_get_mode(void)
{
  return pid_state.mode;
}

void pid_set_curve(pid_mode_t mode, const pid_curve_t *curve)
{
  uint32_t primask;

  if((mode >= PID_MODE_COUNT) || (pid_curve_is_valid(curve) == 0U))
  {
    return;
  }

  primask = __get_PRIMASK();
  __disable_irq();

  pid_state.curves[mode] = *curve;

  if(mode == PID_MODE_STANDARD)
  {
    pid_generate_boost();
  }

  if(primask == 0U)
  {
    __enable_irq();
  }
}

void pid_get_curve(pid_mode_t mode, pid_curve_t *curve)
{
  uint32_t primask;

  if((mode >= PID_MODE_COUNT) || (curve == NULL))
  {
    return;
  }

  primask = __get_PRIMASK();
  __disable_irq();
  *curve = pid_state.curves[mode];
  if(primask == 0U)
  {
    __enable_irq();
  }
}

void pid_set_custom_curve(const pid_curve_t *curve)
{
  uint32_t primask;

  if(pid_curve_is_valid(curve) == 0U)
  {
    return;
  }

  primask = __get_PRIMASK();
  __disable_irq();

  pid_state.curves[PID_MODE_CUSTOM] = *curve;
  pid_state.mode = PID_MODE_CUSTOM;

  for(uint32_t i = 0U; i < PID_CUSTOM_POINTS; i++)
  {
    pid_custom_points[i].gain = *curve;
  }

  if(primask == 0U)
  {
    __enable_irq();
  }
}

void pid_get_custom_points(pid_custom_point_t *points)
{
  uint32_t primask;

  if(points == NULL)
  {
    return;
  }

  primask = __get_PRIMASK();
  __disable_irq();

  for(uint32_t i = 0U; i < PID_CUSTOM_POINTS; i++)
  {
    points[i] = pid_custom_points[i];
  }

  if(primask == 0U)
  {
    __enable_irq();
  }
}

uint8_t pid_set_custom_points(const pid_custom_point_t *points)
{
  uint32_t primask;

  if(points == NULL)
  {
    return 0U;
  }

  for(uint32_t i = 0U; i < PID_CUSTOM_POINTS; i++)
  {
    if((points[i].temp_c < 0.0f) || (points[i].temp_c > 500.0f) ||
       (pid_curve_is_valid(&points[i].gain) == 0U))
    {
      return 0U;
    }

    if((i > 0U) && (points[i].temp_c <= points[i - 1U].temp_c))
    {
      return 0U;
    }
  }

  primask = __get_PRIMASK();
  __disable_irq();

  for(uint32_t i = 0U; i < PID_CUSTOM_POINTS; i++)
  {
    pid_custom_points[i] = points[i];
  }

  pid_state.curves[PID_MODE_CUSTOM] = points[0].gain;

  if(primask == 0U)
  {
    __enable_irq();
  }

  return 1U;
}

void pid_generate_boost(void)
{
  pid_state.curves[PID_MODE_BOOST].kp =
    pid_state.curves[PID_MODE_STANDARD].kp * PID_BOOST_KP_SCALE;
  pid_state.curves[PID_MODE_BOOST].ki =
    pid_state.curves[PID_MODE_STANDARD].ki * PID_BOOST_KI_SCALE;
  pid_state.curves[PID_MODE_BOOST].kd =
    pid_state.curves[PID_MODE_STANDARD].kd * PID_BOOST_KD_SCALE;
}

/**
  * @brief  普通 PID 计算核心
  * @param  temperature_c: 当前温度
  * @param  dt_s: 控制周期，秒
  * @retval 0~100% 加热功率
  * @note   调用前需已更新 pid_state.last_temp_c；
  *         首次调用只做初始化并返回 0。
  */
static float pid_compute(float temperature_c, float dt_s)
{
  float error;
  float p_term;
  float i_candidate;
  float d_term;
  float feedforward;
  float raw_output;
  float output;
  float output_max;
  pid_curve_t curve;

  if(pid_state.mode >= PID_MODE_COUNT)
  {
    pid_state.mode = PID_MODE_STANDARD;
  }

  if(pid_state.time_initialized == 0U)
  {
    pid_state.time_initialized = 1U;
    pid_state.prev_temp_c = temperature_c;
    pid_state.d_filtered = 0.0f;
    pid_state.output = 0.0f;
    return 0.0f;
  }

  if(dt_s <= 0.0f)
  {
    return pid_state.output;
  }

  if(dt_s > PID_MAX_DT_S)
  {
    dt_s = PID_MAX_DT_S;
  }

  if(pid_state.mode == PID_MODE_CUSTOM)
  {
    curve = pid_custom_interpolate(temperature_c);
  }
  else
  {
    curve = pid_state.curves[pid_state.mode];
  }

  error = pid_state.setpoint_c - temperature_c;
  p_term = curve.kp * error;

  /* 微分作用在测量值上，并做低通滤波，避免设定温度变化产生微分冲击 */
  {
    float derivative = (temperature_c - pid_state.prev_temp_c) / dt_s;
    float alpha = PID_DERIVATIVE_FILTER * (dt_s / PID_DERIVATIVE_REF_DT_S);

    if(alpha > 1.0f)
    {
      alpha = 1.0f;
    }

    pid_state.d_filtered = (alpha * derivative) +
                           ((1.0f - alpha) * pid_state.d_filtered);
  }

  d_term = -curve.kd * pid_state.d_filtered;

  /*
   * 离目标还远时完全放开功率，直接用 100% 升温。
   * 进入目标前 15°C 后才交回 PID，由前馈、积分和微分接管。
   */
  if(error > PID_BULK_BAND_C)
  {
    pid_state.integral = 0.0f;
    pid_state.output = PID_OUTPUT_MAX;
    pid_state.prev_temp_c = temperature_c;
    return PID_OUTPUT_MAX;
  }

  /* 设定温度前馈：只在目标下方给基础功率；越接近目标越小，避免过冲后仍有基础加热 */
  if(error > 0.0f)
  {
    feedforward = pid_clampf(pid_state.setpoint_c * PID_FF_PER_C,
                             0.0f, PID_FF_MAX);

    if(error < PID_FF_TAPER_C)
    {
      feedforward = feedforward * (error / PID_FF_TAPER_C);
    }
  }
  else
  {
    feedforward = 0.0f;
  }

  output_max = PID_OUTPUT_MAX;

  /* 积分分离：远离目标时只保留已有积分，不再累积 */
  i_candidate = pid_state.integral;

  if((error < PID_I_BAND_C) && (error > -PID_I_BAND_C))
  {
    i_candidate += curve.ki * error * dt_s;
  }

  raw_output = p_term + i_candidate + d_term + feedforward;

  /* 抗积分饱和：输出已经顶到当前限幅时，不再往同方向积分 */
  if(((raw_output > output_max) && (error > 0.0f)) ||
     ((raw_output < PID_OUTPUT_MIN) && (error < 0.0f)))
  {
    i_candidate = pid_state.integral;
  }

  pid_state.integral = pid_clampf(i_candidate, PID_INTEGRAL_MIN, PID_INTEGRAL_MAX);

  /* 过冲超过 0.5°C 时清掉正积分，避免残留积分继续维持小功率加热 */
  if((error < -0.5f) && (pid_state.integral > 0.0f))
  {
    pid_state.integral = 0.0f;
  }

  output = p_term + pid_state.integral + d_term + feedforward;

  /*
   * 最后 10°C 增加功率下限，避免微分制动把输出压到很低：
   *   误差 10°C -> 至少 40%
   *   误差  5°C -> 至少 20%
   *   误差  1°C -> 至少 4%
   */
  if((error > 0.0f) && (error < PID_NEAR_BAND_C))
  {
    float near_floor = error * PID_NEAR_FLOOR_PER_C;

    if(output < near_floor)
    {
      output = near_floor;
    }
  }

  pid_state.output = pid_clampf(output, PID_OUTPUT_MIN, output_max);
  pid_state.prev_temp_c = temperature_c;

  return pid_state.output;
}

/**
  * @brief  PID 周期计算（毫秒时间戳版本）
  * @param  temperature_c: 当前温度
  * @param  temperature_valid: 1 有效，0 无效
  * @param  now_ms: 毫秒时间戳
  * @retval 0~100% 加热功率
  * @note   自动整定时返回 PWM 双电平占空比。
  */
float pid_update(float temperature_c, uint8_t temperature_valid, uint32_t now_ms)
{
  float dt_s;

  if(temperature_valid == 0U)
  {
    if(pid_at.state == PID_AUTOTUNE_RUNNING)
    {
      pid_autotune_fail(PID_AUTOTUNE_ERR_TEMP_INVALID);
    }

    pid_reset_runtime();
    return 0.0f;
  }

  pid_state.last_temp_c = temperature_c;

  if(pid_at.state == PID_AUTOTUNE_RUNNING)
  {
    return pid_autotune_step(temperature_c, now_ms);
  }

  if(pid_state.time_initialized == 0U)
  {
    pid_state.last_ms = now_ms;
    return pid_compute(temperature_c, 0.0f);
  }

  if(now_ms == pid_state.last_ms)
  {
    return pid_state.output;
  }

  dt_s = (float)(uint32_t)(now_ms - pid_state.last_ms) / 1000.0f;
  pid_state.last_ms = now_ms;

  return pid_compute(temperature_c, dt_s);
}

/**
  * @brief  PID 固定周期计算（20 kHz 控制环）
  * @param  temperature_c: 当前温度
  * @param  temperature_valid: 1 有效，0 无效
  * @param  dt_s: 控制周期，秒
  * @retval 0~100% 加热功率
  */
float pid_update_fixed(float temperature_c, uint8_t temperature_valid, float dt_s)
{
  if(temperature_valid == 0U)
  {
    if(pid_at.state == PID_AUTOTUNE_RUNNING)
    {
      pid_autotune_fail(PID_AUTOTUNE_ERR_TEMP_INVALID);
    }

    pid_reset_runtime();
    return 0.0f;
  }

  pid_state.last_temp_c = temperature_c;

  /* 自动整定由 1 kHz 慢环推进，此期间不执行普通 PID */
  if(pid_at.state == PID_AUTOTUNE_RUNNING)
  {
    return pid_state.output;
  }

  return pid_compute(temperature_c, dt_s);
}

float pid_get_output(void)
{
  return pid_state.output;
}

void pid_get_status(pid_status_t *status)
{
  if(status == NULL)
  {
    return;
  }

  status->setpoint_c = pid_state.setpoint_c;
  status->temperature_c = pid_state.last_temp_c;
  status->error_c = pid_state.setpoint_c - pid_state.last_temp_c;
  status->output = pid_state.output;
  status->mode = pid_state.mode;
  if(pid_state.mode == PID_MODE_CUSTOM)
  {
    status->curve = pid_custom_interpolate(status->temperature_c);
  }
  else
  {
    status->curve = pid_state.curves[pid_state.mode];
  }
  status->autotune_state = pid_at.state;
  status->autotune_error = pid_at.error;
}

/**
  * @brief  启动 PWM 双电平自动整定
  * @param  setpoint_c: 目标温度，传 0 使用当前设定
  * @retval 1 成功，0 失败
  */
uint8_t pid_autotune_start(float setpoint_c)
{
  if(pid_at.state == PID_AUTOTUNE_RUNNING)
  {
    return 0U;
  }

  if(setpoint_c <= 0.0f)
  {
    setpoint_c = pid_state.setpoint_c;
  }

  if((setpoint_c < PID_AT_MIN_SETPOINT_C) || (setpoint_c > PID_AT_MAX_SETPOINT_C))
  {
    return 0U;
  }

  memset(&pid_at, 0, sizeof(pid_at));

  pid_at.state = PID_AUTOTUNE_RUNNING;
  pid_at.error = PID_AUTOTUNE_ERR_NONE;
  pid_at.phase = PID_AT_PHASE_HIGH;
  pid_at.setpoint_c = setpoint_c;
  pid_at.started = 0U;

  /* 整定目标同时作为之后的控制目标 */
  pid_state.setpoint_c = setpoint_c;

  pid_reset_runtime();
  return 1U;
}

void pid_autotune_cancel(void)
{
  if(pid_at.state == PID_AUTOTUNE_RUNNING)
  {
    pid_at.state = PID_AUTOTUNE_IDLE;
    pid_at.error = PID_AUTOTUNE_ERR_CANCELLED;
    pid_at.phase = PID_AT_PHASE_IDLE;
  }

  pid_reset_runtime();
}

uint8_t pid_is_autotuning(void)
{
  return (pid_at.state == PID_AUTOTUNE_RUNNING) ? 1U : 0U;
}

pid_autotune_state_t pid_autotune_get_state(void)
{
  return pid_at.state;
}

pid_autotune_error_t pid_autotune_get_error(void)
{
  return pid_at.error;
}

void pid_autotune_get_result(pid_autotune_result_t *result)
{
  if(result == NULL)
  {
    return;
  }

  *result = pid_at.result;
  result->state = pid_at.state;
  result->error = pid_at.error;
}

