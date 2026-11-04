/*
 * TODO(display): 屏幕仍有花屏，当前 SPI 已降至 7.5 MHz。
 * 待确认面板 offset/MADCTL、排线信号完整性和 ST7735S 初始化时序后再继续。
 */

/**
  **************************************************************************
  * @file     display.c
  * @brief    ST7735S 160x80 横屏 4-line SPI 显示驱动
  **************************************************************************
  * 硬件连接：
  *   PA7  -> ST7735S SDA / MOSI   (SPI1_MOSI, MUX_0)
  *   PA5  -> ST7735S SCL / SCLK   (SPI1_SCK,  MUX_0)
  *   PB0  -> ST7735S RS / DC
  *   PA4  -> ST7735S CS
  *   PA3  -> 背光控制
  *
  * 4-line SPI：
  *   CS 低有效，DC=0 命令，DC=1 数据，MSB first，SPI mode 0。
  *   SPI1 时钟 120 MHz / 8 = 15 MHz。
  *
  * 80x160 面板是 ST7735S 的裁剪屏，横屏使用：
  *   X offset = 1, Y offset = 26
  *   MADCTL = 0xA8（MY|MV|BGR，横屏 160x80）
  * 如果实际显示偏移或镜像，修改下方 DISPLAY_X_OFFSET /
  * DISPLAY_Y_OFFSET / DISPLAY_MADCTL。
  **************************************************************************
  */

#include "display.h"
#include "at32f421_conf.h"
#include "wk_system.h"
#include <stddef.h>

/* ============================ 引脚定义 ============================ */
#define DISPLAY_SPI                     SPI1
#define DISPLAY_SPI_CLOCK               CRM_SPI1_PERIPH_CLOCK
/* 硬件 SPI：937.5 kHz，使用合法的 DIV_128 编码 */
#define DISPLAY_SPI_DIV                 SPI_MCLK_DIV_16

/*
 * 参考 ST7735S-master 的硬件 SPI 写法：命令/数据之间释放 CS。
 */
#define DISPLAY_USE_BITBANG             (0U)

#define DISPLAY_GPIO_PORT               GPIOA
#define DISPLAY_CS_PIN                  GPIO_PINS_4
#define DISPLAY_SCK_PIN                 GPIO_PINS_5
#define DISPLAY_MOSI_PIN                GPIO_PINS_7
#define DISPLAY_BL_PIN                  GPIO_PINS_3

#define DISPLAY_DC_PORT                 GPIOB
#define DISPLAY_DC_PIN                  GPIO_PINS_0

#define DISPLAY_PIN_MUX                 GPIO_MUX_0
#define DISPLAY_SCK_SOURCE              GPIO_PINS_SOURCE5
#define DISPLAY_MOSI_SOURCE             GPIO_PINS_SOURCE7

/* ============================ 面板参数 ============================ */
#define DISPLAY_X_OFFSET                (-2)
#define DISPLAY_Y_OFFSET                (24U)

/* MADCTL: MY | MV，横屏 160x80，RGB 顺序 */
#define DISPLAY_MADCTL_VALUE            (0xA0U)

/* 面板默认需要反显；如果颜色反了改成 0 */
#define DISPLAY_INVERT_COLORS           (1U)

/* 背光有效电平：1=高电平点亮 */
#define DISPLAY_BL_ACTIVE_HIGH          (1U)

/* SOC0.96-A 的 RESET（U2.6）板上直接接 3.3V，只能用 SWRESET */
#define DISPLAY_USE_RST                 (0U)
#if DISPLAY_USE_RST
#define DISPLAY_RST_PORT                GPIOB
#define DISPLAY_RST_PIN                 GPIO_PINS_2
#endif

/* ============================ ST7735S 命令 ============================ */
#define ST7735_SWRESET                  (0x01U)
#define ST7735_SLPOUT                   (0x11U)
#define ST7735_NORON                    (0x13U)
#define ST7735_INVOFF                   (0x20U)
#define ST7735_INVON                    (0x21U)
#define ST7735_DISPON                   (0x29U)
#define ST7735_CASET                    (0x2AU)
#define ST7735_RASET                    (0x2BU)
#define ST7735_RAMWR                    (0x2CU)
#define ST7735_MADCTL                   (0x36U)
#define ST7735_COLMOD                   (0x3AU)
#define ST7735_FRMCTR1                  (0xB1U)
#define ST7735_FRMCTR2                  (0xB2U)
#define ST7735_FRMCTR3                  (0xB3U)
#define ST7735_INVCTR                   (0xB4U)
#define ST7735_PWCTR1                   (0xC0U)
#define ST7735_PWCTR2                   (0xC1U)
#define ST7735_PWCTR3                   (0xC2U)
#define ST7735_PWCTR4                   (0xC3U)
#define ST7735_PWCTR5                   (0xC4U)
#define ST7735_VMCTR1                   (0xC5U)
#define ST7735_GMCTRP1                  (0xE0U)
#define ST7735_GMCTRN1                  (0xE1U)

/* ============================ 初始化数据 ============================ */
static const uint8_t display_frmctr1[] = { 0x05U, 0x3CU, 0x3CU };
static const uint8_t display_frmctr2[] = { 0x05U, 0x3CU, 0x3CU };
static const uint8_t display_frmctr3[] = { 0x05U, 0x3CU, 0x3CU, 0x05U, 0x3CU, 0x3CU };
static const uint8_t display_invctr[]  = { 0x03U };
static const uint8_t display_pwctr1[]  = { 0x28U, 0x08U, 0x04U };
static const uint8_t display_pwctr2[]  = { 0xC0U };
static const uint8_t display_pwctr3[]  = { 0x0DU, 0x00U };
static const uint8_t display_pwctr4[]  = { 0x8DU, 0x2AU };
static const uint8_t display_pwctr5[]  = { 0x8DU, 0xEEU };
static const uint8_t display_vmctr1[]  = { 0x1AU };
static const uint8_t display_madctl[]  = { DISPLAY_MADCTL_VALUE };
static const uint8_t display_colmod[]  = { 0x05U };  /* RGB565 */

static const uint8_t display_gamma_p[] =
{
  0x00U, 0x1BU, 0x1AU, 0x0AU, 0x0FU, 0x05U, 0x2BU, 0x33U,
  0x3FU, 0x3FU, 0x0FU, 0x0FU, 0x2EU, 0x30U, 0x0FU, 0x00U
};

static const uint8_t display_gamma_n[] =
{
  0x00U, 0x1BU, 0x1AU, 0x0AU, 0x0EU, 0x04U, 0x2AU, 0x32U,
  0x3FU, 0x3FU, 0x0FU, 0x0EU, 0x2DU, 0x2FU, 0x0FU, 0x00U
};

static uint8_t display_initialized;

/* ============================ 3x5 ASCII 字体 ============================ */
typedef struct
{
  char ch;
  uint8_t rows[5];
} display_glyph_t;

static const display_glyph_t display_font[] =
{
  { ' ', { 0U, 0U, 0U, 0U, 0U } },
  { '%', { 5U, 1U, 2U, 4U, 5U } },
  { '-', { 0U, 0U, 7U, 0U, 0U } },
  { '.', { 0U, 0U, 0U, 0U, 2U } },
  { '/', { 1U, 1U, 2U, 4U, 4U } },
  { ':', { 0U, 2U, 0U, 2U, 0U } },

  { '0', { 7U, 5U, 5U, 5U, 7U } },
  { '1', { 2U, 6U, 2U, 2U, 7U } },
  { '2', { 7U, 1U, 7U, 4U, 7U } },
  { '3', { 7U, 1U, 3U, 1U, 7U } },
  { '4', { 5U, 5U, 7U, 1U, 1U } },
  { '5', { 7U, 4U, 7U, 1U, 7U } },
  { '6', { 7U, 4U, 7U, 5U, 7U } },
  { '7', { 7U, 1U, 2U, 2U, 2U } },
  { '8', { 7U, 5U, 7U, 5U, 7U } },
  { '9', { 7U, 5U, 7U, 1U, 7U } },

  { 'A', { 2U, 5U, 7U, 5U, 5U } },
  { 'B', { 6U, 5U, 6U, 5U, 6U } },
  { 'C', { 3U, 4U, 4U, 4U, 3U } },
  { 'D', { 6U, 5U, 5U, 5U, 6U } },
  { 'E', { 7U, 4U, 6U, 4U, 7U } },
  { 'F', { 7U, 4U, 6U, 4U, 4U } },
  { 'G', { 3U, 4U, 5U, 5U, 3U } },
  { 'H', { 5U, 5U, 7U, 5U, 5U } },
  { 'I', { 7U, 2U, 2U, 2U, 7U } },
  { 'J', { 1U, 1U, 1U, 5U, 2U } },
  { 'K', { 5U, 5U, 6U, 5U, 5U } },
  { 'L', { 4U, 4U, 4U, 4U, 7U } },
  { 'M', { 5U, 7U, 7U, 5U, 5U } },
  { 'N', { 5U, 7U, 7U, 5U, 5U } },
  { 'O', { 2U, 5U, 5U, 5U, 2U } },
  { 'P', { 6U, 5U, 6U, 4U, 4U } },
  { 'Q', { 2U, 5U, 5U, 7U, 3U } },
  { 'R', { 6U, 5U, 6U, 5U, 5U } },
  { 'S', { 3U, 4U, 2U, 1U, 6U } },
  { 'T', { 7U, 2U, 2U, 2U, 2U } },
  { 'U', { 5U, 5U, 5U, 5U, 7U } },
  { 'V', { 5U, 5U, 5U, 5U, 2U } },
  { 'W', { 5U, 5U, 7U, 7U, 5U } },
  { 'X', { 5U, 5U, 2U, 5U, 5U } },
  { 'Y', { 5U, 5U, 2U, 2U, 2U } },
  { 'Z', { 7U, 1U, 2U, 4U, 7U } },

  /* Chill245 品牌字所需的小写 h/i/l */
  { 'h', { 4U, 4U, 6U, 5U, 5U } },
  { 'i', { 2U, 0U, 2U, 2U, 2U } },
  { 'l', { 6U, 2U, 2U, 2U, 3U } },

  /* 0x7F 作为度符号 ° */
  { (char)0x7F, { 2U, 5U, 2U, 0U, 0U } }
};

/* ============================ 底层 SPI ============================ */
static void display_cs_low(void)
{
  gpio_bits_reset(DISPLAY_GPIO_PORT, DISPLAY_CS_PIN);
}

static void display_cs_high(void)
{
  gpio_bits_set(DISPLAY_GPIO_PORT, DISPLAY_CS_PIN);
}

static void display_dc_command(void)
{
  gpio_bits_reset(DISPLAY_DC_PORT, DISPLAY_DC_PIN);
}

static void display_dc_data(void)
{
  gpio_bits_set(DISPLAY_DC_PORT, DISPLAY_DC_PIN);
}

static void display_spi_write_byte(uint8_t data)
{
#if DISPLAY_USE_BITBANG
  uint8_t mask;
  uint8_t nop;

  for(mask = 0x80U; mask != 0U; mask >>= 1)
  {
    if((data & mask) != 0U)
    {
      DISPLAY_GPIO_PORT->scr = DISPLAY_MOSI_PIN;
    }
    else
    {
      DISPLAY_GPIO_PORT->clr = DISPLAY_MOSI_PIN;
    }

    DISPLAY_GPIO_PORT->scr = DISPLAY_SCK_PIN;
    for(nop = 0U; nop < 30U; nop++)
    {
      __NOP();
    }
    DISPLAY_GPIO_PORT->clr = DISPLAY_SCK_PIN;
    for(nop = 0U; nop < 30U; nop++)
    {
      __NOP();
    }
  }
#else
  while(spi_i2s_flag_get(DISPLAY_SPI, SPI_I2S_TDBE_FLAG) == RESET)
  {
  }

  spi_i2s_data_transmit(DISPLAY_SPI, data);
#endif
}

static void display_spi_wait_idle(void)
{
#if DISPLAY_USE_BITBANG
  /* bit-bang 写入是同步完成的 */
#else
  while(spi_i2s_flag_get(DISPLAY_SPI, SPI_I2S_TDBE_FLAG) == RESET)
  {
  }

  while(spi_i2s_flag_get(DISPLAY_SPI, SPI_I2S_BF_FLAG) != RESET)
  {
  }
#endif
}

/**
  * @brief  命令 + 参数一次传输，CS 保持低
  */
static void display_cmd_data(uint8_t cmd, const uint8_t *data, uint16_t len)
{
  uint16_t i;

  display_cs_low();
  display_dc_command();
  display_spi_write_byte(cmd);
  display_spi_wait_idle();
  display_cs_high();

  if((data != NULL) && (len > 0U))
  {
    /* 参考 ST7735S-master：命令和参数之间释放一次 CS */
    display_dc_data();
    display_cs_low();

    for(i = 0U; i < len; i++)
    {
      display_spi_write_byte(data[i]);
    }

    display_spi_wait_idle();
    display_cs_high();
  }
}

void display_write_command(uint8_t cmd)
{
  display_cs_low();
  display_dc_command();
  display_spi_write_byte(cmd);
  display_spi_wait_idle();
  display_cs_high();
}

void display_write_data(const uint8_t *data, uint16_t len)
{
  uint16_t i;

  if((data == NULL) || (len == 0U))
  {
    return;
  }

  display_cs_low();
  display_dc_data();

  for(i = 0U; i < len; i++)
  {
    display_spi_write_byte(data[i]);
  }

  display_spi_wait_idle();
  display_cs_high();
}

/* ============================ 硬件初始化 ============================ */
static void display_hw_init(void)
{
  gpio_init_type gpio_init_struct;
#if !DISPLAY_USE_BITBANG
  spi_init_type spi_init_struct;
#endif

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
#if !DISPLAY_USE_BITBANG
  crm_periph_clock_enable(DISPLAY_SPI_CLOCK, TRUE);
#endif

  /* 先设置安全电平，再切换输出，避免上电毛刺 */
  gpio_bits_set(DISPLAY_GPIO_PORT, DISPLAY_CS_PIN);
  gpio_bits_reset(DISPLAY_GPIO_PORT, DISPLAY_BL_PIN);
  gpio_bits_set(DISPLAY_DC_PORT, DISPLAY_DC_PIN);
  gpio_bits_reset(DISPLAY_GPIO_PORT, DISPLAY_SCK_PIN | DISPLAY_MOSI_PIN);

  gpio_default_para_init(&gpio_init_struct);

  /* PA4 CS + PA3 背光 */
  gpio_init_struct.gpio_pins = DISPLAY_CS_PIN | DISPLAY_BL_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(DISPLAY_GPIO_PORT, &gpio_init_struct);

  /* PB0 RS/DC */
  gpio_init_struct.gpio_pins = DISPLAY_DC_PIN;
  gpio_init(DISPLAY_DC_PORT, &gpio_init_struct);

  /* PA5 SCLK + PA7 MOSI */
  gpio_init_struct.gpio_pins = DISPLAY_SCK_PIN | DISPLAY_MOSI_PIN;
#if DISPLAY_USE_BITBANG
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
#else
  gpio_init_struct.gpio_mode = GPIO_MODE_MUX;
#endif
  gpio_init_struct.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gpio_init_struct.gpio_pull = GPIO_PULL_NONE;
  gpio_init_struct.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(DISPLAY_GPIO_PORT, &gpio_init_struct);

#if DISPLAY_USE_BITBANG
  /* 软件 SPI：SCK 空闲低，MOSI 低 */
  gpio_bits_reset(DISPLAY_GPIO_PORT, DISPLAY_SCK_PIN | DISPLAY_MOSI_PIN);
#else
  gpio_pin_mux_config(DISPLAY_GPIO_PORT, DISPLAY_SCK_SOURCE, DISPLAY_PIN_MUX);
  gpio_pin_mux_config(DISPLAY_GPIO_PORT, DISPLAY_MOSI_SOURCE, DISPLAY_PIN_MUX);

  /* SPI1 master, 4-line SPI TX only */
  spi_default_para_init(&spi_init_struct);
  spi_init_struct.transmission_mode = SPI_TRANSMIT_FULL_DUPLEX;
  spi_init_struct.master_slave_mode = SPI_MODE_MASTER;
  spi_init_struct.mclk_freq_division = DISPLAY_SPI_DIV;
  spi_init_struct.first_bit_transmission = SPI_FIRST_BIT_MSB;
  spi_init_struct.frame_bit_num = SPI_FRAME_8BIT;
  spi_init_struct.clock_polarity = SPI_CLOCK_POLARITY_LOW;
  spi_init_struct.clock_phase = SPI_CLOCK_PHASE_1EDGE;
  spi_init_struct.cs_mode_selection = SPI_CS_SOFTWARE_MODE;
  spi_init(DISPLAY_SPI, &spi_init_struct);
  spi_enable(DISPLAY_SPI, TRUE);
#endif

#if DISPLAY_USE_RST
  gpio_init_struct.gpio_pins = DISPLAY_RST_PIN;
  gpio_init_struct.gpio_mode = GPIO_MODE_OUTPUT;
  gpio_init(DISPLAY_RST_PORT, &gpio_init_struct);
  gpio_bits_set(DISPLAY_RST_PORT, DISPLAY_RST_PIN);
#endif
}

/* ============================ 对外函数 ============================ */

void display_backlight_set(uint8_t on)
{
#if DISPLAY_BL_ACTIVE_HIGH
  if(on != 0U)
  {
    gpio_bits_set(DISPLAY_GPIO_PORT, DISPLAY_BL_PIN);
  }
  else
  {
    gpio_bits_reset(DISPLAY_GPIO_PORT, DISPLAY_BL_PIN);
  }
#else
  if(on != 0U)
  {
    gpio_bits_reset(DISPLAY_GPIO_PORT, DISPLAY_BL_PIN);
  }
  else
  {
    gpio_bits_set(DISPLAY_GPIO_PORT, DISPLAY_BL_PIN);
  }
#endif
}

uint16_t display_get_width(void)
{
  return DISPLAY_WIDTH;
}

uint16_t display_get_height(void)
{
  return DISPLAY_HEIGHT;
}

void display_init(void)
{
  display_hw_init();
  display_backlight_set(0);

  /* 上电稳定 */
  wk_delay_ms(120U);

#if DISPLAY_USE_RST
  gpio_bits_reset(DISPLAY_RST_PORT, DISPLAY_RST_PIN);
  wk_delay_ms(20U);
  gpio_bits_set(DISPLAY_RST_PORT, DISPLAY_RST_PIN);
  wk_delay_ms(120U);
#endif

  /* 硬件复位后再发一次软件复位，确保 ST7735S 进入已知状态 */
  display_cmd_data(ST7735_SWRESET, NULL, 0U);
  wk_delay_ms(150U);

  display_cmd_data(ST7735_SLPOUT, NULL, 0U);
  wk_delay_ms(120U);

  display_cmd_data(ST7735_FRMCTR1, display_frmctr1, sizeof(display_frmctr1));
  display_cmd_data(ST7735_FRMCTR2, display_frmctr2, sizeof(display_frmctr2));
  display_cmd_data(ST7735_FRMCTR3, display_frmctr3, sizeof(display_frmctr3));
  display_cmd_data(ST7735_INVCTR, display_invctr, sizeof(display_invctr));
  display_cmd_data(ST7735_PWCTR1, display_pwctr1, sizeof(display_pwctr1));
  display_cmd_data(ST7735_PWCTR2, display_pwctr2, sizeof(display_pwctr2));
  display_cmd_data(ST7735_PWCTR3, display_pwctr3, sizeof(display_pwctr3));
  display_cmd_data(ST7735_PWCTR4, display_pwctr4, sizeof(display_pwctr4));
  display_cmd_data(ST7735_PWCTR5, display_pwctr5, sizeof(display_pwctr5));
  display_cmd_data(ST7735_VMCTR1, display_vmctr1, sizeof(display_vmctr1));

  display_cmd_data(ST7735_MADCTL, display_madctl, sizeof(display_madctl));
  display_cmd_data(ST7735_COLMOD, display_colmod, sizeof(display_colmod));
  wk_delay_ms(10U);

  display_cmd_data(ST7735_GMCTRP1, display_gamma_p, sizeof(display_gamma_p));
  display_cmd_data(ST7735_GMCTRN1, display_gamma_n, sizeof(display_gamma_n));

#if DISPLAY_INVERT_COLORS
  display_cmd_data(ST7735_INVON, NULL, 0U);
#else
  display_cmd_data(ST7735_INVOFF, NULL, 0U);
#endif

  display_cmd_data(ST7735_NORON, NULL, 0U);
  wk_delay_ms(10U);

  display_cmd_data(ST7735_DISPON, NULL, 0U);
  wk_delay_ms(100U);

  display_initialized = 1U;
}

void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  int32_t x0s;
  int32_t x1s;
  int32_t y0s;
  int32_t y1s;
  uint8_t data[4];

  if((w == 0U) || (h == 0U) || (x >= DISPLAY_WIDTH) || (y >= DISPLAY_HEIGHT))
  {
    return;
  }

  if((x + w) > DISPLAY_WIDTH)
  {
    w = (uint16_t)(DISPLAY_WIDTH - x);
  }

  if((y + h) > DISPLAY_HEIGHT)
  {
    h = (uint16_t)(DISPLAY_HEIGHT - y);
  }

  x0s = (int32_t)x + DISPLAY_X_OFFSET;
  if(x0s < 0)
  {
    x0s = 0;
  }

  /* 窗口宽度保持与调用方写入的像素数一致；负偏移只裁剪最左侧 */
  x1s = x0s + (int32_t)w - 1;
  y0s = (int32_t)y + (int32_t)DISPLAY_Y_OFFSET;
  y1s = y0s + (int32_t)h - 1;

  data[0] = (uint8_t)((uint32_t)x0s >> 8);
  data[1] = (uint8_t)((uint32_t)x0s & 0xFFU);
  data[2] = (uint8_t)((uint32_t)x1s >> 8);
  data[3] = (uint8_t)((uint32_t)x1s & 0xFFU);
  display_cmd_data(ST7735_CASET, data, 4U);

  data[0] = (uint8_t)((uint32_t)y0s >> 8);
  data[1] = (uint8_t)((uint32_t)y0s & 0xFFU);
  data[2] = (uint8_t)((uint32_t)y1s >> 8);
  data[3] = (uint8_t)((uint32_t)y1s & 0xFFU);
  display_cmd_data(ST7735_RASET, data, 4U);
}

static void display_write_color_repeat(uint16_t color, uint32_t count)
{
  uint32_t i;
  uint8_t high = (uint8_t)(color >> 8);
  uint8_t low = (uint8_t)(color & 0xFFU);

  if(count == 0U)
  {
    return;
  }

  display_cs_low();
  display_dc_command();
  display_spi_write_byte(ST7735_RAMWR);
  display_spi_wait_idle();
  display_cs_high();

  display_dc_data();
  display_cs_low();

  for(i = 0U; i < count; i++)
  {
    display_spi_write_byte(high);
    display_spi_write_byte(low);
  }

  display_spi_wait_idle();
  display_cs_high();
}

void display_write_pixels(const uint16_t *colors, uint32_t count)
{
  uint32_t i;
  uint16_t color;

  if((display_initialized == 0U) || (colors == NULL) || (count == 0U))
  {
    return;
  }

  display_cs_low();
  display_dc_command();
  display_spi_write_byte(ST7735_RAMWR);
  display_spi_wait_idle();
  display_cs_high();

  display_dc_data();
  display_cs_low();

  for(i = 0U; i < count; i++)
  {
    color = colors[i];
    display_spi_write_byte((uint8_t)(color >> 8));
    display_spi_write_byte((uint8_t)(color & 0xFFU));
  }

  display_spi_wait_idle();
  display_cs_high();
}

void display_begin_pixel_stream(void)
{
  if(display_initialized == 0U)
  {
    return;
  }

  display_cs_low();
  display_dc_command();
  display_spi_write_byte(ST7735_RAMWR);
  display_spi_wait_idle();
  display_cs_high();

  display_dc_data();
  display_cs_low();
}

void display_write_pixel_stream(const uint16_t *colors, uint32_t count)
{
  uint32_t i;
  uint16_t color;

  if((display_initialized == 0U) || (colors == NULL))
  {
    return;
  }

  for(i = 0U; i < count; i++)
  {
    color = colors[i];
    display_spi_write_byte((uint8_t)(color >> 8));
    display_spi_write_byte((uint8_t)(color & 0xFFU));
  }
}

void display_end_pixel_stream(void)
{
  if(display_initialized == 0U)
  {
    return;
  }

  display_spi_wait_idle();
  display_cs_high();
}
void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  if((display_initialized == 0U) || (w == 0U) || (h == 0U) ||
     (x >= DISPLAY_WIDTH) || (y >= DISPLAY_HEIGHT))
  {
    return;
  }

  if((x + w) > DISPLAY_WIDTH)
  {
    w = (uint16_t)(DISPLAY_WIDTH - x);
  }

  if((y + h) > DISPLAY_HEIGHT)
  {
    h = (uint16_t)(DISPLAY_HEIGHT - y);
  }

  display_set_window(x, y, w, h);
  display_write_color_repeat(color, (uint32_t)w * (uint32_t)h);
}

void display_fill(uint16_t color)
{
  display_fill_rect(0U, 0U, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void display_clear(uint16_t color)
{
  display_fill(color);
}

void display_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
  if((display_initialized == 0U) ||
     (x >= DISPLAY_WIDTH) || (y >= DISPLAY_HEIGHT))
  {
    return;
  }

  display_set_window(x, y, 1U, 1U);
  display_write_color_repeat(color, 1U);
}

void display_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
  display_fill_rect(x, y, w, 1U, color);
}

void display_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
  display_fill_rect(x, y, 1U, h, color);
}

/* ============================ 文本与线条 ============================ */
static const display_glyph_t *display_find_glyph(char ch)
{
  uint16_t i;
  uint16_t count = (uint16_t)(sizeof(display_font) / sizeof(display_font[0]));

  /* 先按原字符找；Chill245 的 h/i/l 使用专门的小写字形 */
  for(i = 0U; i < count; i++)
  {
    if(display_font[i].ch == ch)
    {
      return &display_font[i];
    }
  }

  if((ch >= 'a') && (ch <= 'z'))
  {
    ch = (char)(ch - ('a' - 'A'));

    for(i = 0U; i < count; i++)
    {
      if(display_font[i].ch == ch)
      {
        return &display_font[i];
      }
    }
  }

  return &display_font[0];
}

static void display_draw_glyph(uint16_t x, uint16_t y,
                               const uint8_t rows[5],
                               uint16_t fg, uint16_t bg, uint8_t scale)
{
  uint16_t row;
  uint16_t sy;
  uint16_t col;
  uint16_t sx;
  uint8_t bits;
  uint16_t color;

  if((x >= DISPLAY_WIDTH) || (y >= DISPLAY_HEIGHT))
  {
    return;
  }

  if(((uint32_t)x + (3U * scale)) > DISPLAY_WIDTH)
  {
    return;
  }

  if(((uint32_t)y + (5U * scale)) > DISPLAY_HEIGHT)
  {
    return;
  }

  display_set_window(x, y, (uint16_t)(3U * scale), (uint16_t)(5U * scale));

  display_cs_low();
  display_dc_command();
  display_spi_write_byte(ST7735_RAMWR);
  display_spi_wait_idle();
  display_cs_high();

  display_dc_data();
  display_cs_low();

  for(row = 0U; row < 5U; row++)
  {
    bits = rows[row];

    for(sy = 0U; sy < scale; sy++)
    {
      for(col = 0U; col < 3U; col++)
      {
        color = ((bits & (uint8_t)(1U << (2U - col))) != 0U) ? fg : bg;

        for(sx = 0U; sx < scale; sx++)
        {
          display_spi_write_byte((uint8_t)(color >> 8));
          display_spi_write_byte((uint8_t)(color & 0xFFU));
        }
      }
    }
  }

  display_spi_wait_idle();
  display_cs_high();
}

void display_draw_char(uint16_t x, uint16_t y, char ch,
                       uint16_t fg, uint16_t bg, uint8_t scale)
{
  const display_glyph_t *glyph;

  if(display_initialized == 0U)
  {
    return;
  }

  if(scale == 0U)
  {
    scale = 1U;
  }

  if(scale > 4U)
  {
    scale = 4U;
  }

  glyph = display_find_glyph(ch);
  display_draw_glyph(x, y, glyph->rows, fg, bg, scale);
}

void display_draw_string(uint16_t x, uint16_t y, const char *text,
                         uint16_t fg, uint16_t bg, uint8_t scale)
{
  if((display_initialized == 0U) || (text == NULL))
  {
    return;
  }

  if(scale == 0U)
  {
    scale = 1U;
  }

  if(scale > 4U)
  {
    scale = 4U;
  }

  while(*text != '\0')
  {
    display_draw_char(x, y, *text, fg, bg, scale);
    x = (uint16_t)(x + (4U * scale));
    text++;
  }
}

uint16_t display_string_width(const char *text, uint8_t scale)
{
  uint16_t count = 0U;

  if(text == NULL)
  {
    return 0U;
  }

  if(scale == 0U)
  {
    scale = 1U;
  }

  while(*text != '\0')
  {
    count++;
    text++;
  }

  return (uint16_t)(count * 4U * scale);
}

void display_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
  int16_t dx;
  int16_t sx;
  int16_t dy;
  int16_t sy;
  int16_t err;
  int16_t e2;

  if(display_initialized == 0U)
  {
    return;
  }

  dx = (x0 < x1) ? (int16_t)(x1 - x0) : (int16_t)(x0 - x1);
  sx = (x0 < x1) ? 1 : -1;
  dy = (y0 < y1) ? (int16_t)(y0 - y1) : (int16_t)(y1 - y0);
  sy = (y0 < y1) ? 1 : -1;
  err = (int16_t)(dx + dy);

  for(;;)
  {
    display_draw_pixel((uint16_t)x0, (uint16_t)y0, color);

    if((x0 == x1) && (y0 == y1))
    {
      break;
    }

    e2 = (int16_t)(2 * err);

    if(e2 >= dy)
    {
      err = (int16_t)(err + dy);
      x0 = (int16_t)(x0 + sx);
    }

    if(e2 <= dx)
    {
      err = (int16_t)(err + dx);
      y0 = (int16_t)(y0 + sy);
    }
  }
}

void display_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
  if((display_initialized == 0U) || (w < 2U) || (h < 2U))
  {
    return;
  }

  display_draw_hline(x, y, w, color);
  display_draw_hline(x, (uint16_t)(y + h - 1U), w, color);
  display_draw_vline(x, y, h, color);
  display_draw_vline((uint16_t)(x + w - 1U), y, h, color);
}
