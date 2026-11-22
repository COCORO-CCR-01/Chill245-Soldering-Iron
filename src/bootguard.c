#include "bootguard.h"
#include "buttons.h"
#include "display.h"
#include "eeprom.h"
#include "fmt.h"
#include "heater.h"
#include "statemachine.h"
#include "menu.h"

#define BOOTGUARD_BG        (0x0001U)
#define BOOTGUARD_WHITE     (0xFFFEU)
#define BOOTGUARD_RED       (0xF800U)
#define BOOTGUARD_YELLOW    (0xFFE0U)

typedef enum
{
  BOOTGUARD_WAIT = 0,
  BOOTGUARD_HIGH,
  BOOTGUARD_HOLD,
  BOOTGUARD_DONE
} bootguard_state_t;

static bootguard_state_t bootguard_state;
static uint8_t bootguard_last_any;
static uint8_t bootguard_last_next;
static uint8_t bootguard_last_prev;
static uint32_t bootguard_last_draw;
static int bootguard_last_temp;

static uint8_t bootguard_any_pressed(void)
{
  return ((buttons_is_pressed(BUTTON_MENU) != 0U) ||
          (buttons_is_pressed(BUTTON_PREV) != 0U) ||
          (buttons_is_pressed(BUTTON_NEXT) != 0U)) ? 1U : 0U;
}

static void bootguard_center(uint16_t y, const char *text,
                             uint16_t color, uint8_t scale)
{
  uint16_t width = display_string_width(text, scale);
  uint16_t x = (width < DISPLAY_WIDTH) ?
               (uint16_t)((DISPLAY_WIDTH - width) / 2U) : 0U;
  display_draw_string(x, y, text, color, BOOTGUARD_BG, scale);
}

static void bootguard_draw(const sm_status_t *status)
{
  char text[20];
  int temp = (status->temp_valid != 0U) ?
             (int)(status->tip_c + 0.5f) : 0;
  int high = (int)(g_eeprom_params.boot_high_temp_c + 0.5f);

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, DISPLAY_HEIGHT, BOOTGUARD_BG);

  if(bootguard_state == BOOTGUARD_HIGH)
  {
    bootguard_center(4U, "TEMP HIGH", BOOTGUARD_RED, 2U);
    bootguard_center(20U, "NO HEAT", BOOTGUARD_WHITE, 2U);
    if(status->temp_valid != 0U)
    {
      fmt_snprintf(text, sizeof(text), "%dC", temp);
      bootguard_center(38U, text, BOOTGUARD_WHITE, 4U);
      if((int)status->tip_c < high)
      {
        bootguard_center(66U, "ANY KEY", BOOTGUARD_YELLOW, 1U);
      }
      else
      {
        fmt_snprintf(text, sizeof(text), "<%dC", high);
        bootguard_center(66U, text, BOOTGUARD_YELLOW, 1U);
      }
    }
    else
    {
      bootguard_center(40U, "SENSOR", BOOTGUARD_YELLOW, 2U);
    }
  }
  else if(bootguard_state == BOOTGUARD_HOLD)
  {
    bootguard_center(6U, "BOOT HOLD", BOOTGUARD_RED, 2U);
    bootguard_center(24U, "HEAT OFF", BOOTGUARD_WHITE, 2U);
    bootguard_center(44U, "PREV/NEXT", BOOTGUARD_YELLOW, 2U);
    bootguard_center(62U, "START", BOOTGUARD_WHITE, 1U);
    bootguard_center(72U, "HOLD MENU SETUP", BOOTGUARD_YELLOW, 1U);
  }
  else
  {
    bootguard_center(32U, "WAIT", BOOTGUARD_WHITE, 2U);
  }
}

void bootguard_init(void)
{
  (void)sm_set_heater_enabled(0U);
  heater_force_off();
  bootguard_state = BOOTGUARD_WAIT;
  bootguard_last_any = bootguard_any_pressed();
  bootguard_last_next = buttons_is_pressed(BUTTON_NEXT);
  bootguard_last_prev = buttons_is_pressed(BUTTON_PREV);
  bootguard_last_draw = 0U;
  bootguard_last_temp = -1000;
}

uint8_t bootguard_is_active(void)
{
  return (bootguard_state != BOOTGUARD_DONE) ? 1U : 0U;
}

void bootguard_invalidate(void)
{
  bootguard_last_draw = 0U;
}

void bootguard_abort(void)
{
  (void)sm_set_heater_enabled(0U);
  heater_force_off();
  bootguard_state = BOOTGUARD_DONE;
}

void bootguard_task(uint32_t now_ms)
{
  sm_status_t status;
  uint8_t any;
  uint8_t next;
  uint8_t prev;
  uint8_t any_edge;
  uint8_t next_edge;
  uint8_t prev_edge;

  if(bootguard_state == BOOTGUARD_DONE)
  {
    return;
  }

  /* 联锁期间任何路径重新使能加热都要被压回 */
  if(sm_is_heater_enabled() != 0U)
  {
    (void)sm_set_heater_enabled(0U);
  }
  heater_force_off();

  sm_get_status(&status);
  any = bootguard_any_pressed();
  next = buttons_is_pressed(BUTTON_NEXT);
  prev = buttons_is_pressed(BUTTON_PREV);
  any_edge = ((any != 0U) && (bootguard_last_any == 0U)) ? 1U : 0U;
  next_edge = ((next != 0U) && (bootguard_last_next == 0U)) ? 1U : 0U;
  prev_edge = ((prev != 0U) && (bootguard_last_prev == 0U)) ? 1U : 0U;
  bootguard_last_any = any;
  bootguard_last_next = next;
  bootguard_last_prev = prev;

  switch(bootguard_state)
  {
    case BOOTGUARD_WAIT:
      if(status.temp_valid != 0U)
      {
        if(status.tip_c >= g_eeprom_params.boot_high_temp_c)
        {
          bootguard_state = BOOTGUARD_HIGH;
          bootguard_last_draw = 0U;
        }
        else if(g_eeprom_params.boot_hold_enabled != 0U)
        {
          bootguard_state = BOOTGUARD_HOLD;
          bootguard_last_draw = 0U;
        }
        else
        {
          (void)sm_set_heater_enabled(1U);
          bootguard_state = BOOTGUARD_DONE;
          return;
        }
      }
      break;

    case BOOTGUARD_HIGH:
      if((status.temp_valid != 0U) &&
         (status.tip_c < g_eeprom_params.boot_high_temp_c) &&
         (any_edge != 0U))
      {
        (void)sm_set_heater_enabled(1U);
        bootguard_state = BOOTGUARD_DONE;
        return;
      }
      break;

    case BOOTGUARD_HOLD:
      if((buttons_is_pressed(BUTTON_MENU) != 0U) &&
         (buttons_get_press_duration(BUTTON_MENU, now_ms) >= 2000U))
      {
        menu_open_service_setup();
        return;
      }

      /* PREV / NEXT 任意一个都能启动加热：单手操作时不必专门去找 NEXT。
       * 提示语已经跟着改成 "PREV/NEXT"。 */
      if((next_edge != 0U) || (prev_edge != 0U))
      {
        (void)sm_set_heater_enabled(1U);
        bootguard_state = BOOTGUARD_DONE;
        return;
      }
      break;

    default:
      return;
  }

  /* 只有真的会把温度画到屏幕上的状态，温度变化才算重画的理由。
   *
   * BOOTGUARD_HOLD 画的是 BOOT HOLD / HEAT OFF / PREV/NEXT / START / HOLD MENU
   * SETUP，BOOTGUARD_WAIT 只有一个 WAIT，全是固定文字，一个数字都没有；原来这里
   * 不分状态一律"整数温度变了就重画"，而 bootguard_draw() 开头是整屏
   * display_fill_rect(0,0,160,80)（25600 字节），烙铁靠环境温度漂个 1°C 就把整个
   * HOLD 界面刷黑重铺一遍 —— 这就是 boot HOLD 界面一直闪的原因。
   * 换状态的地方都会把 bootguard_last_draw 清 0，所以该重画的一次都不会少。 */
  {
    uint8_t temp_shown = (bootguard_state == BOOTGUARD_HIGH) ? 1U : 0U;
    uint8_t temp_changed = ((status.temp_valid != 0U) &&
                            ((int)(status.tip_c + 0.5f) != bootguard_last_temp))
                           ? 1U : 0U;

    if((bootguard_last_draw == 0U) ||
       ((temp_shown != 0U) && (temp_changed != 0U)))
    {
      bootguard_draw(&status);
      bootguard_last_draw = (now_ms == 0U) ? 1U : now_ms;
      bootguard_last_temp = (status.temp_valid != 0U) ?
                            (int)(status.tip_c + 0.5f) : -1;
    }
  }
}
