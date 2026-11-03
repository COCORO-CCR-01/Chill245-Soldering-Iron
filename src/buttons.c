/**
  **************************************************************************
  * @file     buttons.c
  * @brief    三个按键扫描与事件注册
  **************************************************************************
  * 硬件：
  *   PA15 -> MENU / ENTER
  *   PB3  -> PREV
  *   PB4  -> NEXT
  *
  * 三个按键均内部上拉，按下拉低。
  * 注意：PA15/PB3/PB4 在部分 AT32 型号上是 JTAG 引脚。
  *       AT32F421 使用 SWD 调试时，这三个脚可作 GPIO；
  *       如果调试器占用，需要先切换为 SWD-only。
  **************************************************************************
  */

#include "buttons.h"
#include "at32f421_conf.h"
#include <stddef.h>

#define BUTTONS_MAX_CALLBACKS           (24U)

typedef struct
{
  button_id_t button;
  button_event_t event;
  button_callback_t callback;
} button_callback_entry_t;

typedef struct
{
  uint8_t raw_pressed;
  uint8_t stable_pressed;
  uint8_t long_sent;
  uint32_t raw_change_ms;
  uint32_t press_start_ms;
  uint32_t last_repeat_ms;
} button_state_t;

static button_state_t button_state[BUTTON_COUNT];
static button_callback_entry_t button_callbacks[BUTTONS_MAX_CALLBACKS];
static uint8_t button_callback_count;
static uint8_t buttons_initialized;

static uint8_t buttons_read_pressed(button_id_t button)
{
  flag_status level;

  switch(button)
  {
    case BUTTON_MENU:
      level = gpio_input_data_bit_read(GPIOA, GPIO_PINS_15);
      break;

    case BUTTON_PREV:
      level = gpio_input_data_bit_read(GPIOB, GPIO_PINS_3);
      break;

    case BUTTON_NEXT:
      level = gpio_input_data_bit_read(GPIOB, GPIO_PINS_4);
      break;

    default:
      return 0U;
  }

  /* 上拉输入，低电平表示按下 */
  return (level == RESET) ? 1U : 0U;
}

static void buttons_emit(button_id_t button, button_event_t event)
{
  uint8_t i;

  for(i = 0U; i < button_callback_count; i++)
  {
    if((button_callbacks[i].button == button) &&
       (button_callbacks[i].event == event))
    {
      button_callbacks[i].callback(button, event);
    }
  }
}

void buttons_init(void)
{
  gpio_init_type gpio_init_struct;
  uint8_t i;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&gpio_init_struct);
  gpio_init_struct.gpio_mode = GPIO_MODE_INPUT;
  gpio_init_struct.gpio_pull = GPIO_PULL_UP;

  /* PA15 MENU */
  gpio_init_struct.gpio_pins = GPIO_PINS_15;
  gpio_init(GPIOA, &gpio_init_struct);

  /* PB3 PREV + PB4 NEXT */
  gpio_init_struct.gpio_pins = GPIO_PINS_3 | GPIO_PINS_4;
  gpio_init(GPIOB, &gpio_init_struct);

  for(i = 0U; i < (uint8_t)BUTTON_COUNT; i++)
  {
    button_state[i].raw_pressed = 0U;
    button_state[i].stable_pressed = 0U;
    button_state[i].long_sent = 0U;
    button_state[i].raw_change_ms = 0U;
    button_state[i].press_start_ms = 0U;
    button_state[i].last_repeat_ms = 0U;
  }

  button_callback_count = 0U;
  buttons_initialized = 1U;
}

void buttons_update(uint32_t now_ms)
{
  uint8_t i;
  uint8_t pressed;
  button_state_t *state;

  if(buttons_initialized == 0U)
  {
    return;
  }

  for(i = 0U; i < (uint8_t)BUTTON_COUNT; i++)
  {
    state = &button_state[i];
    pressed = buttons_read_pressed((button_id_t)i);

    if(pressed != state->raw_pressed)
    {
      state->raw_pressed = pressed;
      state->raw_change_ms = now_ms;
    }

    if(((uint32_t)(now_ms - state->raw_change_ms) >= BUTTON_DEBOUNCE_MS) &&
       (state->stable_pressed != state->raw_pressed))
    {
      state->stable_pressed = state->raw_pressed;

      if(state->stable_pressed != 0U)
      {
        state->press_start_ms = now_ms;
        state->long_sent = 0U;
        state->last_repeat_ms = now_ms;
        buttons_emit((button_id_t)i, BUTTON_EVENT_PRESS);
      }
      else
      {
        buttons_emit((button_id_t)i, BUTTON_EVENT_RELEASE);

        if(state->long_sent == 0U)
        {
          buttons_emit((button_id_t)i, BUTTON_EVENT_CLICK);
        }
      }
    }

    if((state->stable_pressed != 0U) && (state->long_sent == 0U) &&
       ((uint32_t)(now_ms - state->press_start_ms) >= BUTTON_LONG_PRESS_MS))
    {
      state->long_sent = 1U;
      state->last_repeat_ms = now_ms;
      buttons_emit((button_id_t)i, BUTTON_EVENT_LONG_PRESS);
    }

    if((state->stable_pressed != 0U) && (state->long_sent != 0U) &&
       ((uint32_t)(now_ms - state->last_repeat_ms) >= BUTTON_REPEAT_MS))
    {
      state->last_repeat_ms = now_ms;
      buttons_emit((button_id_t)i, BUTTON_EVENT_REPEAT);
    }
  }
}

uint8_t buttons_register_callback(button_id_t button,
                                  button_event_t event,
                                  button_callback_t callback)
{
  uint8_t i;

  if((button >= BUTTON_COUNT) ||
     (event >= BUTTON_EVENT_COUNT) ||
     (callback == NULL))
  {
    return 0U;
  }

  for(i = 0U; i < button_callback_count; i++)
  {
    if((button_callbacks[i].button == button) &&
       (button_callbacks[i].event == event) &&
       (button_callbacks[i].callback == callback))
    {
      return 0U;
    }
  }

  if(button_callback_count >= BUTTONS_MAX_CALLBACKS)
  {
    return 0U;
  }

  button_callbacks[button_callback_count].button = button;
  button_callbacks[button_callback_count].event = event;
  button_callbacks[button_callback_count].callback = callback;
  button_callback_count++;

  return 1U;
}

uint8_t buttons_is_pressed(button_id_t button)
{
  if(button >= BUTTON_COUNT)
  {
    return 0U;
  }

  return button_state[button].stable_pressed;
}

uint32_t buttons_get_press_duration(button_id_t button, uint32_t now_ms)
{
  if(button >= BUTTON_COUNT)
  {
    return 0U;
  }

  if(button_state[button].stable_pressed == 0U)
  {
    return 0U;
  }

  return (uint32_t)(now_ms - button_state[button].press_start_ms);
}
