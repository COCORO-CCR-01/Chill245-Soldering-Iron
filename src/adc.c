#include "at32f421_conf.h"
#include "adc.h"
#include "wk_system.h"
#include <stddef.h>

/* PA0 -> ADC_IN0：T-NTC */
#define ADC_NTC_CHANNEL                 ADC_CHANNEL_0
/* PA1 -> ADC_IN1：T-ADC */
#define ADC_THERMOCOUPLE_CHANNEL        ADC_CHANNEL_1
/* PA2 -> ADC_IN2：V-ADC */
#define ADC_VIN_CHANNEL                 ADC_CHANNEL_2

/* 三路输入均为外部模拟信号；41.5 周期在 15 MHz ADC 时钟下约 2.8 µs，
   三路总转换约 10.8 µs，可放进 20 kHz（50 µs）控制周期。 */
#define ADC_SAMPLE_TIME                 ADC_SAMPLETIME_41_5

/*
 * 上电执行完整校准：ADCALINIT -> ADCAL。
 * 校准等待带超时，避免异常时永久卡住；超时后仍继续后续流程。
 */
#define ADC_CALIBRATION_ENABLE          (1U)

/* 单次转换等待超时（仅用于防止 ADC 异常时死等） */
#define ADC_CONVERSION_TIMEOUT          (10000U)

/**
  * @brief  等待指定的 ADC 标志置位
  * @param  flag: ADC 标志，例如 ADC_CCE_FLAG
  * @retval 1：标志已置位；0：等待超时
  */
static uint8_t adc_wait_flag(uint8_t flag)
{
  uint32_t timeout = ADC_CONVERSION_TIMEOUT;

  while(adc_flag_get(ADC1, flag) == RESET)
  {
    if(--timeout == 0U)
    {
      return 0U;
    }
  }

  return 1U;
}

/**
  * @brief  ADC 初始化
  * @note   PA0/PA1/PA2 配置为模拟输入；ADC1 工作在普通组扫描模式，
  *         转换顺序为 PA0 -> PA1 -> PA2，由软件触发。
  *         ADC 时钟 = APB2(120MHz) / 6 = 20MHz。
  * @param  none
  * @retval none
  */
void adc_init(void)
{
  gpio_init_type gpio_init_struct;
  adc_base_config_type adc_base_struct;
  uint32_t timeout;

  /* 使能 GPIOA 时钟（wk_periph_clock_config() 中通常已使能，这里保证模块独立可用） */
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);

  /* PA0、PA1、PA2 配置为模拟输入 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = GPIO_PINS_0 | GPIO_PINS_1 | GPIO_PINS_2;
  gpio_init_struct.gpio_mode = GPIO_MODE_ANALOG;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init(GPIOA, &gpio_init_struct);

  /* 使能 ADC1 时钟（并直接再置一次 APB2EN，排除库宏/时钟门控异常） */
  crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
  CRM->apb2en |= (1U << 9);

  /* 复位 ADC1 到默认状态，再配置 ADC 时钟分频（与 AT32 官方例程顺序一致） */
  adc_reset(ADC1);
  CRM->apb2rst &= ~(1U << 9);
  wk_delay_us(10U);
  crm_adc_clock_div_set(CRM_ADC_DIV_6);   /* 120 MHz / 6 = 20 MHz，与官方例程一致 */

  /* 普通组单通道模式：每路单独软件触发，避免扫描模式 CCE 只在整组结束置位 */
  adc_base_default_para_init(&adc_base_struct);
  adc_base_struct.sequence_mode = FALSE;
  adc_base_struct.repeat_mode = FALSE;
  adc_base_struct.data_align = ADC_RIGHT_ALIGNMENT;
  adc_base_struct.ordinary_channel_length = 1;
  adc_base_config(ADC1, &adc_base_struct);

  /* 默认 rank1 配 NTC；运行时每次转换前切换通道 */
  adc_ordinary_channel_set(ADC1, ADC_NTC_CHANNEL, 1, ADC_SAMPLE_TIME);

  /* 使能普通组软件触发 */
  adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_SOFTWARE, TRUE);

  /* 使能 ADC1 */
  adc_enable(ADC1, TRUE);

  /* 等待 ADC 上电稳定（tSTAB）后再进行校准 */
  wk_delay_ms(10U);

#if ADC_CALIBRATION_ENABLE
  /* 初始化校准寄存器 */
  adc_calibration_init(ADC1);

  timeout = 0U;
  while(adc_calibration_init_status_get(ADC1) == SET)
  {
    if(++timeout > 2000000U)
    {
      break;
    }
  }

  /* 开始校准 */
  adc_calibration_start(ADC1);

  timeout = 0U;
  while(adc_calibration_status_get(ADC1) == SET)
  {
    if(++timeout > 2000000U)
    {
      break;
    }
  }
#else
  (void)timeout;
#endif
}

/**
  * @brief  读取 PA0、PA1、PA2 的 ADC 原始值
  * @param  raw: 输出数据指针
  * @retval none
  * @note   每次调用触发一轮扫描，按转换顺序读取 ODT；
  *         读取 ODT 会同时清除 ADC_CCE_FLAG。
  */
void adc_read_all(adc_raw_data_t *raw)
{
  uint16_t ntc_raw;
  uint16_t thermocouple_raw;
  uint16_t vin_raw;

  if(raw == NULL)
  {
    return;
  }

  /* 本次采样先标记为无效，只有三路都成功才更新数据 */
  raw->valid = 0U;

  /* 1. PA0 / NTC */
  adc_flag_clear(ADC1, ADC_CCE_FLAG);
  adc_ordinary_channel_set(ADC1, ADC_NTC_CHANNEL, 1, ADC_SAMPLE_TIME);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  if(adc_wait_flag(ADC_CCE_FLAG) == 0U)
  {
    return;
  }
  ntc_raw = adc_ordinary_conversion_data_get(ADC1);

  /* 2. PA1 / 热电偶 */
  adc_flag_clear(ADC1, ADC_CCE_FLAG);
  adc_ordinary_channel_set(ADC1, ADC_THERMOCOUPLE_CHANNEL, 1, ADC_SAMPLE_TIME);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  if(adc_wait_flag(ADC_CCE_FLAG) == 0U)
  {
    return;
  }
  thermocouple_raw = adc_ordinary_conversion_data_get(ADC1);

  /* 3. PA2 / VIN */
  adc_flag_clear(ADC1, ADC_CCE_FLAG);
  adc_ordinary_channel_set(ADC1, ADC_VIN_CHANNEL, 1, ADC_SAMPLE_TIME);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  if(adc_wait_flag(ADC_CCE_FLAG) == 0U)
  {
    return;
  }
  vin_raw = adc_ordinary_conversion_data_get(ADC1);

  raw->ntc = ntc_raw;
  raw->thermocouple = thermocouple_raw;
  raw->vin = vin_raw;
  raw->valid = 1U;
}
