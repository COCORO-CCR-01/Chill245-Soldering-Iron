/**
  **************************************************************************
  * @file     powermanagement.c
  * @brief    输入电压、震动休眠、电源保护和 CH224A 电压诱骗
  **************************************************************************
  * 硬件：
  *   PA2  -> V-ADC，V-ADC = VCC * 1/(10+1)，VCC = V-ADC * 11
  *   PB1  -> 震动开关 SLEEP，3V3 -> 10k -> PB1 -> 开关 -> GND
  *   PB6  -> I2C1_SCL -> CH224A
  *   PB7  -> I2C1_SDA -> CH224A
  *
  * 休眠流程：
  *   ACTIVE --静止超时--> REDUCED --继续静止超时--> HEATER_OFF
  *          --继续静止超时--> BACKLIGHT_OFF
  *
  * REDUCED / HEATER_OFF 或手动 DISABLED 状态检测到震动可唤醒；BACKLIGHT_OFF 只能按键唤醒。
  * *************************************************************************
  */

#include "powermanagement.h"
#include "adc.h"
#include "at32f421_conf.h"
#include "buttons.h"
#include "display.h"
#include "eeprom.h"
#include "statemachine.h"
#include "wk_system.h"
#include <stddef.h>

#define POWER_VOLTAGE_SAMPLE_MS         (10U)
#define POWER_VIBRATION_ACTIVITY_MS     (100U)
#define POWER_MANUAL_SLEEP_WAKE_BLOCK_MS (30000U)
#define POWER_LOW_POWER_DRAIN           (3U)
#define POWER_LOW_POWER_DEFAULT         (13U)
#define POWER_I2C_TIMEOUT               (100000U)
#define POWER_MIN_TIMEOUT_MS            (1000U)

/* CH224A 协商时序（全部来自板上实测，不是 datasheet 推测）*/
#define POWER_CH224A_SETTLE_MS          (1200U)   /* 实测 400 ms 内完成，留 3 倍余量 */
#define POWER_CH224A_POLL_MS            (50U)
#define POWER_CH224A_COMMIT_MS          (300U)
#define POWER_CH224A_ATTEMPTS           (2U)
/* 实测 20V=19.86~19.92V / 15V=14.89~14.95V / 9V=8.90~8.97V，
* ±12% 既能容下测量误差，又能一眼看出根本没切档 */
#define POWER_CH224A_TOLERANCE_PCT      (12U)
/* 周期纠偏：VBUS 偏离目标超过容差且持续这么久才动手，避免切换过程误触发。
* 开机第一次把 9V 谈到 20V 也走这条路径，所以不要太长。*/
#define POWER_PD_FAULT_MS               (1500U)
#define POWER_PD_STEP_MS                (350U)
#define POWER_PD_BACKOFF_MS             (15000U)
#define POWER_PD_MAX_ATTEMPTS           (2U)

static power_state_t power_state;
static power_fault_t power_fault;
static power_fault_callback_t power_fault_callback;

static uint32_t power_stationary_timeout_ms;
static uint8_t power_low_power_threshold;
static float power_reduced_temp_c;
static uint32_t power_reduced_off_timeout_ms;
static uint32_t power_backlight_off_timeout_ms;

static uint8_t power_max_duty_pct = 100U;
static uint16_t power_low_voltage_mv;
static uint16_t power_high_voltage_mv;
static uint16_t power_voltage_hysteresis_mv;
static uint32_t power_voltage_trip_ms;
static uint32_t power_voltage_recovery_ms;

static uint16_t power_voltage_mv;
static uint32_t power_last_voltage_sample_ms;
static uint32_t power_voltage_bad_since_ms;
static uint32_t power_voltage_good_since_ms;

static uint32_t power_last_motion_ms;
static uint32_t power_low_power_accum_ms;
static uint32_t power_low_power_last_ms;
static uint32_t power_vibration_latch_ms;
static uint32_t power_vibration_wake_block_until_ms;
static uint32_t power_vibration_raw_change_ms;
static uint32_t power_state_enter_ms;
static uint8_t power_vibration_raw_level;
static uint8_t power_vibration_stable_level;
static uint8_t power_sleep_heater_was_enabled;

static float power_saved_setpoint_c;
static uint8_t power_saved_setpoint_valid;

static uint8_t power_fault_heater_was_enabled;
static uint8_t power_wake_pending;
static uint8_t power_initialized;

static uint8_t power_ch224a_found;
static uint8_t power_ch224a_address;
static float power_ch224a_voltage_v;
static uint8_t power_ch224a_status;
static uint8_t power_ch224a_target_index = POWER_CH224A_INDEX_20V;

/* PD 档位保持状态机：非阻塞，每个 power_update tick 只走一小步，每步只是一次
* 三字节 I2C 事务（几十 us），不会卡住显示和加热。*/
typedef enum
{
  POWER_PD_MONITOR = 0,   /* 正常监控 */
  POWER_PD_COMMIT,        /* 已回写 commit 档，等合同重新确认 */
  POWER_PD_REQUEST,       /* 已重写目标档，等 VBUS 到位 */
  POWER_PD_GIVE_UP        /* 本轮不再尝试，隔一段时间再来看 */
} power_pd_action_t;

static power_pd_action_t power_pd_action;
static uint32_t power_pd_bad_since_ms;
static uint32_t power_pd_step_ms;
static uint8_t power_pd_attempts;

/* AUTO 档：解析能力表有代价（48×2 次单字节读），不能每拍都做；没解析出来时每隔
 * POWER_PD_AUTO_RETRY_MS 再试一次，试满 POWER_PD_AUTO_MAX_TRIES 次就按当前上限要档。 */
#define POWER_PD_AUTO_RETRY_MS          (1000U)
#define POWER_PD_AUTO_MAX_TRIES         (3U)

/* VBUS 掉到这个值以下：连 5V 默认档都撑不住，是供电方被我们拉得重启了（过流保护
 * 或者软启动失败），不用等 1.5 s 容错计时，立刻降档。 */
#define POWER_PD_COLLAPSE_MV            (4500U)

static uint8_t power_pd_auto_index = POWER_PD_INDEX_NONE;  /* AUTO 当前要的档位 */
static uint8_t power_pd_auto_cap = POWER_CH224A_INDEX_20V; /* 允许的最高档，失败就压 */
static uint8_t power_pd_auto_tries;
static uint32_t power_pd_auto_step_ms;

static uint8_t power_read_vibration_level(void);

/* ---------------------------------------------------------------------------
 * CH224A 调试打印。默认关闭，关闭时这些代码不会进固件。
 * 打开后从 H2 串口（USART1 115200 8N1，和刷机 / logo 上传同一个口）打印 PD
 * 状态变化，插上 PD 头就能直接看到“探测 -> commit -> 请求 -> 校验”的过程。
 * 注意：logo 上传走的是同一个串口，调试期间别同时上传 logo。 */
#define POWER_CH224A_DEBUG              (0U)

#if POWER_CH224A_DEBUG
/* 上一次打出来的输入电压，用来判断"跳了一档没有" */
static uint16_t power_dbg_last_mv;
/* 上一条跳变打印的时间，限死最少间隔，别拿串口当示波器刷 */
static uint32_t power_dbg_last_print_ms;
/* 配置来源只打一次 */
static uint8_t power_dbg_cfg_done;

static void power_dbg_char(char c)
{
  uint32_t guard = 200000U;

  /* 有界等待：USART1 时钟没开的时候标志位永远读不到，不能死等 */
  while((usart_flag_get(USART1, USART_TDBE_FLAG) == RESET) && (guard != 0U))
  {
    guard--;
  }

  usart_data_transmit(USART1, (uint16_t)(uint8_t)c);
}

static void power_dbg_str(const char *text)
{
  while(*text != '\0')
  {
    power_dbg_char(*text);
    text++;
  }
}

static void power_dbg_hex8(uint8_t value)
{
  static const char digits[] = "0123456789ABCDEF";

  power_dbg_char(digits[(value >> 4) & 0x0FU]);
  power_dbg_char(digits[value & 0x0FU]);
}

static void power_dbg_dec(uint32_t value)
{
  char buf[10];
  uint8_t n = 0U;

  do
  {
    buf[n] = (char)('0' + (uint8_t)(value % 10U));
    n++;
    value /= 10U;
  } while((value != 0U) && (n < sizeof(buf)));

  while(n != 0U)
  {
    n--;
    power_dbg_char(buf[n]);
  }
}

/* power_init 比 bootlogo 的串口初始化还早，调试打印要自己把 USART1 带起来。
 * 参数和 bootlogo_uart_init() 完全一致（PA9/PA10 MUX_1、115200 8N1），所以
 * 后面 bootlogo 再初始化一次也是同样的配置，两边不冲突。 */
static void power_dbg_init(void)
{
  gpio_init_type gpio_init_struct;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = GPIO_PINS_9 | GPIO_PINS_10;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(GPIOA, &gpio_init_struct);

  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE9, GPIO_MUX_1);
  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE10, GPIO_MUX_1);

  usart_init(USART1, 115200U, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_transmitter_enable(USART1, TRUE);
  usart_receiver_enable(USART1, TRUE);
  usart_enable(USART1, TRUE);
}

static uint32_t power_dbg_hb_ms;

#define POWER_DBG(text)                 power_dbg_str((text))
#define POWER_DBG_HEX(value)            power_dbg_hex8((uint8_t)(value))
#define POWER_DBG_DEC(value)            power_dbg_dec((uint32_t)(value))
#define POWER_DBG_INIT()                power_dbg_init()
#else
#define POWER_DBG(text)                 do {} while(0)
#define POWER_DBG_HEX(value)            do {} while(0)
#define POWER_DBG_DEC(value)            do {} while(0)
#define POWER_DBG_INIT()                do {} while(0)
#endif

static const float power_ch224a_voltage_table[6] =
{
  5.0f, 9.0f, 12.0f, 15.0f, 20.0f, 28.0f
};

/* 整数档位表：比较 VBUS 用不上浮点，省掉 __aeabi_fmul/__aeabi_f2uiz */
static const uint16_t power_ch224a_voltage_mv_table[6] =
{
  5000U, 9000U, 12000U, 15000U, 20000U, 28000U
};

/* ------------------------------------------------------------------------- */
/* CH224A I2C                                                                 */
/* ------------------------------------------------------------------------- */

static uint8_t power_i2c_wait_flag(uint32_t flag, uint32_t timeout)
{
  while(i2c_flag_get(I2C1, flag) == RESET)
  {
    if(i2c_flag_get(I2C1, I2C_ACKFAIL_FLAG) != RESET)
    {
      i2c_stop_generate(I2C1);
      i2c_flag_clear(I2C1, I2C_ACKFAIL_FLAG);
      wk_delay_us(10U);
      return 0U;
    }

    if(timeout == 0U)
    {
      i2c_stop_generate(I2C1);
      wk_delay_us(10U);
      return 0U;
    }

    timeout--;
  }

  return 1U;
}

static uint8_t power_ch224a_read_reg_addr(uint8_t address, uint8_t reg, uint8_t *value);
static uint16_t power_measure_vin_mv(void);

/* ---------------------------------------------------------------------------
 * 总线恢复 + I2C1 硬复位。两种卡死都要处理：
 *   1) 从机在掉电 / 热插拔过程中把 SDA 卡在低电平 -> I2C1 永远 BUSY，只有外部
 *      9 个时钟 + STOP 能让它把那一位释放掉；
 *   2) I2C1 自己停在半笔事务上（实测签名：上电第一笔能读到 0x09=0x48，之后每笔
 *      在地址阶段超时、sts1=0040、SCL/SDA 仍是空闲高电平）-> 复位外设。
 * 顺序：先用 GPIO 手打时钟把从机放开，再复位重新初始化 I2C1。 */
static void power_i2c_recover_bus(void)
{
  gpio_init_type gpio_init_struct;
  uint8_t i;

  i2c_enable(I2C1, FALSE);

  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE6, GPIO_MUX_0);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE7, GPIO_MUX_0);

  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = GPIO_PINS_6 | GPIO_PINS_7;
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init(GPIOB, &gpio_init_struct);

  gpio_bits_set(GPIOB, GPIO_PINS_6 | GPIO_PINS_7);
  wk_delay_us(10U);

  for(i = 0U; i < 9U; i++)
  {
    gpio_bits_reset(GPIOB, GPIO_PINS_6);
    wk_delay_us(10U);
    gpio_bits_set(GPIOB, GPIO_PINS_6);
    wk_delay_us(10U);
  }

  /* STOP：SCL 为高期间 SDA 由低跳高 */
  gpio_bits_reset(GPIOB, GPIO_PINS_7);
  wk_delay_us(10U);
  gpio_bits_set(GPIOB, GPIO_PINS_6);
  wk_delay_us(10U);
  gpio_bits_set(GPIOB, GPIO_PINS_7);
  wk_delay_us(10U);

  /* 关键：bit-bang 结束之后必须把引脚重新配回 GPIO_MODE_MUX，只改 mux 寄存器是
   * 不够的 —— 模式还留在 GPIO_MODE_OUTPUT 的话，引脚是普通输出脚，I2C1 外设和
   * 引脚之间是断开的，之后每一笔传输都发不出去（实测 found=0 就是这么来的）。
   * 参数和 power_init 里的初始配置保持一致。 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = GPIO_PINS_6 | GPIO_PINS_7;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOB, &gpio_init_struct);

  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE6, GPIO_MUX_1);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE7, GPIO_MUX_1);

  crm_periph_reset(CRM_I2C1_PERIPH_RESET, TRUE);
  crm_periph_reset(CRM_I2C1_PERIPH_RESET, FALSE);
  crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, TRUE);
  i2c_init(I2C1, I2C_FSMODE_DUTY_2_1, 100000);
  i2c_enable(I2C1, TRUE);
  wk_delay_ms(2U);
}

/* AT32F421 的 I2C 在一笔事务结束后会在数据寄存器里留下一个没被读走的字节
 * (RDBF)，状态机就停在那里，下一次 START 根本不会上线：既不报 NACK 也不报仲裁
 * 丢失，状态寄存器里只有 0x0040，SCL/SDA 看着还是空闲高电平，非常难查。
 * 板上实测签名：上电第一笔能正常读到 0x09 = 0x48，之后每一笔都在地址阶段超时。 */
static uint8_t power_i2c_begin(void)
{
  uint32_t timeout;

  if(power_initialized == 0U)
  {
    return 0U;
  }

  if(i2c_flag_get(I2C1, I2C_RDBF_FLAG) != RESET)
  {
    (void)i2c_data_receive(I2C1);
  }

  if(i2c_flag_get(I2C1, I2C_ACKFAIL_FLAG) != RESET)
  {
    i2c_flag_clear(I2C1, I2C_ACKFAIL_FLAG);
  }

  timeout = POWER_I2C_TIMEOUT;

  while(i2c_flag_get(I2C1, I2C_BUSYF_FLAG) != RESET)
  {
    if(timeout == 0U)
    {
      power_i2c_recover_bus();
      return 0U;
    }

    timeout--;
  }

  return 1U;
}

/* 事务收尾。上面那种锁死只在“成功事务之后”出现，实测只有重新使能外设才能
* 把状态机拉回干净状态；CH224A 每秒最多被访问几次，代价可以忽略。*/
static void power_i2c_end(void)
{
  i2c_stop_generate(I2C1);
  wk_delay_us(10U);

  i2c_enable(I2C1, FALSE);
  wk_delay_us(20U);
  i2c_enable(I2C1, TRUE);
  wk_delay_us(10U);
}

static uint8_t power_ch224a_write_reg(uint8_t address, uint8_t reg, uint8_t value)
{
  uint8_t attempt;

  for(attempt = 0U; attempt < 2U; attempt++)
  {
    if(power_i2c_begin() == 0U)
    {
      continue;
    }

    i2c_start_generate(I2C1);

    if(power_i2c_wait_flag(I2C_STARTF_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    /* 传预移位地址：见 powermanagement.h 里 POWER_CH224A_I2C_ADDR_* 的说明 */
    i2c_7bit_address_send(I2C1, address, I2C_DIRECTION_TRANSMIT);

    if(power_i2c_wait_flag(I2C_ADDR7F_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    /* 必须释放 ADDR（先读 STS1 再读 STS2 那个序列）。不释放的话外设会自己
     * 把 SCL 钳在低电平，后面所有传输全部超时，而且不报任何错误。 */
    i2c_flag_clear(I2C1, I2C_ADDR7F_FLAG);

    if(power_i2c_wait_flag(I2C_TDBE_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    i2c_data_send(I2C1, reg);

    if(power_i2c_wait_flag(I2C_TDC_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    if(power_i2c_wait_flag(I2C_TDBE_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    i2c_data_send(I2C1, value);

    if(power_i2c_wait_flag(I2C_TDC_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    power_i2c_end();
    return 1U;
  }

  return 0U;
}

/* 读一个寄存器。两条实测结论：
 * 1) 单字节读的 NACK 必须在释放 ADDR 之前就关好，否则从机会多被时钟一个字节，
 * 传输永远等不到结束，读回来的是 0；
 * 2) CH224A 不支持一次多字节的流式读：RESTART 之后第一个字节是有效数据，之后
 * 继续时钟读到的全部是 0xFF。所以读 N 个寄存器就要调用 N 次本函数。 */
static uint8_t power_ch224a_read_reg_addr(uint8_t address, uint8_t reg, uint8_t *value)
{
  uint8_t attempt;

  for(attempt = 0U; attempt < 2U; attempt++)
  {
    if(power_i2c_begin() == 0U)
    {
      continue;
    }

    /* 第一段：写寄存器指针 */
    i2c_start_generate(I2C1);

    if(power_i2c_wait_flag(I2C_STARTF_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    i2c_7bit_address_send(I2C1, address, I2C_DIRECTION_TRANSMIT);

    if(power_i2c_wait_flag(I2C_ADDR7F_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    i2c_flag_clear(I2C1, I2C_ADDR7F_FLAG);

    if(power_i2c_wait_flag(I2C_TDBE_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    i2c_data_send(I2C1, reg);

    if(power_i2c_wait_flag(I2C_TDC_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    /* 绗簩娈碉細RESTART 杞 */
    i2c_start_generate(I2C1);

    if(power_i2c_wait_flag(I2C_STARTF_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      power_i2c_end();
      continue;
    }

    i2c_7bit_address_send(I2C1, address, I2C_DIRECTION_RECEIVE);

    if(power_i2c_wait_flag(I2C_ADDR7F_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      i2c_ack_enable(I2C1, TRUE);
      power_i2c_end();
      continue;
    }

    i2c_ack_enable(I2C1, FALSE);         /* 先回 NACK，再清 ADDR */
    i2c_flag_clear(I2C1, I2C_ADDR7F_FLAG);
    i2c_stop_generate(I2C1);             /* 单字节读：STOP 提前挂上 */

    if(power_i2c_wait_flag(I2C_RDBF_FLAG, POWER_I2C_TIMEOUT) == 0U)
    {
      i2c_ack_enable(I2C1, TRUE);
      power_i2c_end();
      continue;
    }

    *value = (uint8_t)i2c_data_receive(I2C1);

    i2c_ack_enable(I2C1, TRUE);
    power_i2c_end();
    return 1U;
  }

  return 0U;
}

/* 协商期间 power_update 不在跑，state machine 缓存的 vin_raw 是旧的，
 * 所以这里直接阻塞采一次 PA2。 */
static uint16_t power_measure_vin_mv(void)
{
  adc_raw_data_t raw;

  adc_read_all(&raw);

  if(raw.valid == 0U)
  {
    return power_voltage_mv;
  }

  return power_vin_raw_to_mv(raw.vin);
}

/* 判断实测 VBUS 是不是已经落在目标档的容差窗口内。
 * 用 power_voltage_mv 缓存（power_voltage_task 每 10 ms 刷新一次；阻塞协商时
 * power_ch224a_negotiate_index 自己刷新），这样周期纠偏不会去碰 ADC。 */
static uint8_t power_vin_matches_index(uint8_t index)
{
  uint32_t target_mv;
  uint32_t window_mv;
  uint32_t vbus;

  if(index > 5U)
  {
    return 0U;
  }

  target_mv = (uint32_t)power_ch224a_voltage_mv_table[index];
  window_mv = (target_mv * POWER_CH224A_TOLERANCE_PCT) / 100U;
  vbus = (uint32_t)power_voltage_mv;

  if((vbus + window_mv >= target_mv) &&
     (vbus <= target_mv + window_mv))
  {
    return 1U;
  }

  return 0U;
}

/* 探测改用“读 0x09”。原来用“写 20V”当探测有两个毛病：上电之后的第一次
* 0x0A 写，CH224A 会直接吞掉（见 power_ch224a_negotiate_index），探测报成功
* 却没有任何协商效果；而且一旦失败，连芯片到底在不在线都读不出来。*/
static uint8_t power_ch224a_find(void)
{
  uint8_t status;

  status = 0U;

  if(power_ch224a_read_reg_addr(POWER_CH224A_I2C_ADDR_PRIMARY,
                                POWER_CH224A_REG_STATUS, &status) != 0U)
  {
    power_ch224a_address = POWER_CH224A_I2C_ADDR_PRIMARY;
    power_ch224a_status = status;
    power_ch224a_found = 1U;
    return 1U;
  }

  if(power_ch224a_read_reg_addr(POWER_CH224A_I2C_ADDR_SECONDARY,
                                POWER_CH224A_REG_STATUS, &status) != 0U)
  {
    power_ch224a_address = POWER_CH224A_I2C_ADDR_SECONDARY;
    power_ch224a_status = status;
    power_ch224a_found = 1U;
    return 1U;
  }

  power_ch224a_found = 0U;
  return 0U;
}

/* 浮点电压 -> 档位索引。CH224A 是固定档位，没有 24V，24V 请求落到 20V 档。 */
static uint8_t power_ch224a_index_of(float volts)
{
  if(volts <= 5.0f)
  {
    return POWER_CH224A_INDEX_5V;
  }

  if(volts <= 9.0f)
  {
    return POWER_CH224A_INDEX_9V;
  }

  if(volts <= 12.0f)
  {
    return POWER_CH224A_INDEX_12V;
  }

  if(volts <= 15.0f)
  {
    return POWER_CH224A_INDEX_15V;
  }

  if(volts <= 20.0f)
  {
    return POWER_CH224A_INDEX_20V;
  }

  if(volts <= 28.0f)
  {
    return POWER_CH224A_INDEX_28V;
  }

  return 0xFFU;
}

uint8_t power_ch224a_negotiate_index(uint8_t index)
{
  uint8_t attempt;
  uint8_t poll;
  uint8_t status;

  if(index > 5U)
  {
    return 0U;
  }

  power_ch224a_target_index = index;

  if(power_ch224a_found == 0U)
  {
    if(power_ch224a_find() == 0U)
    {
      return 0U;
    }
  }

  for(attempt = 0U; attempt < POWER_CH224A_ATTEMPTS; attempt++)
  {
    if(power_ch224a_write_reg(power_ch224a_address,
                              POWER_CH224A_REG_VOLTAGE, index) == 0U)
    {
      power_ch224a_found = 0U;
      return 0U;
    }

    for(poll = 0U; poll < (POWER_CH224A_SETTLE_MS / POWER_CH224A_POLL_MS); poll++)
    {
      wk_delay_ms(POWER_CH224A_POLL_MS);

      status = 0U;
      (void)power_ch224a_read_reg_addr(power_ch224a_address,
                                       POWER_CH224A_REG_STATUS, &status);
      power_ch224a_status = status;

      power_voltage_mv = power_measure_vin_mv();

      if(((status & POWER_CH224A_STATUS_PD) != 0U) &&
         (power_vin_matches_index(index) != 0U))
      {
        power_ch224a_voltage_v = power_ch224a_voltage_table[index];
        power_pd_action = POWER_PD_MONITOR;
        power_pd_bad_since_ms = 0U;
        power_pd_attempts = 0U;
        POWER_DBG("\r\n[pd] OK idx=");
        POWER_DBG_DEC(index);
        POWER_DBG(" st=0x");
        POWER_DBG_HEX(status);
        POWER_DBG(" mv=");
        POWER_DBG_DEC(power_voltage_mv);
        POWER_DBG("\r\n");
        return 1U;
      }
    }

    /* 没到位。实测最常见的原因是合同还停在 0x48 没有被确认，此时写 0x0A 会被
     * 忽略：先回写一档把合同 commit（9V 是 CFG1=205k 单电阻模式的默认档，
     * 来回切换最安全），下一轮再打真正的目标档。*/
    POWER_DBG("\r\n[pd] try");
    POWER_DBG_DEC(attempt);
    POWER_DBG(" idx=");
    POWER_DBG_DEC(index);
    POWER_DBG(" st=0x");
    POWER_DBG_HEX(power_ch224a_status);
    POWER_DBG(" mv=");
    POWER_DBG_DEC(power_voltage_mv);
    POWER_DBG(" -> commit\r\n");
    (void)power_ch224a_write_reg(power_ch224a_address,
                                 POWER_CH224A_REG_VOLTAGE,
                                 POWER_CH224A_INDEX_COMMIT);
    wk_delay_ms(POWER_CH224A_COMMIT_MS);
  }

  /* 到这里说明这个供电方没有这一档，或者根本不是 PD 源。不要继续重试：
   * 反复打断 PD 协商只会让输入来回抖，交给欠压保护和 UI 去提示用户换头。*/
  power_voltage_mv = power_measure_vin_mv();
  power_ch224a_voltage_v = (float)power_voltage_mv / 1000.0f;

  POWER_DBG("\r\n[pd] FAIL idx=");
  POWER_DBG_DEC(index);
  POWER_DBG(" st=0x");
  POWER_DBG_HEX(power_ch224a_status);
  POWER_DBG(" mv=");
  POWER_DBG_DEC(power_voltage_mv);
  POWER_DBG("\r\n");

  return 0U;
}

uint8_t power_ch224a_negotiate(float volts)
{
  uint8_t index;

  index = power_ch224a_index_of(volts);

  if(index == 0xFFU)
  {
    return 0U;
  }

  return power_ch224a_negotiate_index(index);
}

/* PD 档位保持：实测档位会中途掉（例如 20V 被供电方退回 9V），输入电压和已请求
 * 档位持续不一致时，重走一遍“commit 合同 -> 重写目标档 -> 校验 VBUS”。
 * 
 * 非阻塞：每个 tick 最多做一件事，正常情况只是比较一次电压。
 * 5V 档不纠偏：CFG1=205k 单电阻模式没有 5V 配置，写 0 芯片会保持默认档，
 * 纠偏只会白白打断合同。 */
/* 适配器 Source_Capabilities 读取与解码。AUTO 档要靠它挑出这颗供电能给的最高档，
 * 所以生产固件里也要编译，不再是调试打印专用。
 *
 * 0x60~0x8F 官方手册只写了两句话：只读、装的是适配器发来的 SRCCAP（EPR 模式下是
 * EPR_SRCCAP）原始数据，内部怎么排布官方没写。社区三份独立实现（M5Stack-CH224x-PPS、
 * 0wQ/ch224q、CH32V307 例程）加 PowerZ 抓包逐字节对照的结论一致：
 *   0x60/0x61 = PD 消息头，16 位小端，B4:0 消息类型（1 = Source_Capabilities），
 *               B14:12 = PDO 个数；
 *   0x62 起   = PDO0..PDO6，每个 32 位小端；再往后是 CRC32，其余寄存器无定义。
 * PDO 位域按 USB PD 规范：Fixed Supply 是 B31:30=00、B19:10=电压(50mV 单位)、
 * B9:0=最大电流(10mA) —— 是 B19:10，不是 Battery PDO 的 B29:20，搞混了会把 5V
 * 解成 8V。
 *
 * CH224A 不支持连续读，只能一个寄存器一个寄存器读；读的过程中 PD 引擎还可能刷新
 * 这个缓冲，所以整块读两遍，两遍一致的字节才当可信，不一致的抹零并计数报出来。 */
static uint8_t power_ch224a_read_caps(uint8_t *data, uint8_t *out_ndo, uint8_t *bad_out)
{
  uint8_t again;
  uint8_t bad;
  uint8_t len;
  uint8_t ndo;
  uint8_t i;
  uint16_t head;

  /* 先只读消息头拿 PDO 个数。CH224A 不支持连续读，48 个寄存器读两遍是 96 次单字节
   * 事务，实测会把 I2C1 逼进锁死（读完之后芯片就再也不 ACK，只能靠总线恢复起来）；
   * 头 + PDO 最多 30 字节，读两遍 62 次，够用又不折腾总线。 */
  if((power_ch224a_read_reg(POWER_CH224A_REG_CAP_BASE, &data[0]) == 0U)
     || (power_ch224a_read_reg((uint8_t)(POWER_CH224A_REG_CAP_BASE + 1U), &data[1]) == 0U))
  {
    return 0U;
  }

  head = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));

  /* 消息类型必须是 1 = Source_Capabilities，否则这块缓冲现在装的不是能力表
   * （刚拔插、或者 PD 引擎正在写别的报文），当读失败处理，别拿脏数据去挑档。 */
  if((head & 0x1FU) != 0x01U)
  {
    return 0U;
  }

  ndo = (uint8_t)((head >> 12) & 0x07U);

  if(ndo == 0U)
  {
    return 0U;
  }

  len = (uint8_t)(2U + (ndo * 4U));

  if(len > POWER_CH224A_CAP_COUNT)
  {
    len = POWER_CH224A_CAP_COUNT;
  }

  for(i = 2U; i < len; i++)
  {
    if(power_ch224a_read_reg((uint8_t)(POWER_CH224A_REG_CAP_BASE + i), &data[i]) == 0U)
    {
      return 0U;
    }
  }

  /* 整段再读一遍对结果，两遍一致的字节才可信，不一致的抹零并计数。中途读失败就别
   * 继续硬撑了 —— 继续读只会把已经发飘的总线按死，整块当读失败，让调用方重来。 */
  bad = 0U;

  for(i = 0U; i < len; i++)
  {
    again = 0U;

    if(power_ch224a_read_reg((uint8_t)(POWER_CH224A_REG_CAP_BASE + i), &again) == 0U)
    {
      return 0U;
    }

    if(again != data[i])
    {
      data[i] = 0U;
      bad++;
    }
  }

  if(bad_out != NULL)
  {
    *bad_out = bad;
  }

  *out_ndo = ndo;

  return 1U;
}


/* 这颗供电方能给的最高 Fixed 档，换算成 CH224A 电压索引；读不到或者认不出档，
 * 返回 POWER_PD_INDEX_NONE 让调用方去走退路。
 * 只认 Fixed Supply（B31:30=00）：PPS / 电池档整机用不上，0x0A 那个索引寄存器本来
 * 也只对应固定档。 */
static uint8_t power_ch224a_max_index_from_caps(void)
{
  /* 和下标就是 POWER_CH224A_INDEX_5V..20V 对齐的标称电压 */
  static const uint16_t nominal[5] = {5000U, 9000U, 12000U, 15000U, 20000U};
  uint8_t data[POWER_CH224A_CAP_COUNT];
  uint8_t ndo;
  uint8_t best;
  uint8_t i;
  uint8_t k;

  if(power_ch224a_read_caps(data, &ndo, NULL) == 0U)
  {
    return POWER_PD_INDEX_NONE;
  }

  best = POWER_PD_INDEX_NONE;

  for(i = 0U; i < ndo; i++)
  {
    uint32_t pdo;
    uint16_t mv;

    pdo = (uint32_t)data[2U + (i * 4U)]
        | ((uint32_t)data[3U + (i * 4U)] << 8)
        | ((uint32_t)data[4U + (i * 4U)] << 16)
        | ((uint32_t)data[5U + (i * 4U)] << 24);

    if(((pdo >> 30) & 0x03U) != 0x00U)
    {
      continue;
    }

    mv = (uint16_t)(((pdo >> 10) & 0x3FFU) * 50U);

    for(k = 0U; k < 5U; k++)
    {
      /* 放 ±0.5V 余量：有的头把 20V 标成 19.9V 或 20.1V，档还是那一档 */
      if((mv + 500U >= nominal[k]) && (mv <= (uint16_t)(nominal[k] + 500U))
         && ((best == POWER_PD_INDEX_NONE) || (k > best)))
      {
        best = k;
      }
    }
  }

  return best;
}


#if POWER_CH224A_DEBUG
/* 一次 attach 只 dump 一次：能力表在没拔插之前不会变，反复 dump 只是反复 hammer
 * 总线（实测读得越多越容易把 I2C1 读锁死）。重新发现芯片时再放开一次。 */
static uint8_t power_pd_caps_dumped;
#define power_pd_caps_dumped_reset()    (power_pd_caps_dumped = 0U)

static void power_pd_dbg_hex32(uint32_t value)
{
  POWER_DBG_HEX((uint8_t)(value >> 24));
  POWER_DBG_HEX((uint8_t)(value >> 16));
  POWER_DBG_HEX((uint8_t)(value >> 8));
  POWER_DBG_HEX((uint8_t)value);
}


static void power_pd_dump_caps(void)
{
  uint8_t data[POWER_CH224A_CAP_COUNT];
  uint8_t bad;
  uint8_t i;
  uint16_t head;
  uint8_t ndo;

  if(power_pd_caps_dumped != 0U)
  {
    return;
  }

  power_pd_caps_dumped = 1U;

  if(power_ch224a_read_caps(data, &ndo, &bad) == 0U)
  {
    POWER_DBG("\r\n[pd] cap read fail\r\n");

    return;
  }

  head = (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));

  POWER_DBG("\r\n[pd] cap hdr=");
  POWER_DBG_HEX((uint8_t)(head >> 8));
  POWER_DBG_HEX((uint8_t)head);
  POWER_DBG(" type=");
  POWER_DBG_DEC((uint32_t)(head & 0x1FU));
  POWER_DBG(" rev=");
  POWER_DBG_DEC((uint32_t)((head >> 6) & 0x03U));
  POWER_DBG(" ndo=");
  POWER_DBG_DEC(ndo);
  POWER_DBG(" bad=");
  POWER_DBG_DEC(bad);

  if(((head & 0x1FU) != 0x01U) || (ndo == 0U) || (ndo > 7U))
  {
    POWER_DBG("\r\n");

    return;
  }

  for(i = 0U; i < ndo; i++)
  {
    uint32_t pdo;

    pdo = (uint32_t)data[2U + (i * 4U)]
        | ((uint32_t)data[3U + (i * 4U)] << 8)
        | ((uint32_t)data[4U + (i * 4U)] << 16)
        | ((uint32_t)data[5U + (i * 4U)] << 24);

    POWER_DBG("\r\n[pd] pdo");
    POWER_DBG_DEC(i);
    POWER_DBG("=");
    power_pd_dbg_hex32(pdo);

    if(((pdo >> 30) & 0x03U) == 0x00U)
    {
      POWER_DBG(" F ");
      POWER_DBG_DEC(((pdo >> 10) & 0x3FFU) * 50U);
      POWER_DBG("mV/");
      POWER_DBG_DEC((pdo & 0x3FFU) * 10U);
      POWER_DBG("mA");
    }
    else if(((pdo >> 30) & 0x03U) == 0x03U)
    {
      POWER_DBG(" PPS ");
      POWER_DBG_DEC(((pdo >> 8) & 0xFFU) * 100U);
      POWER_DBG("-");
      POWER_DBG_DEC(((pdo >> 17) & 0xFFU) * 100U);
      POWER_DBG("mV/");
      POWER_DBG_DEC((pdo & 0x7FU) * 50U);
      POWER_DBG("mA");
    }
    else
    {
      POWER_DBG(" BAT");
    }
  }

  POWER_DBG("\r\n");
}
#else
#define power_pd_dump_caps()            do {} while(0)
#define power_pd_caps_dumped_reset()    do {} while(0)
#endif


/* 菜单存的是 PD VOLT 的“选项号”：0 = AUTO，1..5 = 5/9/12/15/20V。
 * 老参数里可能残留越界值，一律退回 AUTO，别让固件去要一个整机没设计的电压。 */
static uint8_t power_pd_option_sanitize(uint8_t option)
{
  if(option >= POWER_PD_OPTION_COUNT)
  {
    return POWER_PD_OPTION_AUTO;
  }

  return option;
}


/* 选项号 -> CH224A 0x0A 的电压索引。AUTO 没有固定索引，返回 POWER_PD_INDEX_NONE，
 * 由 power_pd_auto_resolve() 去问适配器。只放开 15V / 20V：低压档 PMOS 导通电阻
 * 损耗大、发热大，整机不在 15V 以下工作。 */
static uint8_t power_pd_option_to_index(uint8_t option)
{
  static const uint8_t map[POWER_PD_OPTION_COUNT] = {
    POWER_PD_INDEX_NONE,      /* 0    AUTO */
    POWER_CH224A_INDEX_15V,   /* 1    15V  */
    POWER_CH224A_INDEX_20V    /* 2    20V  */
  };

  if(option >= POWER_PD_OPTION_COUNT)
  {
    return POWER_PD_INDEX_NONE;
  }

  return map[option];
}


/* AUTO：问适配器它能给哪几档，取最高的那一档，并且不许超过当前上限
 * power_pd_auto_cap —— 那个上限就是“谈不下来 / 把电源拉重启”之后一步步压下来的值。 */
static void power_pd_auto_resolve(uint32_t now_ms)
{
  uint8_t idx;

  if(power_pd_auto_index != POWER_PD_INDEX_NONE)
  {
    return;
  }

  if((power_pd_auto_tries != 0U)
     && ((uint32_t)(now_ms - power_pd_auto_step_ms) < POWER_PD_AUTO_RETRY_MS))
  {
    return;
  }

  power_pd_auto_tries++;
  power_pd_auto_step_ms = now_ms;

  idx = power_ch224a_max_index_from_caps();

  if(idx == POWER_PD_INDEX_NONE)
  {
    /* 读失败：可能这颗不是 PD 源，也可能这么密集的读把 I2C1 读锁死了。先做一次总线
     * 恢复，别让一次失败的解析把后面的纠偏一起带走。 */
    power_i2c_recover_bus();

    /* 读不出能力表：可能这颗根本不是 PD 源（纯 QC/DC 头），也可能缓冲一直没稳定。
     * 试满次数就不再等，直接按上限要档，谈不成自然有 watchdog 一层层往下退。 */
    if(power_pd_auto_tries >= POWER_PD_AUTO_MAX_TRIES)
    {
      power_pd_auto_index = power_pd_auto_cap;

      POWER_DBG("\r\n[pd] auto no caps -> idx=");
      POWER_DBG_DEC(power_pd_auto_index);
      POWER_DBG("\r\n");
    }

    return;
  }

  if(idx < POWER_PD_INDEX_MIN)
  {
    /* 这颗供电连 15V 都没有（只有 5V / 9V 的头）：整机不在 15V 以下工作，就按
     * 最低允许档去要，要不到是它的问题，不去偷偷放松欠压门限。 */
    idx = POWER_PD_INDEX_MIN;
  }

  if(idx > power_pd_auto_cap)
  {
    idx = power_pd_auto_cap;
  }

  power_pd_auto_index = idx;

  POWER_DBG("\r\n[pd] auto top idx=");
  POWER_DBG_DEC(idx);
  POWER_DBG("\r\n");
}


/* AUTO 退一档：这一档谈不下来，或者把供电方拉得掉压重启了，就往下退一档重来。
 * 只退到 POWER_PD_INDEX_MIN（15V）为止 —— 再低 PMOS 发热大，整机不打算在那个电压
 * 下工作。返回 0 表示已经退无可退，调用方该去走放弃/退避，不要几秒就重谈一次。 */
static uint8_t power_pd_auto_step_down(uint32_t now_ms, const char *why)
{
  (void)why;

  if(power_pd_auto_index == POWER_PD_INDEX_NONE)
  {
    power_pd_auto_index = power_pd_auto_cap;
  }

  if(power_pd_auto_index <= POWER_PD_INDEX_MIN)
  {
    return 0U;
  }

  power_pd_auto_index--;
  power_pd_auto_cap = power_pd_auto_index;

  POWER_DBG("\r\n[pd] auto down(");
  POWER_DBG(why);
  POWER_DBG(") -> idx=");
  POWER_DBG_DEC(power_pd_auto_index);
  POWER_DBG("\r\n");

  power_ch224a_target_index = power_pd_auto_index;
  power_pd_action = POWER_PD_MONITOR;
  power_pd_bad_since_ms = 0U;
  power_pd_attempts = 0U;
  power_pd_step_ms = now_ms;

  return 1U;
}


/* 本轮放弃：退到后台，隔 POWER_PD_BACKOFF_MS 再看一眼。手动档要不到目标档，
 * 或者 AUTO 已经退到最低允许档还是要不到，都走这里。 */
static void power_pd_give_up(uint32_t now_ms, uint8_t index)
{
  power_pd_step_ms = now_ms;
  power_pd_action = POWER_PD_GIVE_UP;

  POWER_DBG("\r\n[pd] wd GIVEUP idx=");
  POWER_DBG_DEC(index);
  POWER_DBG(" st=0x");
  POWER_DBG_HEX(power_ch224a_status);
  POWER_DBG(" mv=");
  POWER_DBG_DEC(power_voltage_mv);
  POWER_DBG("\r\n");
}


/* 跟踪菜单里选的 PD 档位。这里只改目标档位，不在主循环里做阻塞协商：
 * 把 watchdog 清回 MONITOR，它下个周期发现实测 VBUS 和新目标不一致，就自己走
 * commit -> 重写目标档 -> 校验，换档过程显示和加热都不会卡住。
 * PD VOLT 只放开 AUTO / 15V / 20V（电压越低 PMOS 导通损耗和发热越大），所以这里
 * 不会再碰到 5V / 9V 那种"芯片谈不到、只能保持默认档"的档位。 */
static void power_pd_slot_task(uint32_t now_ms)
{
#if POWER_CH224A_DEBUG
  /* 第一拍打一次"配置从哪来的"，用来查"改了 PD VOLT 重启又变回 AUTO"：
   *   opt=2 eev=1  -> 存和读都对，问题出在界面显示；
   *   opt=0 eev=1  -> 存进去的就是 0，保存那条链路的问题；
   *   eev=0        -> 压根没认到有效记录，走的是出厂默认（opt 被强制成 0），
   *                   ee 会告诉我们是 CORRUPT 还是 OLD_VERSION。
   * 放在这一拍而不是 power_init()，是因为 power_init() 跑得比 eeprom_init() 早，
   * 那时候参数还没加载。 */
  if(power_dbg_cfg_done == 0U)
  {
    power_dbg_cfg_done = 1U;

    POWER_DBG("\r\n[pd] cfg opt=");
    POWER_DBG_DEC(g_eeprom_params.ch224a_index);
    POWER_DBG(" eev=");
    POWER_DBG_DEC(eeprom_has_valid_record());
    POWER_DBG(" ee=");
    POWER_DBG_DEC((uint32_t)eeprom_get_error());
    POWER_DBG("\r\n");
  }
#endif
  uint8_t option;
  uint8_t slot;

  option = power_pd_option_sanitize(g_eeprom_params.ch224a_index);

  if(option == POWER_PD_OPTION_AUTO)
  {
    /* AUTO：每拍只问一次“解析好了没有”，真正的解析在 resolve 里按时间片限流 */
    power_pd_auto_resolve(now_ms);
    slot = (power_pd_auto_index != POWER_PD_INDEX_NONE)
         ? power_pd_auto_index
         : power_pd_auto_cap;
  }
  else
  {
    /* 手动档：把 AUTO 状态整个清掉，下次切回 AUTO 时重新解析、重新从最高档试起 */
    if(power_pd_auto_index != POWER_PD_INDEX_NONE)
    {
      power_pd_auto_index = POWER_PD_INDEX_NONE;
      power_pd_auto_tries = 0U;
      power_pd_auto_cap = POWER_CH224A_INDEX_20V;
    }

    slot = power_pd_option_to_index(option);
  }

  if(slot == power_ch224a_target_index)
  {
    return;
  }

  power_ch224a_target_index = slot;
  power_pd_action = POWER_PD_MONITOR;
  power_pd_bad_since_ms = 0U;
  power_pd_step_ms = now_ms;
  power_pd_attempts = 0U;

  POWER_DBG("\r\n[pd] slot->");
  POWER_DBG_DEC(slot);
  POWER_DBG("\r\n");
}


static void power_pd_watchdog(uint32_t now_ms)
{
  uint8_t index;

  if(power_initialized == 0U)
  {
    return;
  }

  if(power_ch224a_found == 0U)
  {
    /* 芯片临时不应答（拔插 PD 源的一瞬间会这样）：按退避节奏重新探测，
     * 不能就此永远放弃，否则重新插回去档位就再也不会恢复了。*/
    if((power_pd_step_ms == 0U) ||
       ((uint32_t)(now_ms - power_pd_step_ms) >= POWER_PD_BACKOFF_MS))
    {
      power_pd_step_ms = now_ms;

      /* 先做总线恢复再探。I2C1 一旦锁死（一笔事务之后全在地址阶段 TIMEOUT），光
       * 重新探测是起不来的 —— 实测就是这么一直 found=0；得 9 个时钟 + STOP 把卡在
       * 低电平的从机放开，再复位重新初始化我们自己的 I2C1。 */
      power_i2c_recover_bus();
      (void)power_ch224a_find();

      if(power_ch224a_found != 0U)
      {
        /* 重新发现芯片（换供电方、或者总线刚从锁死里恢复）：能力表允许再 dump 一次 */
        power_pd_caps_dumped_reset();
      }

      POWER_DBG("\r\n[pd] reprobe found=");
      POWER_DBG_DEC(power_ch224a_found);
      POWER_DBG(" st=0x");
      POWER_DBG_HEX(power_ch224a_status);
      POWER_DBG(" mv=");
      POWER_DBG_DEC(power_voltage_mv);
      POWER_DBG("\r\n");
    }

    return;
  }

  index = power_ch224a_target_index;

  if(index == POWER_CH224A_INDEX_5V)
  {
    power_pd_action = POWER_PD_MONITOR;
    power_pd_bad_since_ms = 0U;
    return;
  }

  switch(power_pd_action)
  {
    case POWER_PD_MONITOR:

      /* VBUS 掉到 5V 以下，而芯片还在应答 I2C（线还插着，不是被拔掉了）：这是供电方
       * 被我们拽重启了。不排队等 1.5 s 容错计时，AUTO 马上退一档重来。
       * 只在 MONITOR 里判：换档过程中本来就会抖，那会儿在 COMMIT / REQUEST 状态。 */
      if((power_voltage_mv != 0U) && (power_voltage_mv < POWER_PD_COLLAPSE_MV)
         && (power_pd_option_sanitize(g_eeprom_params.ch224a_index) == POWER_PD_OPTION_AUTO)
         && (power_pd_auto_step_down(now_ms, "supply reset") != 0U))
      {
        break;
      }

      if(power_vin_matches_index(index) == 0U)
      {
        if(power_pd_bad_since_ms == 0U)
        {
          power_pd_bad_since_ms = now_ms;
        }
        else if((uint32_t)(now_ms - power_pd_bad_since_ms) >= POWER_PD_FAULT_MS)
        {
          /* 先把合同 commit 一次：实测 0x09 回到 0x48 时直接写目标档会被忽略 */
          power_pd_bad_since_ms = 0U;
          power_pd_attempts = 0U;

          POWER_DBG("\r\n[pd] watchdog fix idx=");
          POWER_DBG_DEC(index);
          POWER_DBG(" mv=");
          POWER_DBG_DEC(power_voltage_mv);
          POWER_DBG("\r\n");

          if(power_ch224a_write_reg(power_ch224a_address,
                                    POWER_CH224A_REG_VOLTAGE,
                                    POWER_CH224A_INDEX_COMMIT) != 0U)
          {
            power_pd_step_ms = now_ms;
            power_pd_action = POWER_PD_COMMIT;
          }
          else
          {
            power_ch224a_found = 0U;
          }
        }
      }
      else
      {
        power_pd_bad_since_ms = 0U;
        power_pd_attempts = 0U;
#if POWER_CH224A_DEBUG
        if((uint32_t)(now_ms - power_dbg_hb_ms) >= 5000U)
        {
          power_dbg_hb_ms = now_ms;
          POWER_DBG("\r\n[pd] hb st=0x");
          POWER_DBG_HEX(power_ch224a_status);
          POWER_DBG(" mv=");
          POWER_DBG_DEC(power_voltage_mv);
          POWER_DBG("\r\n");
        }
#endif
      }
      break;

    case POWER_PD_COMMIT:

      if((uint32_t)(now_ms - power_pd_step_ms) >= POWER_PD_STEP_MS)
      {
        if(power_ch224a_write_reg(power_ch224a_address,
                                  POWER_CH224A_REG_VOLTAGE, index) != 0U)
        {
          power_pd_step_ms = now_ms;
          power_pd_action = POWER_PD_REQUEST;
          POWER_DBG("\r\n[pd] wd req idx=");
          POWER_DBG_DEC(index);
          POWER_DBG("\r\n");
        }
        else
        {
          power_ch224a_found = 0U;
          power_pd_action = POWER_PD_MONITOR;
        }
      }
      break;

    case POWER_PD_REQUEST:

      if((uint32_t)(now_ms - power_pd_step_ms) >= POWER_PD_FAULT_MS)
      {
        if(power_vin_matches_index(index) == 0U)
        {
          power_pd_attempts++;

          if(power_pd_attempts >= POWER_PD_MAX_ATTEMPTS)
          {
            /* 手动档要不到这一档，或者 AUTO 已经退到最低允许档还是要不到：退到后台，
             * 隔一段时间再试。AUTO 还能退档的话就退一档重来，不趴退避。 */
            if((power_pd_option_sanitize(g_eeprom_params.ch224a_index) != POWER_PD_OPTION_AUTO)
               || (power_pd_auto_step_down(now_ms, "no slot") == 0U))
            {
              power_pd_give_up(now_ms, index);
            }
          }
          else if(power_ch224a_write_reg(power_ch224a_address,
                                         POWER_CH224A_REG_VOLTAGE,
                                         POWER_CH224A_INDEX_COMMIT) != 0U)
          {
            power_pd_step_ms = now_ms;
            power_pd_action = POWER_PD_COMMIT;
          }
          else
          {
            power_ch224a_found = 0U;
            power_pd_action = POWER_PD_MONITOR;
          }
        }
        else
        {
          /* 实测 VBUS 已经落在目标档窗口内：这轮纠偏成功，回到正常监控 */
          power_pd_action = POWER_PD_MONITOR;
          power_pd_bad_since_ms = 0U;
          power_pd_attempts = 0U;
          POWER_DBG("\r\n[pd] wd OK idx=");
          POWER_DBG_DEC(index);
          POWER_DBG(" st=0x");
          POWER_DBG_HEX(power_ch224a_status);
          POWER_DBG(" mv=");
          POWER_DBG_DEC(power_voltage_mv);
          POWER_DBG("\r\n");
          power_pd_dump_caps();
        }
      }
      break;

    default:

      if((uint32_t)(now_ms - power_pd_step_ms) >= POWER_PD_BACKOFF_MS)
      {
        power_pd_action = POWER_PD_MONITOR;
        power_pd_bad_since_ms = 0U;
      }
      break;
  }
}

uint8_t power_ch224a_set_voltage_index(uint8_t index)
{
  return power_ch224a_negotiate_index(index);
}

uint8_t power_ch224a_set_voltage(float volts)
{
  return power_ch224a_negotiate(volts);
}

uint8_t power_ch224a_read_reg(uint8_t reg, uint8_t *value)
{
  if((value == NULL) || (power_initialized == 0U))
  {
    return 0U;
  }

  if(power_ch224a_found == 0U)
  {
    if(power_ch224a_find() == 0U)
    {
      return 0U;
    }
  }

  if(power_ch224a_read_reg_addr(power_ch224a_address, reg, value) == 0U)
  {
    power_ch224a_found = 0U;
    return 0U;
  }

  if(reg == POWER_CH224A_REG_STATUS)
  {
    power_ch224a_status = *value;
  }

  return 1U;
}

uint8_t power_ch224a_get_status(void)
{
  return power_ch224a_status;
}

uint8_t power_ch224a_is_present(void)
{
  return power_ch224a_found;
}

uint8_t power_ch224a_get_target_index(void)
{
  return power_ch224a_target_index;
}

/* ------------------------------------------------------------------------- */
/* 电压、休眠、保护                                                           */
/* ------------------------------------------------------------------------- */

uint16_t power_vin_raw_to_mv(uint16_t raw)
{
  uint32_t mv;

  mv = ((uint32_t)raw * POWER_VIN_VREF_MV *
        (POWER_VIN_DIVIDER_R1 + POWER_VIN_DIVIDER_R2)) /
       (POWER_ADC_FULL_SCALE * POWER_VIN_DIVIDER_R2);

  return (uint16_t)mv;
}

uint16_t power_get_voltage_mv(void)
{
  return power_voltage_mv;
}

float power_get_ch224a_voltage(void)
{
  return power_ch224a_voltage_v;
}

void power_notify_activity(uint32_t now_ms)
{
  power_last_motion_ms = now_ms;
  power_low_power_accum_ms = 0U;
  power_low_power_last_ms = now_ms;
}

power_state_t power_get_state(void)
{
  return power_state;
}

uint8_t power_is_sleeping(void)
{
  return ((power_state == POWER_STATE_HEATER_OFF) ||
          (power_state == POWER_STATE_BACKLIGHT_OFF)) ? 1U : 0U;
}

uint8_t power_get_vibration_state(void)
{
  uint32_t now_ms = sm_get_uptime_ms();

  /* 只有稳定低电平才算震动，避免悬空/抖动误触发 */
  if(power_vibration_stable_level == 0U)
  {
    return 1U;
  }

  return ((int32_t)(power_vibration_latch_ms - now_ms) > 0) ? 1U : 0U;
}

uint32_t power_get_sleep_remaining_ms(void)
{
  uint32_t now_ms = sm_get_uptime_ms();
  uint32_t elapsed;
  uint32_t timeout;

  switch(power_state)
  {
    case POWER_STATE_ACTIVE:
      timeout = power_stationary_timeout_ms;
      elapsed = power_low_power_accum_ms;
      break;

    case POWER_STATE_REDUCED:
      timeout = power_reduced_off_timeout_ms;
      elapsed = (uint32_t)(now_ms - power_state_enter_ms);
      break;

    case POWER_STATE_HEATER_OFF:
      timeout = power_backlight_off_timeout_ms;
      elapsed = (uint32_t)(now_ms - power_state_enter_ms);
      break;

    case POWER_STATE_BACKLIGHT_OFF:
    default:
      return 0U;
  }

  return (elapsed >= timeout) ? 0U : (timeout - elapsed);
}

power_fault_t power_get_fault(void)
{
  return power_fault;
}

void power_register_fault_callback(power_fault_callback_t callback)
{
  power_fault_callback = callback;
}

static uint8_t power_read_vibration_level(void)
{
  return (gpio_input_data_bit_read(GPIOB, GPIO_PINS_1) == SET) ? 1U : 0U;
}

static void power_restore_setpoint(void)
{
  if(power_saved_setpoint_valid != 0U)
  {
    (void)sm_set_setpoint(power_saved_setpoint_c);
    power_saved_setpoint_valid = 0U;
  }
}

static void power_enter_reduced(uint32_t now_ms)
{
  sm_status_t status;
  float saved;

  sm_get_status(&status);

  /* 整定过程中不要改目标温度 */
  if(status.autotune_state == PID_AUTOTUNE_RUNNING)
  {
    power_last_motion_ms = now_ms;
    return;
  }

  saved = sm_get_setpoint();
  power_saved_setpoint_c = saved;
  power_saved_setpoint_valid = 1U;

  if(power_reduced_temp_c < saved)
  {
    if(sm_set_setpoint(power_reduced_temp_c) == 0U)
    {
      power_saved_setpoint_valid = 0U;
      power_last_motion_ms = now_ms;
      return;
    }
  }

  power_state = POWER_STATE_REDUCED;
  power_state_enter_ms = now_ms;
  power_low_power_accum_ms = 0U;
}

static void power_enter_heater_off(uint32_t now_ms)
{
  power_sleep_heater_was_enabled = sm_is_heater_enabled();
  (void)sm_set_heater_enabled(0U);
  power_state = POWER_STATE_HEATER_OFF;
  power_state_enter_ms = now_ms;
}

static void power_enter_backlight_off(uint32_t now_ms)
{
  if(power_state != POWER_STATE_HEATER_OFF)
  {
    power_sleep_heater_was_enabled = sm_is_heater_enabled();
  }

  display_backlight_set(0U);
  power_state = POWER_STATE_BACKLIGHT_OFF;
  power_state_enter_ms = now_ms;
}

static void power_wake_now(uint32_t now_ms)
{
  display_backlight_set(1U);

  power_vibration_wake_block_until_ms = 0U;
  power_last_motion_ms = now_ms;
  power_low_power_accum_ms = 0U;
  power_low_power_last_ms = now_ms;
  power_state_enter_ms = now_ms;
  power_vibration_raw_level = power_read_vibration_level();
  power_vibration_stable_level = power_vibration_raw_level;
  power_vibration_raw_change_ms = now_ms;

  if(power_fault != POWER_FAULT_NONE)
  {
    power_wake_pending = 1U;
    power_restore_setpoint();
    power_state = POWER_STATE_ACTIVE;
    return;
  }

  if(power_state == POWER_STATE_REDUCED)
  {
    power_restore_setpoint();
  }
  else if((power_state == POWER_STATE_HEATER_OFF) ||
          (power_state == POWER_STATE_BACKLIGHT_OFF))
  {
    power_restore_setpoint();

    if(power_sleep_heater_was_enabled != 0U)
    {
      (void)sm_set_heater_enabled(1U);
    }

    power_sleep_heater_was_enabled = 0U;
  }

  power_state = POWER_STATE_ACTIVE;
}

void power_wake(void)
{
  power_wake_now(sm_get_uptime_ms());
}

static void power_sleep_common(uint8_t screen_off, uint8_t restore_heater)
{
  uint32_t now_ms;

  if(power_initialized == 0U)
  {
    return;
  }

  if(power_state == POWER_STATE_BACKLIGHT_OFF)
  {
    return;
  }

  now_ms = sm_get_uptime_ms();

  /* 按键手动休眠后 30 s 内屏蔽震动唤醒，避免放下烙铁时立刻被震动叫醒 */
  power_vibration_wake_block_until_ms = now_ms + POWER_MANUAL_SLEEP_WAKE_BLOCK_MS;

  /* 手动休眠也要记住原设定温度，唤醒后恢复 */
  if(power_saved_setpoint_valid == 0U)
  {
    power_saved_setpoint_c = sm_get_setpoint();
    power_saved_setpoint_valid = 1U;
  }

  if(power_state != POWER_STATE_HEATER_OFF)
  {
    power_enter_heater_off(now_ms);
  }

  if(restore_heater == 0U)
  {
    power_sleep_heater_was_enabled = 0U;
  }

  if(screen_off != 0U)
  {
    power_enter_backlight_off(now_ms);
  }
}

void power_sleep(void)
{
  /* 菜单 SLEEP NOW：关加热 + 背光，唤醒时恢复休眠前的加热状态 */
  power_sleep_common(1U, 1U);
}

void power_heater_off(void)
{
  /* 短按 PREV：只关加热，屏幕保持，唤醒后不自动恢复加热 */
  power_sleep_common(0U, 0U);
}

void power_sleep_deep(void)
{
  /* 长按 PREV：关加热+屏幕，唤醒后加热保持关闭 */
  power_sleep_common(1U, 0U);
}

static void power_latch_fault(power_fault_t fault, uint16_t voltage_mv)
{
  power_fault = fault;
  power_voltage_bad_since_ms = 0U;
  power_voltage_good_since_ms = 0U;

  power_fault_heater_was_enabled = sm_is_heater_enabled();
  (void)sm_set_heater_enabled(0U);

  if(power_fault_callback != NULL)
  {
    power_fault_callback(fault, voltage_mv);
  }
}

uint8_t power_clear_fault(void)
{
  uint8_t restore_heater;

  if(power_fault == POWER_FAULT_NONE)
  {
    return 1U;
  }

  if((power_voltage_mv <=
      (uint16_t)(power_low_voltage_mv + power_voltage_hysteresis_mv)) ||
     (power_voltage_mv >=
      (uint16_t)(power_high_voltage_mv - power_voltage_hysteresis_mv)))
  {
    return 0U;
  }

  restore_heater = ((power_fault_heater_was_enabled != 0U) ||
                    (power_wake_pending != 0U)) ? 1U : 0U;

  power_fault = POWER_FAULT_NONE;
  power_voltage_bad_since_ms = 0U;
  power_voltage_good_since_ms = 0U;
  power_fault_heater_was_enabled = 0U;
  power_wake_pending = 0U;

  if((restore_heater != 0U) &&
     ((power_state == POWER_STATE_ACTIVE) ||
      (power_state == POWER_STATE_REDUCED)))
  {
    (void)sm_set_heater_enabled(1U);
  }

  return 1U;
}

static void power_voltage_task(uint32_t now_ms)
{
  sm_status_t status;

  if((uint32_t)(now_ms - power_last_voltage_sample_ms) <
     POWER_VOLTAGE_SAMPLE_MS)
  {
    return;
  }

  power_last_voltage_sample_ms = now_ms;

  sm_get_status(&status);

  if(status.adc_valid == 0U)
  {
    power_voltage_bad_since_ms = 0U;
    power_voltage_good_since_ms = 0U;
    return;
  }

  /* 休眠计时只由震动/按键等活动输入重置，不再根据功率或温度误差推断使用状态 */

  power_voltage_mv = power_vin_raw_to_mv(status.vin_raw);

#if POWER_CH224A_DEBUG
  /* 输入电压跳一档就打一行，带上当时的目标档、状态机状态、芯片在不在线。
   * 以后再遇到"电压自己掉了"，一眼能分清是我们写 0x0A 写的，还是供电方退档 /
   * CH224A 掉回 CFG 默认档：want 和 act 没变却掉档，就是外面干的。
   *
   * 门限取 2500mV：档位级变化最小也有 5000mV（15V->20V），而实测加热一开始工作
   * VIN 本身就带正负几百毫伏的纹波（刷完第一次抓 30 s，读数在 18.9V~21.4V 之间
   * 来回晃，2.5V 峰峰值）。上一版用 500mV 直接被纹波当"跳档"打了 7.6KB 串口。
   * 再加一条最少隔 1 s 才允许打一条，就算哪天真有慢速漂移也灌不死串口。 */
  {
    int32_t dv = (int32_t)power_voltage_mv - (int32_t)power_dbg_last_mv;

    if(((dv >= 2500) || (dv <= -2500))
       && ((uint32_t)(now_ms - power_dbg_last_print_ms) >= 1000U))
    {
      POWER_DBG("\r\n[pd] vbus ");
      POWER_DBG_DEC(power_dbg_last_mv);
      POWER_DBG("->");
      POWER_DBG_DEC(power_voltage_mv);
      POWER_DBG(" want=");
      POWER_DBG_DEC(power_ch224a_target_index);
      POWER_DBG(" act=");
      POWER_DBG_DEC(power_pd_action);
      POWER_DBG(" found=");
      POWER_DBG_DEC(power_ch224a_found);
      POWER_DBG(" st=0x");
      POWER_DBG_HEX(power_ch224a_status);
      POWER_DBG("\r\n");

      power_dbg_last_mv = power_voltage_mv;
      power_dbg_last_print_ms = now_ms;
    }
  }
#endif

  /* 上电/CH224A 重新协商期间不触发保护 */
  if(now_ms < POWER_VOLTAGE_STARTUP_GRACE_MS)
  {
    power_voltage_bad_since_ms = 0U;
    power_voltage_good_since_ms = 0U;
    return;
  }

  if(power_fault == POWER_FAULT_NONE)
  {
    if((power_voltage_mv < power_low_voltage_mv) ||
       (power_voltage_mv > power_high_voltage_mv))
    {
      if(power_voltage_bad_since_ms == 0U)
      {
        power_voltage_bad_since_ms = now_ms;
      }
      else if((uint32_t)(now_ms - power_voltage_bad_since_ms) >=
              power_voltage_trip_ms)
      {
        if(power_voltage_mv < power_low_voltage_mv)
        {
          power_latch_fault(POWER_FAULT_UNDERVOLTAGE, power_voltage_mv);
        }
        else
        {
          power_latch_fault(POWER_FAULT_OVERVOLTAGE, power_voltage_mv);
        }
      }
    }
    else
    {
      power_voltage_bad_since_ms = 0U;
    }
  }
  else
  {
    if((power_voltage_mv >
        (uint16_t)(power_low_voltage_mv + power_voltage_hysteresis_mv)) &&
       (power_voltage_mv <
        (uint16_t)(power_high_voltage_mv - power_voltage_hysteresis_mv)))
    {
      if(power_voltage_good_since_ms == 0U)
      {
        power_voltage_good_since_ms = now_ms;
      }
      else if((uint32_t)(now_ms - power_voltage_good_since_ms) >=
              power_voltage_recovery_ms)
      {
        (void)power_clear_fault();
      }
    }
    else
    {
      power_voltage_good_since_ms = 0U;
    }
  }
}

static uint8_t power_next_wake_click;

static void power_on_button(button_id_t button, button_event_t event)
{
  (void)button;

  if(event == BUTTON_EVENT_PRESS)
  {
    power_wake();
  }
}

/* 休眠时短按 NEXT 唤醒；NEXT 不再用 PRESS 唤醒，避免按下就翻页 */
static void power_on_next_click(button_id_t button, button_event_t event)
{
  (void)button;

  if((event == BUTTON_EVENT_CLICK) && (power_is_sleeping() != 0U))
  {
    power_next_wake_click = 1U;
    power_wake();
  }
}

uint8_t power_take_wake_click(void)
{
  uint8_t pending = power_next_wake_click;

  power_next_wake_click = 0U;
  return pending;
}

void power_init(void)
{
  gpio_init_type gpio_init_struct;

  power_stationary_timeout_ms = POWER_DEFAULT_STATIONARY_TIMEOUT_MS;
  power_low_power_threshold = POWER_LOW_POWER_DEFAULT;
  power_reduced_temp_c = POWER_DEFAULT_REDUCED_TEMP_C;
  power_reduced_off_timeout_ms = POWER_DEFAULT_REDUCED_OFF_TIMEOUT_MS;
  power_backlight_off_timeout_ms = POWER_DEFAULT_BACKLIGHT_OFF_TIMEOUT_MS;

  power_low_voltage_mv = POWER_DEFAULT_LOW_VOLTAGE_MV;
  power_high_voltage_mv = POWER_DEFAULT_HIGH_VOLTAGE_MV;
  power_voltage_hysteresis_mv = POWER_DEFAULT_VOLTAGE_HYSTERESIS_MV;
  power_voltage_trip_ms = POWER_DEFAULT_VOLTAGE_TRIP_MS;
  power_voltage_recovery_ms = POWER_DEFAULT_VOLTAGE_RECOVERY_MS;

  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

  /* PB1 震动开关，外部 10k 上拉，静止时为高 */
  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_pins = GPIO_PINS_1;
  gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init(GPIOB, &gpio_init_struct);

  /* CH224A I2C1：PB6=SCL，PB7=SDA，MUX_1，板上 1k 上拉 */
  crm_periph_clock_enable(CRM_I2C1_PERIPH_CLOCK, TRUE);

  gpio_init_struct.gpio_pins = GPIO_PINS_6 | GPIO_PINS_7;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_OPEN_DRAIN;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOB, &gpio_init_struct);

  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE6, GPIO_MUX_1);
  gpio_pin_mux_config(GPIOB, GPIO_PINS_SOURCE7, GPIO_MUX_1);

  /* 上电时 CH224A 可能正被上一次掉电打断，I2C1 也有概率停在半笔事务上，
   * 症状是之后每一笔传输都在地址阶段超时。所以先复位外设再做一次总线恢复。
   * 
   * 原来这里还有一行 i2c_own_address1_set(I2C1, I2C_ADDRESS_MODE_7BIT, 0xA0)，
   * 那是“本机作为从机”时的自身地址，设了之后 I2C1 会去应答总线上对 0x50 的
   * 地址匹配，等于自己接自己发出去的东西。我们只做主机，删掉。 */
  power_i2c_recover_bus();

  power_ch224a_found = 0U;
  power_ch224a_address = POWER_CH224A_I2C_ADDR_PRIMARY;
  power_ch224a_voltage_v = POWER_CH224A_DEFAULT_VOLTAGE_V;
  power_ch224a_status = 0U;
  /* 菜单存的是 PD VOLT 选项号。AUTO 在 power_init 里解析不了（这时候 I2C 还没探过，
   * 而且 power_init 跑在 eeprom_init 之前，参数都还没加载），先按 20V 起步，主循环
   * 第一拍 power_pd_slot_task() 会照菜单值把它纠正过来。 */
  power_pd_auto_index = POWER_PD_INDEX_NONE;
  power_pd_auto_tries = 0U;
  power_pd_auto_cap = POWER_CH224A_INDEX_20V;
  power_ch224a_target_index = power_pd_option_to_index(power_pd_option_sanitize(g_eeprom_params.ch224a_index));

  if(power_ch224a_target_index == POWER_PD_INDEX_NONE)
  {
    power_ch224a_target_index = POWER_CH224A_INDEX_20V;
  }
  power_pd_action = POWER_PD_MONITOR;
  power_pd_bad_since_ms = 0U;
  power_pd_step_ms = 0U;
  power_pd_attempts = 0U;

  power_state = POWER_STATE_ACTIVE;
  power_fault = POWER_FAULT_NONE;
  power_voltage_mv = 0U;
  power_last_voltage_sample_ms = 0U;
  power_voltage_bad_since_ms = 0U;
  power_voltage_good_since_ms = 0U;
  power_last_motion_ms = sm_get_uptime_ms();
  power_low_power_accum_ms = 0U;
  power_low_power_last_ms = power_last_motion_ms;
  power_state_enter_ms = power_last_motion_ms;
  power_vibration_raw_level = power_read_vibration_level();
  power_vibration_stable_level = power_vibration_raw_level;
  power_vibration_raw_change_ms = power_last_motion_ms;
  power_vibration_latch_ms = 0U;
  power_vibration_wake_block_until_ms = 0U;
  power_saved_setpoint_valid = 0U;
  power_fault_heater_was_enabled = 0U;
  power_wake_pending = 0U;

  power_initialized = 1U;

  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_PRESS,
                                  power_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_PRESS,
                                  power_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_CLICK,
                                  power_on_next_click);

  /* 默认档位 20V，但不在上电路径里做阻塞协商：这时 sm_init()/adc_init() 还没跑，
   * VBUS 测不到，协商没法校验，只会白等两三秒再报失败。目标 20V 和实测 9V 天然
   * 就是“档位不一致”，power_pd_watchdog 会在开机头几秒（ADC 已活、PD 已 attach）
   * 非阻塞地把它谈上来。这里只做一次探测，把芯片地址和 0x09 读出来。 */
  POWER_DBG_INIT();
  (void)power_ch224a_find();

  POWER_DBG("\r\n[pd] init found=");
  POWER_DBG_DEC(power_ch224a_found);
  POWER_DBG(" addr=0x");
  POWER_DBG_HEX(power_ch224a_address);
  POWER_DBG(" st=0x");
  POWER_DBG_HEX(power_ch224a_status);
  POWER_DBG(" target=");
  POWER_DBG_DEC(power_ch224a_target_index);
  POWER_DBG("\r\n");
}

void power_update(uint32_t now_ms)
{
  uint8_t level;
  uint8_t motion_activity;
  sm_status_t status;
  uint32_t dt_ms;
  uint32_t drain_ms;

  if(power_initialized == 0U)
  {
    return;
  }

  power_voltage_task(now_ms);

  /* CH224A 档位保持：输入电压长期和已请求档位不一致就重新 commit + 请求。
  * 非阻塞，正常情况下一 tick 只是比较一次电压。*/
  /* 菜单里换了 PD 档位就先更新目标，真正的切换交给下面的 watchdog 非阻塞完成 */
  power_pd_slot_task(now_ms);
  power_pd_watchdog(now_ms);

  level = power_read_vibration_level();
  motion_activity = 0U;

  if(level != power_vibration_raw_level)
  {
    power_vibration_raw_level = level;
    power_vibration_raw_change_ms = now_ms;
    power_vibration_latch_ms = now_ms + 500U;
  }

  if(((uint32_t)(now_ms - power_vibration_raw_change_ms) >=
      POWER_VIBRATION_ACTIVITY_MS) &&
     (power_vibration_stable_level != power_vibration_raw_level))
  {
    power_vibration_stable_level = power_vibration_raw_level;
    motion_activity = 1U;
  }

  if(power_fault != POWER_FAULT_NONE)
  {
    return;
  }

  dt_ms = (uint32_t)(now_ms - power_low_power_last_ms);
  power_low_power_last_ms = now_ms;
  if(dt_ms > 1000U)
  {
    dt_ms = 1000U;
  }

  switch(power_state)
  {
    case POWER_STATE_ACTIVE:
      /* 休眠只看功率：≤15% 的功率占绝对多数才累积休眠时间 */
      sm_get_status(&status);
      if(status.power <= power_low_power_threshold)
      {
        power_low_power_accum_ms += dt_ms;
        if(power_low_power_accum_ms > power_stationary_timeout_ms)
        {
          power_low_power_accum_ms = power_stationary_timeout_ms;
        }
      }
      else
      {
        drain_ms = dt_ms * POWER_LOW_POWER_DRAIN;
        power_low_power_accum_ms =
          (power_low_power_accum_ms > drain_ms) ?
          (power_low_power_accum_ms - drain_ms) : 0U;
      }

      if(power_low_power_accum_ms >= power_stationary_timeout_ms)
      {
        power_enter_reduced(now_ms);
      }
      break;

    case POWER_STATE_REDUCED:
      if(motion_activity != 0U)
      {
        power_wake_now(now_ms);
      }
      else if((uint32_t)(now_ms - power_state_enter_ms) >=
              power_reduced_off_timeout_ms)
      {
        power_enter_heater_off(now_ms);
      }
      break;

    case POWER_STATE_HEATER_OFF:
      if((motion_activity != 0U) &&
         ((int32_t)(power_vibration_wake_block_until_ms - now_ms) <= 0))
      {
        /* 手动 disabled 状态下震动直接唤醒并恢复加热 */
        power_wake_now(now_ms);
        (void)sm_set_heater_enabled(1U);
      }
      else if((uint32_t)(now_ms - power_state_enter_ms) >=
              power_backlight_off_timeout_ms)
      {
        power_enter_backlight_off(now_ms);
      }
      break;

    case POWER_STATE_BACKLIGHT_OFF:
    default:
      break;
  }
}

void power_set_stationary_timeout_ms(uint32_t ms)
{
  if(ms >= POWER_MIN_TIMEOUT_MS)
  {
    power_stationary_timeout_ms = ms;
  }
}

uint32_t power_get_stationary_timeout_ms(void)
{
  return power_stationary_timeout_ms;
}

void power_set_low_power_threshold(uint8_t percent)
{
  if(percent > 100U)
  {
    percent = 100U;
  }

  power_low_power_threshold = percent;
}

uint8_t power_get_low_power_threshold(void)
{
  return power_low_power_threshold;
}

float power_get_reduced_temperature(void)
{
  return power_reduced_temp_c;
}

void power_set_reduced_temperature(float celsius)
{
  if((celsius >= 50.0f) && (celsius <= 400.0f))
  {
    power_reduced_temp_c = celsius;

    if(power_state == POWER_STATE_REDUCED)
    {
      (void)sm_set_setpoint(celsius);
    }
  }
}

void power_set_reduced_off_timeout_ms(uint32_t ms)
{
  if(ms >= POWER_MIN_TIMEOUT_MS)
  {
    power_reduced_off_timeout_ms = ms;
  }
}

uint32_t power_get_reduced_off_timeout_ms(void)
{
  return power_reduced_off_timeout_ms;
}

uint32_t power_get_backlight_off_timeout_ms(void)
{
  return power_backlight_off_timeout_ms;
}

void power_set_backlight_off_timeout_ms(uint32_t ms)
{
  if(ms >= POWER_MIN_TIMEOUT_MS)
  {
    power_backlight_off_timeout_ms = ms;
  }
}

uint8_t power_get_max_duty(void)
{
  return power_max_duty_pct;
}

void power_set_max_duty(uint8_t percent)
{
  if(percent < 10U)
  {
    percent = 10U;
  }

  if(percent > 100U)
  {
    percent = 100U;
  }

  power_max_duty_pct = percent;
}

void power_set_voltage_limits(float low_v, float high_v)
{
  uint16_t low_mv;
  uint16_t high_mv;

  if((low_v <= 0.0f) || (high_v <= 0.0f) || (low_v >= high_v))
  {
    return;
  }

  low_mv = (uint16_t)(low_v * 1000.0f);
  high_mv = (uint16_t)(high_v * 1000.0f);

  if((low_mv < 5000U) || (high_mv > 40000U) ||
     ((uint16_t)(low_mv + 1000U) >= high_mv))
  {
    return;
  }

  power_low_voltage_mv = low_mv;
  power_high_voltage_mv = high_mv;

  if(power_voltage_hysteresis_mv >=
     (uint16_t)((high_mv - low_mv) / 2U))
  {
    power_voltage_hysteresis_mv =
      (uint16_t)((high_mv - low_mv) / 4U);
  }
}

void power_get_voltage_limits(float *low_v, float *high_v)
{
  if(low_v != NULL)
  {
    *low_v = (float)power_low_voltage_mv / 1000.0f;
  }

  if(high_v != NULL)
  {
    *high_v = (float)power_high_voltage_mv / 1000.0f;
  }
}

void power_set_voltage_hysteresis(float volts)
{
  uint16_t hysteresis_mv;

  if(volts < 0.0f)
  {
    return;
  }

  hysteresis_mv = (uint16_t)(volts * 1000.0f);

  if(hysteresis_mv <
     (uint16_t)((power_high_voltage_mv - power_low_voltage_mv) / 2U))
  {
    power_voltage_hysteresis_mv = hysteresis_mv;
  }
}

float power_get_voltage_hysteresis(void)
{
  return (float)power_voltage_hysteresis_mv / 1000.0f;
}

void power_set_voltage_trip_time_ms(uint32_t ms)
{
  if((ms >= 10U) && (ms <= 5000U))
  {
    power_voltage_trip_ms = ms;
  }
}

uint32_t power_get_voltage_trip_time_ms(void)
{
  return power_voltage_trip_ms;
}

void power_set_voltage_recovery_time_ms(uint32_t ms)
{
  if((ms >= 100U) && (ms <= 60000U))
  {
    power_voltage_recovery_ms = ms;
  }
}

uint32_t power_get_voltage_recovery_time_ms(void)
{
  return power_voltage_recovery_ms;
}
