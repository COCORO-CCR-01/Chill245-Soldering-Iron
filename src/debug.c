/**
  **************************************************************************
  * @file     debug.c
  * @brief    串口调试控制台
  **************************************************************************
  * 硬件：
  *   PA9  -> USART1_TX
  *   PA10 -> USART1_RX
  *   115200, 8N1
  *
  * 功能：
  *   - 命令控制台：help / list / raw / temp / stream / echo ...
  *   - 注册任意无参数函数，通过串口按名称触发
  *   - 注册带参数命令
  *   - 周期输出 ADC 原始值和温度
  **************************************************************************
  */

#include "at32f421_conf.h"
#include "debug.h"
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

/* ============================ 调试串口参数 ============================ */
#define DEBUG_USART                     USART1
#define DEBUG_GPIO_PORT                 GPIOA
#define DEBUG_TX_PIN                    GPIO_PINS_9
#define DEBUG_RX_PIN                    GPIO_PINS_10
#define DEBUG_BAUD_RATE                 (115200U)

#define DEBUG_RX_BUFFER_SIZE            (128U)
#define DEBUG_TX_BUFFER_SIZE            (1024U)
#define DEBUG_LINE_SIZE                 (96U)
#define DEBUG_MAX_TOKENS                (8U)
#define DEBUG_MAX_TRIGGERS              (16U)
#define DEBUG_MAX_COMMANDS              (8U)

#define DEBUG_STREAM_DEFAULT_MS         (500U)
#define DEBUG_STREAM_MIN_MS             (50U)
#define DEBUG_STREAM_MAX_MS             (10000U)

/* ============================ 内部类型 ============================ */
typedef struct
{
  const char *name;
  const char *help;
  debug_trigger_fn_t fn;
} debug_trigger_entry_t;

typedef struct
{
  const char *name;
  const char *help;
  debug_command_fn_t fn;
} debug_command_entry_t;

/* ============================ 内部状态 ============================ */
static debug_trigger_entry_t debug_triggers[DEBUG_MAX_TRIGGERS];
static uint8_t debug_trigger_count;

static debug_command_entry_t debug_commands[DEBUG_MAX_COMMANDS];
static uint8_t debug_command_count;

static volatile uint8_t debug_rx_buffer[DEBUG_RX_BUFFER_SIZE];
static volatile uint16_t debug_rx_head;
static volatile uint16_t debug_rx_tail;

static volatile uint8_t debug_tx_buffer[DEBUG_TX_BUFFER_SIZE];
static volatile uint16_t debug_tx_head;
static volatile uint16_t debug_tx_tail;
static volatile uint8_t debug_tx_busy;

static char debug_line[DEBUG_LINE_SIZE];
static uint16_t debug_line_length;
static uint8_t debug_discard_line;
static uint8_t debug_last_cr;

static uint8_t debug_echo_enabled;
static uint8_t debug_stream_enabled;
static uint8_t debug_initialized;
static uint32_t debug_stream_period_ms;
static uint32_t debug_stream_last_ms;
static uint32_t debug_now_ms;

static const adc_raw_data_t *debug_adc_raw;
static const temp_data_t *debug_temp_data;

/* ============================ 内部函数声明 ============================ */
static void debug_poll(void);
static void debug_execute_line(char *line);
static void debug_print_raw(void);
static void debug_print_temp(void);
static void debug_print_help(void);
static void debug_list_triggers(void);
static void debug_list_commands(void);
static void debug_handle_stream(int argc, char **argv);
static void debug_handle_echo(int argc, char **argv);
static void debug_prompt(void);
static debug_trigger_entry_t *debug_find_trigger(const char *name);
static debug_command_entry_t *debug_find_command(const char *name);
static uint8_t debug_streq(const char *a, const char *b);
static uint32_t debug_parse_uint(const char *text);

/* ============================ 串口发送 ============================ */

/**
  * @brief  启动 TX 中断
  */
static void debug_tx_start(void)
{
  if((debug_tx_busy == 0U) && (debug_tx_tail != debug_tx_head))
  {
    debug_tx_busy = 1U;
    usart_interrupt_enable(DEBUG_USART, USART_TDBE_INT, TRUE);
  }
}

/**
  * @brief  发送一个字节
  * @note   非阻塞：缓冲区满时丢弃本次字节，保证控制周期不被串口拖慢。
  */
static void debug_putc(char ch)
{
  uint16_t next;

  if(debug_initialized == 0U)
  {
    return;
  }

  next = (uint16_t)((debug_tx_head + 1U) % DEBUG_TX_BUFFER_SIZE);

  if(next == debug_tx_tail)
  {
    return;
  }

  debug_tx_buffer[debug_tx_head] = (uint8_t)ch;
  debug_tx_head = next;
  debug_tx_start();
}

void debug_write(const char *text)
{
  if(text == NULL)
  {
    return;
  }

  while(*text != '\0')
  {
    debug_putc(*text);
    text++;
  }
}

void debug_printf(const char *fmt, ...)
{
  char buffer[128];
  va_list args;
  int length;
  int i;

  if(fmt == NULL)
  {
    return;
  }

  va_start(args, fmt);
  length = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  if(length <= 0)
  {
    return;
  }

  if((uint32_t)length >= sizeof(buffer))
  {
    length = (int)sizeof(buffer) - 1;
  }

  for(i = 0; i < length; i++)
  {
    debug_putc(buffer[i]);
  }
}

void debug_write_float(float value, uint8_t decimals)
{
  int32_t scale = 1;
  int32_t scaled;
  int32_t whole;
  int32_t fraction;
  uint8_t i;

  if(decimals > 3U)
  {
    decimals = 3U;
  }

  for(i = 0U; i < decimals; i++)
  {
    scale *= 10;
  }

  if(value < 0.0f)
  {
    debug_putc('-');
    value = -value;
  }

  scaled = (int32_t)(value * (float)scale + 0.5f);
  whole = scaled / scale;
  fraction = scaled % scale;

  debug_printf("%d", (int)whole);

  if(decimals > 0U)
  {
    debug_putc('.');

    if((decimals == 3U) && (fraction < 100))
    {
      debug_putc('0');
    }

    if((decimals >= 2U) && (fraction < 10))
    {
      debug_putc('0');
    }

    debug_printf("%d", (int)fraction);
  }
}

/* ============================ 字符串/解析 ============================ */

static uint8_t debug_streq(const char *a, const char *b)
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

static uint32_t debug_parse_uint(const char *text)
{
  uint32_t value = 0U;

  if(text == NULL)
  {
    return 0U;
  }

  while((*text >= '0') && (*text <= '9'))
  {
    value = (value * 10U) + (uint32_t)(*text - '0');
    text++;
  }

  return value;
}

/* ============================ 注册表 ============================ */

static debug_trigger_entry_t *debug_find_trigger(const char *name)
{
  uint8_t i;

  if(name == NULL)
  {
    return NULL;
  }

  for(i = 0U; i < debug_trigger_count; i++)
  {
    if(debug_streq(debug_triggers[i].name, name) != 0U)
    {
      return &debug_triggers[i];
    }
  }

  return NULL;
}

static debug_command_entry_t *debug_find_command(const char *name)
{
  uint8_t i;

  if(name == NULL)
  {
    return NULL;
  }

  for(i = 0U; i < debug_command_count; i++)
  {
    if(debug_streq(debug_commands[i].name, name) != 0U)
    {
      return &debug_commands[i];
    }
  }

  return NULL;
}

uint8_t debug_register_trigger(const char *name,
                               const char *help,
                               debug_trigger_fn_t fn)
{
  if((name == NULL) || (name[0] == '\0') || (fn == NULL))
  {
    return 0U;
  }

  if(debug_trigger_count >= DEBUG_MAX_TRIGGERS)
  {
    return 0U;
  }

  if(debug_find_trigger(name) != NULL)
  {
    return 0U;
  }

  debug_triggers[debug_trigger_count].name = name;
  debug_triggers[debug_trigger_count].help = (help != NULL) ? help : "";
  debug_triggers[debug_trigger_count].fn = fn;
  debug_trigger_count++;

  return 1U;
}

uint8_t debug_register_command(const char *name,
                               const char *help,
                               debug_command_fn_t fn)
{
  if((name == NULL) || (name[0] == '\0') || (fn == NULL))
  {
    return 0U;
  }

  if(debug_command_count >= DEBUG_MAX_COMMANDS)
  {
    return 0U;
  }

  if(debug_find_command(name) != NULL)
  {
    return 0U;
  }

  debug_commands[debug_command_count].name = name;
  debug_commands[debug_command_count].help = (help != NULL) ? help : "";
  debug_commands[debug_command_count].fn = fn;
  debug_command_count++;

  return 1U;
}

uint8_t debug_run_trigger(const char *name)
{
  debug_trigger_entry_t *entry = debug_find_trigger(name);

  if(entry == NULL)
  {
    return 0U;
  }

  entry->fn();
  return 1U;
}

/* ============================ 数据输出 ============================ */

static void debug_print_raw(void)
{
  if((debug_adc_raw == NULL) || (debug_temp_data == NULL))
  {
    debug_write("raw: no data\r\n");
    return;
  }

  debug_printf("ADC ntc=%u tc=%u vin=%u | room=",
               (unsigned)debug_adc_raw->ntc,
               (unsigned)debug_adc_raw->thermocouple,
               (unsigned)debug_adc_raw->vin);
  debug_write_float(debug_temp_data->room_c, 2);
  debug_write("C tip=");
  debug_write_float(debug_temp_data->tip_c, 2);
  debug_printf("C valid=%u adc_valid=%u\r\n",
               (unsigned)debug_temp_data->valid,
               (unsigned)debug_adc_raw->valid);
}

static void debug_print_temp(void)
{
  if(debug_temp_data == NULL)
  {
    debug_write("temp: no data\r\n");
    return;
  }

  debug_write("room=");
  debug_write_float(debug_temp_data->room_c, 2);
  debug_write("C tip=");
  debug_write_float(debug_temp_data->tip_c, 2);
  debug_printf("C valid=%u\r\n", (unsigned)debug_temp_data->valid);
}

static void debug_list_triggers(void)
{
  uint8_t i;

  debug_printf("triggers (%u):\r\n", (unsigned)debug_trigger_count);

  if(debug_trigger_count == 0U)
  {
    debug_write("  (none)\r\n");
    return;
  }

  for(i = 0U; i < debug_trigger_count; i++)
  {
    debug_printf("  %s - %s\r\n", debug_triggers[i].name, debug_triggers[i].help);
  }
}

static void debug_list_commands(void)
{
  uint8_t i;

  debug_printf("commands (%u):\r\n", (unsigned)debug_command_count);

  if(debug_command_count == 0U)
  {
    debug_write("  (none)\r\n");
    return;
  }

  for(i = 0U; i < debug_command_count; i++)
  {
    debug_printf("  %s - %s\r\n", debug_commands[i].name, debug_commands[i].help);
  }
}

static void debug_print_help(void)
{
  debug_write("built-in commands:\r\n");
  debug_write("  help|?                  this help\r\n");
  debug_write("  list|triggers           list registered triggers\r\n");
  debug_write("  commands                list registered commands\r\n");
  debug_write("  raw|adc                 print ADC raw + temperature\r\n");
  debug_write("  temp                    print temperature\r\n");
  debug_write("  stream on [ms]|off      periodic raw data\r\n");
  debug_write("  console on [ms]|off     alias of stream\r\n");
  debug_write("  echo on|off             local echo\r\n");
  debug_write("  uptime                  print millisecond timestamp\r\n");
  debug_write("  run <trigger>           run registered trigger\r\n");
  debug_write("use 'list' / 'commands' for registered items.\r\n");
}

static void debug_handle_stream(int argc, char **argv)
{
  uint32_t period;

  if(argc < 2)
  {
    debug_printf("stream=%s period=%u ms\r\n",
                 (debug_stream_enabled != 0U) ? "on" : "off",
                 (unsigned)debug_stream_period_ms);
    return;
  }

  if(debug_streq(argv[1], "on") != 0U)
  {
    period = (argc >= 3) ? debug_parse_uint(argv[2]) : DEBUG_STREAM_DEFAULT_MS;

    if(period == 0U)
    {
      period = DEBUG_STREAM_DEFAULT_MS;
    }

    if(period < DEBUG_STREAM_MIN_MS)
    {
      period = DEBUG_STREAM_MIN_MS;
    }

    if(period > DEBUG_STREAM_MAX_MS)
    {
      period = DEBUG_STREAM_MAX_MS;
    }

    debug_stream_period_ms = period;
    debug_stream_enabled = 1U;
    debug_stream_last_ms = debug_now_ms;

    debug_printf("stream on, period=%u ms\r\n", (unsigned)period);
    debug_print_raw();
  }
  else if(debug_streq(argv[1], "off") != 0U)
  {
    debug_stream_enabled = 0U;
    debug_write("stream off\r\n");
  }
  else
  {
    debug_write("usage: stream on [ms] | stream off\r\n");
  }
}

static void debug_handle_echo(int argc, char **argv)
{
  if(argc < 2)
  {
    debug_printf("echo=%s\r\n", (debug_echo_enabled != 0U) ? "on" : "off");
    return;
  }

  if(debug_streq(argv[1], "on") != 0U)
  {
    debug_echo_enabled = 1U;
    debug_write("echo on\r\n");
  }
  else if(debug_streq(argv[1], "off") != 0U)
  {
    debug_echo_enabled = 0U;
  }
  else
  {
    debug_write("usage: echo on|off\r\n");
  }
}

/* ============================ 命令解析 ============================ */

static void debug_execute_line(char *line)
{
  char *argv[DEBUG_MAX_TOKENS];
  int argc = 0;
  char *p = line;
  debug_command_entry_t *command;
  debug_trigger_entry_t *trigger;

  while((*p != '\0') && (argc < (int)DEBUG_MAX_TOKENS))
  {
    while((*p == ' ') || (*p == '\t'))
    {
      *p = '\0';
      p++;
    }

    if(*p == '\0')
    {
      break;
    }

    argv[argc] = p;
    argc++;

    while((*p != '\0') && (*p != ' ') && (*p != '\t'))
    {
      p++;
    }

    if(*p != '\0')
    {
      *p = '\0';
      p++;
    }
  }

  if(argc == 0)
  {
    return;
  }

  if((debug_streq(argv[0], "help") != 0U) || (debug_streq(argv[0], "?") != 0U))
  {
    debug_print_help();
  }
  else if((debug_streq(argv[0], "list") != 0U) ||
          (debug_streq(argv[0], "triggers") != 0U))
  {
    debug_list_triggers();
  }
  else if(debug_streq(argv[0], "commands") != 0U)
  {
    debug_list_commands();
  }
  else if((debug_streq(argv[0], "raw") != 0U) ||
          (debug_streq(argv[0], "adc") != 0U))
  {
    debug_print_raw();
  }
  else if(debug_streq(argv[0], "temp") != 0U)
  {
    debug_print_temp();
  }
  else if((debug_streq(argv[0], "stream") != 0U) ||
          (debug_streq(argv[0], "console") != 0U))
  {
    debug_handle_stream(argc, argv);
  }
  else if(debug_streq(argv[0], "echo") != 0U)
  {
    debug_handle_echo(argc, argv);
  }
  else if(debug_streq(argv[0], "uptime") != 0U)
  {
    debug_printf("uptime=%u ms\r\n", (unsigned)debug_now_ms);
  }
  else if((debug_streq(argv[0], "run") != 0U) ||
          (debug_streq(argv[0], "trig") != 0U))
  {
    if(argc < 2)
    {
      debug_write("usage: run <trigger>\r\n");
    }
    else if(debug_run_trigger(argv[1]) == 0U)
    {
      debug_printf("unknown trigger: %s\r\n", argv[1]);
    }
  }
  else
  {
    command = debug_find_command(argv[0]);

    if(command != NULL)
    {
      command->fn(argc, argv);
    }
    else
    {
      trigger = debug_find_trigger(argv[0]);

      if(trigger != NULL)
      {
        trigger->fn();
      }
      else
      {
        debug_printf("unknown command: %s (type help)\r\n", argv[0]);
      }
    }
  }
}

static void debug_prompt(void)
{
  if((debug_echo_enabled != 0U) && (debug_stream_enabled == 0U))
  {
    debug_write("> ");
  }
}

static void debug_poll(void)
{
  char ch;

  while(debug_rx_tail != debug_rx_head)
  {
    ch = (char)debug_rx_buffer[debug_rx_tail];
    debug_rx_tail = (uint16_t)((debug_rx_tail + 1U) % DEBUG_RX_BUFFER_SIZE);

    if((ch != '\r') && (ch != '\n'))
    {
      debug_last_cr = 0U;
    }

    if((ch == '\r') || (ch == '\n'))
    {
      if((ch == '\n') && (debug_last_cr != 0U))
      {
        debug_last_cr = 0U;
        continue;
      }

      debug_last_cr = (ch == '\r') ? 1U : 0U;

      if(debug_discard_line != 0U)
      {
        debug_discard_line = 0U;
        debug_line_length = 0U;
        debug_write("\r\n");
        debug_prompt();
        continue;
      }

      if(debug_line_length > 0U)
      {
        debug_line[debug_line_length] = '\0';
        debug_line_length = 0U;

        if(debug_echo_enabled != 0U)
        {
          debug_write("\r\n");
        }

        debug_execute_line(debug_line);
        debug_prompt();
      }
      else if(debug_echo_enabled != 0U)
      {
        debug_write("\r\n");
        debug_prompt();
      }
    }
    else if((ch == '\b') || (ch == 0x7F))
    {
      debug_last_cr = 0U;

      if((debug_line_length > 0U) && (debug_discard_line == 0U))
      {
        debug_line_length--;

        if(debug_echo_enabled != 0U)
        {
          debug_write("\b \b");
        }
      }
    }
    else if((ch >= 0x20) && (ch <= 0x7E))
    {
      debug_last_cr = 0U;

      if(debug_discard_line != 0U)
      {
        continue;
      }

      if(debug_line_length < (DEBUG_LINE_SIZE - 1U))
      {
        debug_line[debug_line_length] = ch;
        debug_line_length++;

        if(debug_echo_enabled != 0U)
        {
          debug_putc(ch);
        }
      }
      else
      {
        debug_discard_line = 1U;
        debug_line_length = 0U;
        debug_write("\r\nline too long\r\n");
      }
    }
  }
}

/* ============================ USART1 中断 ============================ */

void USART1_IRQHandler(void)
{
  if(usart_flag_get(DEBUG_USART, USART_RDBF_FLAG) != RESET)
  {
    uint8_t ch = (uint8_t)usart_data_receive(DEBUG_USART);
    uint16_t next = (uint16_t)((debug_rx_head + 1U) % DEBUG_RX_BUFFER_SIZE);

    if(next != debug_rx_tail)
    {
      debug_rx_buffer[debug_rx_head] = ch;
      debug_rx_head = next;
    }
  }

  if(usart_flag_get(DEBUG_USART, USART_TDBE_FLAG) != RESET)
  {
    if((debug_tx_busy != 0U) && (debug_tx_tail != debug_tx_head))
    {
      usart_data_transmit(DEBUG_USART, debug_tx_buffer[debug_tx_tail]);
      debug_tx_tail = (uint16_t)((debug_tx_tail + 1U) % DEBUG_TX_BUFFER_SIZE);
    }
    else
    {
      usart_interrupt_enable(DEBUG_USART, USART_TDBE_INT, FALSE);
      debug_tx_busy = 0U;
    }
  }
}

/* ============================ 对外函数 ============================ */

void debug_init(void)
{
  gpio_init_type gpio_init_struct;

  debug_rx_head = 0U;
  debug_rx_tail = 0U;
  debug_tx_head = 0U;
  debug_tx_tail = 0U;
  debug_tx_busy = 0U;
  debug_line_length = 0U;
  debug_discard_line = 0U;
  debug_last_cr = 0U;
  debug_echo_enabled = 1U;
  debug_stream_enabled = 0U;
  debug_initialized = 0U;
  debug_stream_period_ms = DEBUG_STREAM_DEFAULT_MS;
  debug_stream_last_ms = 0U;
  debug_now_ms = 0U;
  debug_adc_raw = NULL;
  debug_temp_data = NULL;
  debug_trigger_count = 0U;
  debug_command_count = 0U;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&gpio_init_struct);

  /* PA9 TX */
  gpio_init_struct.gpio_pins = DEBUG_TX_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(DEBUG_GPIO_PORT, &gpio_init_struct);

  /* PA10 RX */
  gpio_init_struct.gpio_pins = DEBUG_RX_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(DEBUG_GPIO_PORT, &gpio_init_struct);

  gpio_pin_mux_config(DEBUG_GPIO_PORT, GPIO_PINS_SOURCE9, GPIO_MUX_1);
  gpio_pin_mux_config(DEBUG_GPIO_PORT, GPIO_PINS_SOURCE10, GPIO_MUX_1);

  nvic_irq_enable(USART1_IRQn, 3, 0);

  usart_init(DEBUG_USART, DEBUG_BAUD_RATE, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_parity_selection_config(DEBUG_USART, USART_PARITY_NONE);
  usart_transmitter_enable(DEBUG_USART, TRUE);
  usart_receiver_enable(DEBUG_USART, TRUE);
  usart_hardware_flow_control_set(DEBUG_USART, USART_HARDWARE_FLOW_NONE);
  usart_interrupt_enable(DEBUG_USART, USART_RDBF_INT, TRUE);
  usart_enable(DEBUG_USART, TRUE);

  debug_initialized = 1U;

  debug_write("\r\nJBC245 debug console\r\n");
  debug_write("PA9=TX PA10=RX 115200 8N1\r\n");
  debug_write("Type 'help' for commands.\r\n");
  debug_prompt();
}

void debug_update(const adc_raw_data_t *adc_raw,
                  const temp_data_t *temp_data,
                  uint32_t now_ms)
{
  debug_adc_raw = adc_raw;
  debug_temp_data = temp_data;
  debug_now_ms = now_ms;

  debug_poll();

  if(debug_stream_enabled != 0U)
  {
    if((uint32_t)(now_ms - debug_stream_last_ms) >= debug_stream_period_ms)
    {
      debug_stream_last_ms = now_ms;
      debug_print_raw();
    }
  }
}
