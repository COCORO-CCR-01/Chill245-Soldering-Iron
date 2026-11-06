#include "bootlogo.h"
#include "at32f421_conf.h"
#include "display.h"
#include "statemachine.h"
#include "wk_system.h"
#include "appcolors.h"
#include "eeprom.h"
#include <stddef.h>

#define BOOTLOGO_CMD_START   ('L')
#define BOOTLOGO_ACK_READY   ('R')
#define BOOTLOGO_ACK_META    ('M')
#define BOOTLOGO_ACK_CHUNK   (0x06U)
#define BOOTLOGO_ACK_DONE    ('K')
#define BOOTLOGO_ACK_ERROR   ('E')
#define BOOTLOGO_EOT         (0x04U)

#define BOOTLOGO_CHUNK_SIZE  (128U)
#define BOOTLOGO_UART_BAUD   (115200U)
#define BOOTLOGO_TIMEOUT_MS  (5000U)
#define BOOTLOGO_BLACK       (0x0000U)
#define BOOTLOGO_PINK        (0xFC9FU)

typedef struct
{
  uint32_t magic;
  uint16_t version;
  uint16_t width;
  uint16_t height;
  uint16_t reserved;
  uint32_t data_len;
  uint32_t data_crc;
  uint32_t header_crc;
  uint16_t palette[16];
  uint8_t pad[8];
} bootlogo_header_t;

_Static_assert(sizeof(bootlogo_header_t) == BOOTLOGO_HEADER_SIZE,
               "bootlogo header size");

static uint16_t bootlogo_line[BOOTLOGO_WIDTH];

static uint32_t bootlogo_crc32(const uint8_t *data, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t i;
  uint8_t bit;

  for(i = 0U; i < len; i++)
  {
    crc ^= data[i];
    for(bit = 0U; bit < 8U; bit++)
    {
      if((crc & 1UL) != 0U)
      {
        crc = (crc >> 1) ^ 0xEDB88320UL;
      }
      else
      {
        crc >>= 1;
      }
    }
  }

  return ~crc;
}

static uint32_t bootlogo_header_crc(const bootlogo_header_t *header)
{
  bootlogo_header_t copy = *header;
  copy.header_crc = 0U;
  return bootlogo_crc32((const uint8_t *)&copy, sizeof(copy));
}

static const uint8_t *bootlogo_data(void)
{
  return (const uint8_t *)(BOOTLOGO_FLASH_ADDR + BOOTLOGO_DATA_OFFSET);
}

uint8_t bootlogo_is_valid(void)
{
  const bootlogo_header_t *header =
    (const bootlogo_header_t *)BOOTLOGO_FLASH_ADDR;
  const uint8_t *data = bootlogo_data();

  if(header->magic != BOOTLOGO_MAGIC)
  {
    return 0U;
  }

  if((header->version != BOOTLOGO_VERSION) ||
     (header->width != BOOTLOGO_WIDTH) ||
     (header->height != BOOTLOGO_HEIGHT) ||
     (header->data_len == 0U) ||
     (header->data_len > BOOTLOGO_MAX_DATA))
  {
    return 0U;
  }

  if(header->header_crc != bootlogo_header_crc(header))
  {
    return 0U;
  }

  if(header->data_crc != bootlogo_crc32(data, header->data_len))
  {
    return 0U;
  }

  return 1U;
}

typedef struct
{
  const uint8_t *src;
  uint16_t len;
  uint16_t pos;
  uint16_t remain;
  uint8_t repeat;
  uint8_t repeat_mode;
  uint8_t have_low;
  uint8_t low_nibble;
} bootlogo_pb_t;

static uint8_t bootlogo_pb_byte(bootlogo_pb_t *pb)
{
  uint8_t control;

  while(pb->remain == 0U)
  {
    if(pb->pos >= pb->len)
    {
      return 0U;
    }

    control = pb->src[pb->pos++];

    if(control < 128U)
    {
      pb->remain = (uint16_t)(control + 1U);
      pb->repeat_mode = 0U;
    }
    else if(control > 128U)
    {
      pb->remain = (uint16_t)(257U - control);
      pb->repeat_mode = 1U;
      if(pb->pos >= pb->len)
      {
        return 0U;
      }
      pb->repeat = pb->src[pb->pos++];
    }
  }

  pb->remain--;

  if(pb->repeat_mode != 0U)
  {
    return pb->repeat;
  }

  if(pb->pos >= pb->len)
  {
    return 0U;
  }

  return pb->src[pb->pos++];
}

static uint8_t bootlogo_pb_nibble(bootlogo_pb_t *pb)
{
  uint8_t value;

  if(pb->have_low != 0U)
  {
    pb->have_low = 0U;
    return pb->low_nibble;
  }

  value = bootlogo_pb_byte(pb);
  pb->low_nibble = (uint8_t)(value & 0x0FU);
  pb->have_low = 1U;
  return (uint8_t)(value >> 4);
}

static uint16_t bootlogo_dim_color(uint16_t color)
{
  uint16_t r = (uint16_t)((color >> 11) & 0x1FU);
  uint16_t g = (uint16_t)((color >> 5) & 0x3FU);
  uint16_t b = (uint16_t)(color & 0x1FU);

  r = (uint16_t)((r * 10U) / 16U);
  g = (uint16_t)((g * 10U) / 16U);
  b = (uint16_t)((b * 10U) / 16U);

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void bootlogo_render(uint8_t dimmed)
{
  const bootlogo_header_t *header =
    (const bootlogo_header_t *)BOOTLOGO_FLASH_ADDR;
  bootlogo_pb_t pb;
  uint16_t x;
  uint16_t y;
  uint16_t color;
  uint8_t index;

  pb.src = bootlogo_data();
  pb.len = (uint16_t)header->data_len;
  pb.pos = 0U;
  pb.remain = 0U;
  pb.repeat = 0U;
  pb.repeat_mode = 0U;
  pb.have_low = 0U;
  pb.low_nibble = 0U;

  display_set_window(0U, 0U, BOOTLOGO_WIDTH, BOOTLOGO_HEIGHT);
  display_begin_pixel_stream();

  for(y = 0U; y < BOOTLOGO_HEIGHT; y++)
  {
    for(x = 0U; x < BOOTLOGO_WIDTH; x++)
    {
      index = bootlogo_pb_nibble(&pb);
      color = header->palette[index & 0x0FU];
      bootlogo_line[x] = (dimmed != 0U) ?
                         bootlogo_dim_color(color) : color;
    }

    display_write_pixel_stream_raw(bootlogo_line, BOOTLOGO_WIDTH);
  }

  display_end_pixel_stream();
}

void bootlogo_render_dimmed(void)
{
  if(bootlogo_is_valid() != 0U)
  {
    bootlogo_render(1U);
  }
}

uint8_t bootlogo_copy_image(uint8_t *packed, uint16_t packed_len,
                            uint16_t palette[16])
{
  const bootlogo_header_t *header =
    (const bootlogo_header_t *)BOOTLOGO_FLASH_ADDR;
  bootlogo_pb_t pb;
  uint16_t i;
  uint8_t high;
  uint8_t low;

  if((packed == 0) || (palette == 0) ||
     (packed_len < 6400U) || (bootlogo_is_valid() == 0U))
  {
    return 0U;
  }

  pb.src = bootlogo_data();
  pb.len = (uint16_t)header->data_len;
  pb.pos = 0U;
  pb.remain = 0U;
  pb.repeat = 0U;
  pb.repeat_mode = 0U;
  pb.have_low = 0U;
  pb.low_nibble = 0U;

  for(i = 0U; i < 6400U; i++)
  {
    high = bootlogo_pb_nibble(&pb);
    low = bootlogo_pb_nibble(&pb);
    packed[i] = (uint8_t)((high << 4) | (low & 0x0FU));
  }

  for(i = 0U; i < 16U; i++)
  {
    palette[i] = header->palette[i];
  }

  return 1U;
}

void bootlogo_boot(float delay_s)
{
  uint32_t delay_ms;
  uint32_t elapsed;

  if((delay_s <= 0.0f) || (bootlogo_is_valid() == 0U))
  {
    return;
  }

  delay_ms = (uint32_t)((delay_s * 1000.0f) + 0.5f);
  if(delay_ms > 10000U)
  {
    delay_ms = 10000U;
  }

  bootlogo_render(0U);

  for(elapsed = 0U; elapsed < delay_ms; elapsed += 50U)
  {
    wk_delay_ms(50U);
    wdt_counter_reload();
  }
}

/* ------------------------------ UART / upload ------------------------------ */

static void bootlogo_uart_init(void)
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

  usart_init(USART1, BOOTLOGO_UART_BAUD, USART_DATA_8BITS,
             USART_STOP_1_BIT);
  usart_transmitter_enable(USART1, TRUE);
  usart_receiver_enable(USART1, TRUE);
  usart_enable(USART1, TRUE);
}

static void bootlogo_uart_send(uint8_t value)
{
  uint32_t start = sm_get_uptime_ms();

  while(usart_flag_get(USART1, USART_TDC_FLAG) == RESET)
  {
    if((uint32_t)(sm_get_uptime_ms() - start) > 20U)
    {
      break;
    }
  }

  usart_data_transmit(USART1, value);
}

static uint8_t bootlogo_uart_read(uint8_t *value, uint32_t timeout_ms)
{
  uint32_t start = sm_get_uptime_ms();

  while(usart_flag_get(USART1, USART_RDBF_FLAG) == RESET)
  {
    if((uint32_t)(sm_get_uptime_ms() - start) > timeout_ms)
    {
      return 0U;
    }
  }

  *value = (uint8_t)usart_data_receive(USART1);
  return 1U;
}

static uint8_t bootlogo_flash_erase(void)
{
  uint32_t address;
  flash_status_type status;

  flash_unlock();
  flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);

  for(address = BOOTLOGO_FLASH_ADDR;
      address < (BOOTLOGO_FLASH_ADDR + BOOTLOGO_REGION_SIZE);
      address += 1024UL)
  {
    status = flash_sector_erase(address);
    if(status != FLASH_OPERATE_DONE)
    {
      flash_lock();
      return 0U;
    }
  }

  flash_lock();
  return 1U;
}

static uint8_t bootlogo_flash_write(uint32_t address,
                                    const uint8_t *data,
                                    uint32_t len)
{
  uint32_t i;
  uint32_t word;
  flash_status_type status;

  flash_unlock();
  flash_flag_clear(FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);

  for(i = 0U; i < len; i += 4U)
  {
    word = (uint32_t)data[i] |
           ((uint32_t)data[i + 1U] << 8) |
           ((uint32_t)data[i + 2U] << 16) |
           ((uint32_t)data[i + 3U] << 24);

    status = flash_word_program(address + i, word);
    if(status != FLASH_OPERATE_DONE)
    {
      flash_lock();
      return 0U;
    }
  }

  flash_lock();
  return 1U;
}

static uint32_t bootlogo_read_le32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

static uint8_t bootlogo_upload(uint8_t with_colors)
{
  uint8_t meta[50];
  uint8_t chunk[BOOTLOGO_CHUNK_SIZE];
  uint8_t value;
  uint8_t meta_len;
  uint8_t palette_offset;
  uint32_t data_len;
  uint32_t expected_crc;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t offset;
  uint32_t i;
  uint32_t bit;
  uint16_t palette[16];
  uint16_t colors[APP_COLOR_COUNT];
  bootlogo_header_t header;
  uint8_t heater_was_enabled;

  meta_len = (with_colors != 0U) ? 50U : 40U;
  palette_offset = (with_colors != 0U) ? 18U : 8U;

  heater_was_enabled = sm_is_heater_enabled();
  if(heater_was_enabled != 0U)
  {
    (void)sm_set_heater_enabled(0U);
  }

  if(bootlogo_flash_erase() == 0U)
  {
    if(heater_was_enabled != 0U)
    {
      (void)sm_set_heater_enabled(1U);
    }
    bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
    return 0U;
  }

  bootlogo_uart_send(BOOTLOGO_ACK_READY);

  for(i = 0U; i < meta_len; i++)
  {
    if(bootlogo_uart_read(&meta[i], BOOTLOGO_TIMEOUT_MS) == 0U)
    {
      if(heater_was_enabled != 0U)
      {
        (void)sm_set_heater_enabled(1U);
      }
      bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
      return 0U;
    }
  }

  data_len = bootlogo_read_le32(&meta[0]);
  expected_crc = bootlogo_read_le32(&meta[4]);

  if((data_len == 0U) || (data_len > BOOTLOGO_MAX_DATA))
  {
    if(heater_was_enabled != 0U)
    {
      (void)sm_set_heater_enabled(1U);
    }
    bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
    return 0U;
  }

  if(with_colors != 0U)
  {
    for(i = 0U; i < APP_COLOR_COUNT; i++)
    {
      colors[i] = (uint16_t)meta[8U + (i * 2U)] |
                  ((uint16_t)meta[9U + (i * 2U)] << 8);
    }
  }

  for(i = 0U; i < 16U; i++)
  {
    palette[i] = (uint16_t)meta[palette_offset + (i * 2U)] |
                 ((uint16_t)meta[palette_offset + 1U + (i * 2U)] << 8);
  }

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, DISPLAY_HEIGHT, BOOTLOGO_BLACK);
  display_draw_string(20U, 36U, (with_colors != 0U) ? "WALLPAPER" : "LOGO RX",
                      BOOTLOGO_PINK, BOOTLOGO_BLACK, 2U);

  bootlogo_uart_send(BOOTLOGO_ACK_META);

  for(offset = 0U; offset < data_len; offset += BOOTLOGO_CHUNK_SIZE)
  {
    for(i = 0U; i < BOOTLOGO_CHUNK_SIZE; i++)
    {
      if(bootlogo_uart_read(&chunk[i], BOOTLOGO_TIMEOUT_MS) == 0U)
      {
        bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
        goto upload_fail;
      }
    }

    for(i = 0U; i < BOOTLOGO_CHUNK_SIZE; i++)
    {
      if((offset + i) < data_len)
      {
        crc ^= chunk[i];
        for(bit = 0U; bit < 8U; bit++)
        {
          crc = ((crc & 1UL) != 0U) ?
                ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
      }
    }

    if(bootlogo_flash_write(
         BOOTLOGO_FLASH_ADDR + BOOTLOGO_DATA_OFFSET + offset,
         chunk, BOOTLOGO_CHUNK_SIZE) == 0U)
    {
      bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
      goto upload_fail;
    }

    bootlogo_uart_send(BOOTLOGO_ACK_CHUNK);
  }

  if(bootlogo_uart_read(&value, BOOTLOGO_TIMEOUT_MS) == 0U)
  {
    bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
    goto upload_fail;
  }

  if(value != BOOTLOGO_EOT)
  {
    bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
    goto upload_fail;
  }

  crc = ~crc;
  if(crc != expected_crc)
  {
    bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
    goto upload_fail;
  }

  header.magic = BOOTLOGO_MAGIC;
  header.version = BOOTLOGO_VERSION;
  header.width = BOOTLOGO_WIDTH;
  header.height = BOOTLOGO_HEIGHT;
  header.reserved = 0U;
  header.data_len = data_len;
  header.data_crc = expected_crc;
  for(i = 0U; i < 16U; i++)
  {
    header.palette[i] = palette[i];
  }
  for(i = 0U; i < sizeof(header.pad); i++)
  {
    header.pad[i] = 0U;
  }
  header.header_crc = 0U;
  header.header_crc = bootlogo_header_crc(&header);

  if(bootlogo_flash_write(BOOTLOGO_FLASH_ADDR,
                          (const uint8_t *)&header,
                          sizeof(header)) == 0U)
  {
    bootlogo_uart_send(BOOTLOGO_ACK_ERROR);
    goto upload_fail;
  }

  if(with_colors != 0U)
  {
    appcolors_set(colors);
    for(i = 0U; i < APP_COLOR_COUNT; i++)
    {
      g_eeprom_params.app_colors[i] = colors[i];
    }
    (void)eeprom_save();
  }

  if(heater_was_enabled != 0U)
  {
    (void)sm_set_heater_enabled(1U);
  }

  bootlogo_uart_send(BOOTLOGO_ACK_DONE);
  return 1U;

upload_fail:
  if(heater_was_enabled != 0U)
  {
    (void)sm_set_heater_enabled(1U);
  }
  return 0U;
}

void bootlogo_init(void)
{
  bootlogo_uart_init();
}

uint8_t bootlogo_poll(void)
{
  uint8_t value;

  if(usart_flag_get(USART1, USART_RDBF_FLAG) == RESET)
  {
    return 0U;
  }

  value = (uint8_t)usart_data_receive(USART1);
  if((value != BOOTLOGO_CMD_START) && (value != 'W'))
  {
    return 0U;
  }

  return (bootlogo_upload((value == 'W') ? 1U : 0U) != 0U) ? 1U : 2U;
}
