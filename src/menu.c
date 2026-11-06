/**
  **************************************************************************
  * @file     menu.c
  * @brief    160x80 横屏多级设置菜单
  **************************************************************************
  * 操作：
  *   MENU 长按 2 s      进入菜单
  *   MENU 单击          进入子菜单 / 确认编辑
  *   PREV / NEXT        移动选择 / 修改数值
  *   MENU 长按 2 s      返回上一级；根页面关闭菜单
  *
  * 菜单关闭时统一把配置写回各模块。
  **************************************************************************
  */

#include "menu.h"
#include "fmt.h"
#include "display.h"
#include "appcolors.h"
#include "about_qr.h"
#include "version.h"
#include "statemachine.h"
#include "pid.h"
#include "temp.h"
#include "verifyheater.h"
#include "powermanagement.h"
#include "autoroll.h"
#include "maindash.h"
#include "eeprom.h"
#include "buzzer.h"
#include "hms.h"
#include "tempcal.h"
#include "onekeyboost.h"
#include "ui_icons.h"
#include <stdio.h>
#include <string.h>

#define menu_config                     g_eeprom_params

#define MENU_LONG_MS                    (2000U)
#define MENU_ROWS                       (5U)
#define MENU_ROW_Y                      (14U)
#define MENU_ROW_H                      (11U)
#define MENU_TIMEOUT_MIN_S              (5.0f)

#define UI_BLACK                        appcolors_surface()
#define UI_WHITE                        appcolors_get(APP_COLOR_QUATERNARY)
#define UI_GRAY                         appcolors_get(APP_COLOR_TERTIARY)
#define UI_DARK                         appcolors_surface()
#define UI_PANEL                        appcolors_surface()
#define UI_HEADER                       appcolors_surface()
#define UI_ACCENT                       appcolors_get(APP_COLOR_PRIMARY)
#define UI_CYAN                         appcolors_get(APP_COLOR_SECONDARY)
#define UI_ORANGE                       appcolors_get(APP_COLOR_PRIMARY)
#define UI_RED                          (0xF800U)
#define UI_VALUE                        appcolors_get(APP_COLOR_QUATERNARY)

typedef enum
{
  MENU_ITEM_SUBMENU = 0,
  MENU_ITEM_FLOAT,
  MENU_ITEM_BOOL,
  MENU_ITEM_ENUM,
  MENU_ITEM_ACTION,
  MENU_ITEM_CURVE_CUSTOM,
  MENU_ITEM_CURVE_CAL
} menu_item_type_t;

typedef struct menu_page menu_page_t;

typedef struct
{
  const char *label;
  menu_item_type_t type;
  void *value;
  const menu_page_t *submenu;
  const char *const *options;
  uint8_t option_count;
  float step;
  float min_value;
  float max_value;
  uint8_t decimals;
  const char *unit;
  void (*action)(void);
} menu_item_t;

struct menu_page
{
  const char *title;
  const menu_item_t *items;
  uint8_t count;
};

typedef enum
{
  MENU_VIEW_LIST = 0,
  MENU_VIEW_EDIT,
  MENU_VIEW_CURVE,
  MENU_VIEW_ABOUT
} menu_view_t;

typedef enum
{
  MENU_CURVE_CUSTOM = 0,
  MENU_CURVE_CAL
} menu_curve_t;

typedef struct
{
  uint8_t view;
  const menu_page_t *page;
  uint8_t selected;
  uint8_t first;
  uint8_t curve_kind;
} menu_nav_t;

static menu_nav_t menu_nav;
static menu_nav_t menu_nav_stack[8];
static uint8_t menu_nav_depth;

static uint8_t menu_open_flag;
static uint8_t menu_need_draw;
static uint8_t menu_long_handled;
static uint8_t menu_quick_edit;
static uint32_t menu_defaults_arm_until;

static const menu_item_t *menu_edit_item;
static float menu_edit_value;

static char menu_dynamic_title[24];
static menu_item_t menu_dynamic_items[5];
static menu_page_t menu_dynamic_page;

static const char *const menu_pid_mode_options[] =
{
  "STD", "BOOST", "CUSTOM"
};

static const char *const menu_ch224a_options[] =
{
  "5V", "9V", "12V", "15V", "20V", "28V"
};

static void menu_action_back(void);
static void menu_action_sleep(void);
static void menu_action_exit(void);
static void menu_action_autotune(void);
static void menu_action_cancel_autotune(void);
static void menu_action_clear_fault(void);
static void menu_action_heater_toggle(void);
static void menu_action_hms(void);
static void menu_action_defaults(void);
static void menu_action_tip_cal(void);
static void menu_action_main_page(void);
static void menu_action_about(void);
static float menu_item_step(const menu_item_t *item);

/* ------------------------------------------------------------------------- */
/* 页面数据                                                                   */
/* ------------------------------------------------------------------------- */

static const menu_item_t menu_temperature_items[] =
{
  { "SETPOINT",     MENU_ITEM_FLOAT,  &menu_config.setpoint_c,       NULL, NULL, 0, 5.0f,   0.0f,  450.0f, 0U, "C",  NULL },
  { "PID MODE",     MENU_ITEM_ENUM,   &menu_config.pid_mode,         NULL, menu_pid_mode_options, 3U, 1.0f, 0.0f, 2.0f, 0U, "", NULL },
  { "KP",           MENU_ITEM_FLOAT,  &menu_config.standard_curve.kp, NULL, NULL, 0, 0.05f,  0.0f,  10.0f,  2U, "",   NULL },
  { "KI",           MENU_ITEM_FLOAT,  &menu_config.standard_curve.ki, NULL, NULL, 0, 0.005f, 0.0f,  5.0f,   3U, "",   NULL },
  { "KD",           MENU_ITEM_FLOAT,  &menu_config.standard_curve.kd, NULL, NULL, 0, 0.05f,  0.0f,  10.0f,  2U, "",   NULL },
  { "CUSTOM CURVE", MENU_ITEM_CURVE_CUSTOM, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "TEMP CAL",     MENU_ITEM_CURVE_CAL,    NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "CAL ENABLE",   MENU_ITEM_BOOL,   &menu_config.cal_enabled,      NULL, NULL, 0, 1.0f, 0.0f, 1.0f, 0U, "", NULL },
  { "CAL REF",      MENU_ITEM_FLOAT,  &menu_config.cal_ref_c,        NULL, NULL, 0, 1.0f,  -20.0f, 60.0f, 0U, "C", NULL },
  { "START AUTOTUNE", MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_autotune },
  { "CANCEL TUNE",  MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_cancel_autotune },
  { "BACK",         MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_back }
};

static const menu_item_t menu_sleep_items[] =
{
  { "SLEEP TIME",   MENU_ITEM_FLOAT, &menu_config.stationary_s,      NULL, NULL, 0, 5.0f,   5.0f,   3600.0f, 0U, "s",  NULL },
  { "SLEEP PWR",    MENU_ITEM_FLOAT, &menu_config.sleep_power_pct,   NULL, NULL, 0, 1.0f,   0.0f,   100.0f,  0U, "%",  NULL },
  { "SLEEP TEMP",   MENU_ITEM_FLOAT, &menu_config.reduced_temp_c,    NULL, NULL, 0, 5.0f,   50.0f,  400.0f,  0U, "C",  NULL },
  { "HEAT OFF T",   MENU_ITEM_FLOAT, &menu_config.reduced_off_s,     NULL, NULL, 0, 5.0f,   5.0f,   7200.0f, 0U, "s",  NULL },
  { "LCD OFF T",    MENU_ITEM_FLOAT, &menu_config.backlight_off_s,   NULL, NULL, 0, 5.0f,   5.0f,   7200.0f, 0U, "s",  NULL },
  { "PD VOLTAGE",   MENU_ITEM_ENUM,  &menu_config.ch224a_index,       NULL, menu_ch224a_options, 6U, 1.0f, 0.0f, 5.0f, 0U, "", NULL },
  { "LOW VOLT",     MENU_ITEM_FLOAT, &menu_config.low_voltage_v,     NULL, NULL, 0, 0.5f,   5.0f,   30.0f,   1U, "V",  NULL },
  { "HIGH VOLT",    MENU_ITEM_FLOAT, &menu_config.high_voltage_v,    NULL, NULL, 0, 0.5f,   20.0f,  40.0f,   1U, "V",  NULL },
  { "VOLT HYST",    MENU_ITEM_FLOAT, &menu_config.voltage_hysteresis_v, NULL, NULL, 0, 0.1f, 0.0f,   3.0f,    1U, "V",  NULL },
  { "TRIP TIME",    MENU_ITEM_FLOAT, &menu_config.voltage_trip_ms,   NULL, NULL, 0, 10.0f,  10.0f,  5000.0f, 0U, "ms", NULL },
  { "RECOVERY",     MENU_ITEM_FLOAT, &menu_config.voltage_recovery_ms, NULL, NULL, 0, 100.0f, 100.0f, 60000.0f, 0U, "ms", NULL },
  { "SLEEP NOW",    MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_sleep },
  { "BACK",         MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_back }
};

static const menu_item_t menu_protect_items[] =
{
  { "OVER TEMP",   MENU_ITEM_FLOAT, &menu_config.verify_overtemp_c,     NULL, NULL, 0, 5.0f,  100.0f, 600.0f,  0U, "C",   NULL },
  { "TEMP JUMP",   MENU_ITEM_FLOAT, &menu_config.verify_jump_c,         NULL, NULL, 0, 1.0f,  1.0f,   200.0f,  0U, "C",   NULL },
  { "JUMP RATE",   MENU_ITEM_FLOAT, &menu_config.verify_jump_rate,      NULL, NULL, 0, 10.0f, 10.0f,  2000.0f, 0U, "C/S", NULL },
  { "NO RISE T",   MENU_ITEM_FLOAT, &menu_config.verify_no_rise_s,      NULL, NULL, 0, 5.0f,  1.0f,   300.0f,  0U, "s",   NULL },
  { "NO RISE D",   MENU_ITEM_FLOAT, &menu_config.verify_no_rise_delta_c, NULL, NULL, 0, 1.0f, 1.0f,   100.0f,  0U, "C",   NULL },
  { "FULL PWR T",  MENU_ITEM_FLOAT, &menu_config.verify_full_power_s,   NULL, NULL, 0, 5.0f,  1.0f,   300.0f,  0U, "s",   NULL },
  { "TEMP DROP",   MENU_ITEM_FLOAT, &menu_config.verify_drop_c,         NULL, NULL, 0, 5.0f,  10.0f,  300.0f,  0U, "C",   NULL },
  { "HEATER ON/OFF", MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_heater_toggle },
  { "CLEAR FAULT", MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_clear_fault },
  { "BACK",        MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_back }
};

static const menu_item_t menu_ui_items[] =
{
  { "BUZZER",    MENU_ITEM_BOOL,  &menu_config.buzzer_enabled,   NULL, NULL, 0, 1.0f, 0.0f, 1.0f, 0U, "", NULL },
  { "VOLUME",    MENU_ITEM_FLOAT, &menu_config.buzzer_volume,    NULL, NULL, 0, 10.0f, 0.0f, 100.0f, 0U, "%", NULL },
  { "MAIN PAGE", MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_main_page },
  { "ALT DASH",  MENU_ITEM_BOOL,  &menu_config.maindash_alt, NULL, NULL, 0, 1.0f, 0.0f, 1.0f, 0U, "", NULL },
  { "LIGHT MODE", MENU_ITEM_BOOL, &menu_config.ui_light,      NULL, NULL, 0, 1.0f, 0.0f, 1.0f, 0U, "", NULL },
  { "BOOT LOGO", MENU_ITEM_FLOAT, &menu_config.bootlogo_delay_s, NULL, NULL, 0, 1.0f, 0.0f, 10.0f, 0U, "s", NULL },
  { "BACK",      MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_back }
};

static const menu_item_t menu_boost_items[] =
{
  { "BOOST TEMP", MENU_ITEM_FLOAT, &menu_config.boost_offset_c,  NULL, NULL, 0, 5.0f, 0.0f, 200.0f, 0U, "C", NULL },
  { "DROP TEMP",  MENU_ITEM_FLOAT, &menu_config.boost_drop_c,    NULL, NULL, 0, 5.0f, 5.0f, 100.0f, 0U, "C", NULL },
  { "BOOST TIME", MENU_ITEM_FLOAT, &menu_config.boost_timeout_s, NULL, NULL, 0, 5.0f, 10.0f, 1800.0f, 0U, "s", NULL },
  { "BACK",       MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_back }
};

static const menu_item_t menu_system_items[] =
{
  { "START AUTOTUNE", MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_autotune },
  { "CANCEL TUNE",    MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_cancel_autotune },
  { "TEMP STEP",      MENU_ITEM_FLOAT,  &menu_config.setpoint_step_c, NULL, NULL, 0, 1.0f, 1.0f, 100.0f, 0U, "C", NULL },
  { "TIME STEP",      MENU_ITEM_FLOAT,  &menu_config.time_step_s,     NULL, NULL, 0, 1.0f, 1.0f, 600.0f, 0U, "s", NULL },
  { "HEATER ON/OFF",  MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_heater_toggle },
  { "CLEAR FAULT",    MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_clear_fault },
  { "HMS SELF TEST",  MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_hms },
  { "TIP CAL",        MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_tip_cal },
  { "DEFAULTS",       MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_defaults },
  { "ABOUT",          MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_about },
  { "EXIT MENU",      MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_exit },
  { "BACK",           MENU_ITEM_ACTION, NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_back }
};

static const menu_page_t menu_page_temperature =
{
  "TEMPERATURE", menu_temperature_items,
  (uint8_t)(sizeof(menu_temperature_items) / sizeof(menu_temperature_items[0]))
};

static const menu_page_t menu_page_sleep =
{
  "SLEEP / POWER", menu_sleep_items,
  (uint8_t)(sizeof(menu_sleep_items) / sizeof(menu_sleep_items[0]))
};

static const menu_page_t menu_page_protect =
{
  "PROTECTION", menu_protect_items,
  (uint8_t)(sizeof(menu_protect_items) / sizeof(menu_protect_items[0]))
};

static const menu_page_t menu_page_ui =
{
  "DISPLAY / UI", menu_ui_items,
  (uint8_t)(sizeof(menu_ui_items) / sizeof(menu_ui_items[0]))
};

static const menu_page_t menu_page_boost =
{
  "BOOST", menu_boost_items,
  (uint8_t)(sizeof(menu_boost_items) / sizeof(menu_boost_items[0]))
};

static const menu_page_t menu_page_system =
{
  "SYSTEM", menu_system_items,
  (uint8_t)(sizeof(menu_system_items) / sizeof(menu_system_items[0]))
};

static const menu_item_t menu_root_items[] =
{
  { "TEMPERATURE",   MENU_ITEM_SUBMENU, NULL, &menu_page_temperature, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "BOOST",         MENU_ITEM_SUBMENU, NULL, &menu_page_boost,       NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "SLEEP / POWER", MENU_ITEM_SUBMENU, NULL, &menu_page_sleep,       NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "PROTECTION",    MENU_ITEM_SUBMENU, NULL, &menu_page_protect,     NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "DISPLAY / UI",  MENU_ITEM_SUBMENU, NULL, &menu_page_ui,          NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "SYSTEM",        MENU_ITEM_SUBMENU, NULL, &menu_page_system,      NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", NULL },
  { "EXIT",          MENU_ITEM_ACTION,  NULL, NULL, NULL, 0, 0.0f, 0.0f, 0.0f, 0U, "", menu_action_exit }
};

static const menu_page_t menu_page_root =
{
  "SETTINGS", menu_root_items,
  (uint8_t)(sizeof(menu_root_items) / sizeof(menu_root_items[0]))
};

/* ------------------------------------------------------------------------- */
/* 菜单状态                                                                   */
/* ------------------------------------------------------------------------- */

static void menu_adjust_first(void)
{
  uint8_t count = 0U;

  if(menu_nav.view == MENU_VIEW_CURVE)
  {
    count = 10U;
  }
  else if(menu_nav.page != NULL)
  {
    count = menu_nav.page->count;
  }

  if(count == 0U)
  {
    menu_nav.first = 0U;
    return;
  }

  if(menu_nav.selected < menu_nav.first)
  {
    menu_nav.first = menu_nav.selected;
  }

  if(menu_nav.selected >= (uint8_t)(menu_nav.first + MENU_ROWS))
  {
    menu_nav.first = (uint8_t)(menu_nav.selected - (MENU_ROWS - 1U));
  }
}

static void menu_push_nav(void)
{
  if(menu_nav_depth < (uint8_t)(sizeof(menu_nav_stack) / sizeof(menu_nav_stack[0])))
  {
    menu_nav_stack[menu_nav_depth] = menu_nav;
    menu_nav_depth++;
  }
}

static void menu_pop_nav(void)
{
  if(menu_nav_depth > 0U)
  {
    menu_nav_depth--;
    menu_nav = menu_nav_stack[menu_nav_depth];
    menu_adjust_first();
    menu_need_draw = 1U;
  }
}

static void menu_draw(void);

static void menu_close_internal(void)
{
  menu_open_flag = 0U;
  menu_need_draw = 0U;

  /* 关闭菜单后重置轮换计时，避免立即跳页 */
  autoroll_set_page(autoroll_get_page());
  autoroll_request_redraw();
}

void menu_open(void)
{
  sm_status_t status;

  if(menu_open_flag != 0U)
  {
    return;
  }

  /* HMS 或热端校准占用屏幕期间禁止进入设置 */
  if((hms_is_active() != 0U) || (tempcal_is_active() != 0U))
  {
    return;
  }

  /* 进入设置即退出 Boost，避免菜单保存覆盖 Boost 设定 */
  onekeyboost_cancel();

  sm_get_status(&status);

  /* 整定过程中不打开菜单，避免旧参数覆盖整定结果 */
  if(status.autotune_state == PID_AUTOTUNE_RUNNING)
  {
    return;
  }

  power_wake();
  eeprom_capture_current();

  menu_nav.view = (uint8_t)MENU_VIEW_LIST;
  menu_nav.page = &menu_page_root;
  menu_nav.selected = 0U;
  menu_nav.first = 0U;
  menu_nav.curve_kind = 0U;
  menu_nav_depth = 0U;

  menu_open_flag = 1U;
  menu_need_draw = 1U;
  menu_long_handled = 1U;
}

void menu_close(void)
{
  if(menu_open_flag == 0U)
  {
    return;
  }

  eeprom_apply();
  if(eeprom_save() == 0U)
  {
    buzzer_play(BUZZER_SOUND_ERROR);
  }
  menu_close_internal();
}

void menu_force_close(void)
{
  if(menu_open_flag == 0U)
  {
    return;
  }

  menu_open_flag = 0U;
  menu_nav_depth = 0U;
  menu_nav.view = (uint8_t)MENU_VIEW_LIST;
  menu_need_draw = 1U;
  menu_long_handled = 0U;
}

uint8_t menu_is_open(void)
{
  return menu_open_flag;
}

static void menu_back(void)
{
  if(menu_quick_edit != 0U)
  {
    menu_quick_edit = 0U;
    menu_close();
    autoroll_set_page(AUTOROLL_PAGE_MAIN);
    autoroll_request_redraw();
    return;
  }

  if(menu_nav.view == MENU_VIEW_EDIT)
  {
    menu_pop_nav();
    return;
  }

  if(menu_nav_depth > 0U)
  {
    menu_pop_nav();
    return;
  }

  menu_close();
}

/* ------------------------------------------------------------------------- */
/* 菜单编辑                                                                   */
/* ------------------------------------------------------------------------- */

static void menu_start_edit(const menu_item_t *item)
{
  if((item == NULL) || (item->type != MENU_ITEM_FLOAT) ||
     (item->value == NULL))
  {
    return;
  }

  menu_push_nav();

  menu_edit_item = item;
  menu_edit_value = *(float *)item->value;

  menu_nav.view = (uint8_t)MENU_VIEW_EDIT;
  menu_need_draw = 1U;
}

void menu_open_quick_setpoint(void)
{
  if(menu_open_flag != 0U)
  {
    return;
  }

  menu_open();

  if(menu_open_flag == 0U)
  {
    return;
  }

  menu_nav.page = &menu_page_temperature;
  menu_nav.selected = 0U;
  menu_nav.first = 0U;
  menu_nav_depth = 0U;

  menu_start_edit(&menu_page_temperature.items[0]);
  menu_quick_edit = 1U;
}

static void menu_adjust_edit(int8_t direction)
{
  if(menu_edit_item == NULL)
  {
    return;
  }

  menu_edit_value += (menu_item_step(menu_edit_item) * (float)direction);

  if(menu_edit_value < menu_edit_item->min_value)
  {
    menu_edit_value = menu_edit_item->min_value;
  }

  if(menu_edit_value > menu_edit_item->max_value)
  {
    menu_edit_value = menu_edit_item->max_value;
  }

  if(menu_quick_edit != 0U)
  {
    maindash_notify_setpoint_adjusted();
  }

  menu_need_draw = 1U;
}

static void menu_confirm_edit(void)
{
  if((menu_edit_item != NULL) && (menu_edit_item->value != NULL))
  {
    *(float *)menu_edit_item->value = menu_edit_value;
  }

  menu_edit_item = NULL;

  if(menu_quick_edit != 0U)
  {
    menu_quick_edit = 0U;
    menu_close();
    autoroll_set_page(AUTOROLL_PAGE_MAIN);
    autoroll_request_redraw();
    return;
  }

  menu_pop_nav();
}

static void menu_build_point_page(uint8_t curve_kind, uint8_t index)
{
  menu_item_t *item;

  memset(menu_dynamic_items, 0, sizeof(menu_dynamic_items));

  if(curve_kind == (uint8_t)MENU_CURVE_CUSTOM)
  {
    float prev_temp;
    float next_temp;

    if(index > 0U)
    {
      prev_temp = menu_config.custom_points[index - 1U].temp_c;
    }
    else
    {
      prev_temp = -1.0f;
    }

    if(index < (PID_CUSTOM_POINTS - 1U))
    {
      next_temp = menu_config.custom_points[index + 1U].temp_c;
    }
    else
    {
      next_temp = 501.0f;
    }

    item = &menu_dynamic_items[0];
    item->label = "TEMP";
    item->type = MENU_ITEM_FLOAT;
    item->value = &menu_config.custom_points[index].temp_c;
    item->step = 5.0f;
    item->min_value = (index == 0U) ? 0.0f : (prev_temp + 1.0f);
    item->max_value = (index == (PID_CUSTOM_POINTS - 1U)) ?
                      500.0f : (next_temp - 1.0f);
    item->decimals = 0U;
    item->unit = "C";

    item = &menu_dynamic_items[1];
    item->label = "KP";
    item->type = MENU_ITEM_FLOAT;
    item->value = &menu_config.custom_points[index].gain.kp;
    item->step = 0.05f;
    item->min_value = 0.0f;
    item->max_value = 10.0f;
    item->decimals = 2U;
    item->unit = "";

    item = &menu_dynamic_items[2];
    item->label = "KI";
    item->type = MENU_ITEM_FLOAT;
    item->value = &menu_config.custom_points[index].gain.ki;
    item->step = 0.005f;
    item->min_value = 0.0f;
    item->max_value = 5.0f;
    item->decimals = 3U;
    item->unit = "";

    item = &menu_dynamic_items[3];
    item->label = "KD";
    item->type = MENU_ITEM_FLOAT;
    item->value = &menu_config.custom_points[index].gain.kd;
    item->step = 0.05f;
    item->min_value = 0.0f;
    item->max_value = 10.0f;
    item->decimals = 2U;
    item->unit = "";

    item = &menu_dynamic_items[4];
    item->label = "BACK";
    item->type = MENU_ITEM_ACTION;
    item->action = menu_action_back;

    menu_dynamic_page.count = 5U;

    fmt_snprintf(menu_dynamic_title, sizeof(menu_dynamic_title),
             "CUSTOM %u/%u", (unsigned)(index + 1U),
             (unsigned)PID_CUSTOM_POINTS);
  }
  else
  {
    float prev_adc;
    float next_adc;

    if(index > 0U)
    {
      prev_adc = menu_config.cal_adc[index - 1U];
    }
    else
    {
      prev_adc = -1.0f;
    }

    if(index < (TEMP_TC_CAL_POINTS - 1U))
    {
      next_adc = menu_config.cal_adc[index + 1U];
    }
    else
    {
      next_adc = 4096.0f;
    }

    item = &menu_dynamic_items[0];
    item->label = "ADC";
    item->type = MENU_ITEM_FLOAT;
    item->value = &menu_config.cal_adc[index];
    item->step = 10.0f;
    item->min_value = (index == 0U) ? 0.0f : (prev_adc + 1.0f);
    item->max_value = (index == (TEMP_TC_CAL_POINTS - 1U)) ?
                      4095.0f : (next_adc - 1.0f);
    item->decimals = 0U;
    item->unit = "";

    item = &menu_dynamic_items[1];
    item->label = "TEMP";
    item->type = MENU_ITEM_FLOAT;
    item->value = &menu_config.cal_temp[index];
    item->step = 1.0f;
    item->min_value = -50.0f;
    item->max_value = 600.0f;
    item->decimals = 0U;
    item->unit = "C";

    item = &menu_dynamic_items[2];
    item->label = "BACK";
    item->type = MENU_ITEM_ACTION;
    item->action = menu_action_back;

    menu_dynamic_page.count = 3U;

    fmt_snprintf(menu_dynamic_title, sizeof(menu_dynamic_title),
             "CAL %u/%u", (unsigned)(index + 1U),
             (unsigned)TEMP_TC_CAL_POINTS);
  }

  menu_dynamic_page.title = menu_dynamic_title;
  menu_dynamic_page.items = menu_dynamic_items;
}

static void menu_enter_curve_point(void)
{
  uint8_t index = menu_nav.selected;
  uint8_t curve_kind = menu_nav.curve_kind;

  menu_push_nav();
  menu_build_point_page(curve_kind, index);

  menu_nav.view = (uint8_t)MENU_VIEW_LIST;
  menu_nav.page = &menu_dynamic_page;
  menu_nav.selected = 0U;
  menu_nav.first = 0U;
  menu_need_draw = 1U;
}

static void menu_enter_item(void)
{
  const menu_item_t *item;
  uint8_t value;

  if((menu_nav.view == (uint8_t)MENU_VIEW_EDIT))
  {
    menu_confirm_edit();
    return;
  }

  if(menu_nav.view == (uint8_t)MENU_VIEW_CURVE)
  {
    menu_enter_curve_point();
    return;
  }

  if((menu_nav.page == NULL) || (menu_nav.selected >= menu_nav.page->count))
  {
    return;
  }

  item = &menu_nav.page->items[menu_nav.selected];

  switch(item->type)
  {
    case MENU_ITEM_SUBMENU:
      if(item->submenu != NULL)
      {
        menu_push_nav();
        menu_nav.page = item->submenu;
        menu_nav.selected = 0U;
        menu_nav.first = 0U;
        menu_need_draw = 1U;
      }
      break;

    case MENU_ITEM_CURVE_CUSTOM:
      menu_push_nav();
      menu_nav.view = (uint8_t)MENU_VIEW_CURVE;
      menu_nav.curve_kind = (uint8_t)MENU_CURVE_CUSTOM;
      menu_nav.selected = 0U;
      menu_nav.first = 0U;
      menu_need_draw = 1U;
      break;

    case MENU_ITEM_CURVE_CAL:
      menu_push_nav();
      menu_nav.view = (uint8_t)MENU_VIEW_CURVE;
      menu_nav.curve_kind = (uint8_t)MENU_CURVE_CAL;
      menu_nav.selected = 0U;
      menu_nav.first = 0U;
      menu_need_draw = 1U;
      break;

    case MENU_ITEM_FLOAT:
      menu_start_edit(item);
      break;

    case MENU_ITEM_BOOL:
      if(item->value != NULL)
      {
        value = *(uint8_t *)item->value;
        *(uint8_t *)item->value = (value != 0U) ? 0U : 1U;
        menu_need_draw = 1U;
      }
      break;

    case MENU_ITEM_ENUM:
      if((item->value != NULL) && (item->option_count > 0U))
      {
        value = *(uint8_t *)item->value;
        value++;
        if(value >= item->option_count)
        {
          value = 0U;
        }
        *(uint8_t *)item->value = value;
        menu_need_draw = 1U;
      }
      break;

    case MENU_ITEM_ACTION:
    default:
      if(item->action != NULL)
      {
        item->action();
      }
      break;
  }
}

static void menu_move(int8_t direction)
{
  uint8_t count;

  if(menu_nav.view == (uint8_t)MENU_VIEW_EDIT)
  {
    menu_adjust_edit(direction);
    return;
  }

  if(menu_nav.view == (uint8_t)MENU_VIEW_CURVE)
  {
    count = 10U;
  }
  else if(menu_nav.page != NULL)
  {
    count = menu_nav.page->count;
  }
  else
  {
    return;
  }

  if(count == 0U)
  {
    return;
  }

  if(direction < 0)
  {
    if(menu_nav.selected > 0U)
    {
      menu_nav.selected--;
    }
  }
  else
  {
    if(menu_nav.selected < (uint8_t)(count - 1U))
    {
      menu_nav.selected++;
    }
  }

  menu_adjust_first();
  menu_need_draw = 1U;
}

/* ------------------------------------------------------------------------- */
/* 动作                                                                       */
/* ------------------------------------------------------------------------- */

static void menu_action_back(void)
{
  menu_back();
}

static void menu_action_exit(void)
{
  menu_close();
}

static void menu_action_sleep(void)
{
  menu_close();
  power_sleep();
}

static void menu_action_autotune(void)
{
  eeprom_apply();
  if(eeprom_save() == 0U)
  {
    buzzer_play(BUZZER_SOUND_ERROR);
  }
  menu_close_internal();

  if(sm_start_autotune(menu_config.setpoint_c) == 0U)
  {
    buzzer_play(BUZZER_SOUND_ERROR);
  }
}

static void menu_action_cancel_autotune(void)
{
  sm_cancel_autotune();
  menu_need_draw = 1U;
}

static void menu_action_clear_fault(void)
{
  (void)sm_clear_fault();
  (void)power_clear_fault();
  menu_need_draw = 1U;
}

static void menu_action_heater_toggle(void)
{
  (void)sm_set_heater_enabled((sm_is_heater_enabled() != 0U) ? 0U : 1U);
  menu_need_draw = 1U;
}

static void menu_action_hms(void)
{
  menu_close();
  hms_show_all_ok();
}

static void menu_action_tip_cal(void)
{
  menu_close();

  if(tempcal_start() == 0U)
  {
    buzzer_play(BUZZER_SOUND_ERROR);
  }
}

static void menu_action_defaults(void)
{
  uint32_t now_ms = sm_get_uptime_ms();

  if((menu_defaults_arm_until != 0U) &&
     ((int32_t)(now_ms - menu_defaults_arm_until) < 0))
  {
    eeprom_factory_reset();
    return;
  }

  menu_defaults_arm_until = now_ms + 3000U;
  buzzer_play(BUZZER_SOUND_ERROR);
  menu_need_draw = 1U;
}

static void menu_action_main_page(void)
{
  autoroll_set_page(AUTOROLL_PAGE_MAIN);
  menu_close();
}

static void menu_action_about(void)
{
  menu_nav.view = (uint8_t)MENU_VIEW_ABOUT;
  menu_need_draw = 1U;
}

/* ------------------------------------------------------------------------- */
/* 绘制                                                                       */
/* ------------------------------------------------------------------------- */

static void menu_draw_header(const char *title, const char *counter)
{
  uint16_t counter_x;

  display_fill_rect(0U, 0U, DISPLAY_WIDTH, 12U, UI_HEADER);
  display_draw_string(3U, 2U, title, UI_WHITE, UI_HEADER, 2U);

  if(counter != NULL)
  {
    counter_x = (uint16_t)(DISPLAY_WIDTH -
                           display_string_width(counter, 1U) - 3U);
    display_draw_string(counter_x, 4U, counter, UI_ORANGE, UI_HEADER, 1U);
  }

  display_draw_hline(0U, 12U, DISPLAY_WIDTH, UI_ORANGE);
}

static void menu_draw_footer(const char *text)
{
  uint32_t now_ms = sm_get_uptime_ms();

  if((menu_defaults_arm_until != 0U) &&
     ((int32_t)(now_ms - menu_defaults_arm_until) < 0))
  {
    display_draw_string(2U, 71U, "PRESS AGAIN TO RESET",
                        UI_RED, UI_BLACK, 1U);
    return;
  }

  display_draw_string(2U, 71U, text, UI_GRAY, UI_BLACK, 1U);
}

static void menu_draw_arrow(uint16_t x, uint16_t y, uint16_t color)
{
  display_draw_line((int16_t)x, (int16_t)(y + 2U),
                    (int16_t)(x + 4U), (int16_t)(y + 5U), color);
  display_draw_line((int16_t)x, (int16_t)(y + 8U),
                    (int16_t)(x + 4U), (int16_t)(y + 5U), color);
}

static float menu_time_step(void)
{
  float step_s = menu_config.time_step_s;

  if(step_s < 1.0f)
  {
    step_s = 1.0f;
  }

  if(step_s > 600.0f)
  {
    step_s = 600.0f;
  }

  return step_s;
}

static float menu_item_step(const menu_item_t *item)
{
  if(item == NULL)
  {
    return 1.0f;
  }

  /* 两个步进设置项自己始终按 1 调整 */
  if((item->value == &menu_config.setpoint_step_c) ||
     (item->value == &menu_config.time_step_s))
  {
    return 1.0f;
  }

  /* 温度步进对所有 °C 项生效 */
  if((item->unit != NULL) && (strcmp(item->unit, "C") == 0))
  {
    return onekeyboost_get_setpoint_step();
  }

  /* 秒级时间项使用 TIME STEP */
  if((item->unit != NULL) && (strcmp(item->unit, "s") == 0))
  {
    return menu_time_step();
  }

  return item->step;
}

static ui_icon_t menu_item_icon(const menu_item_t *item){
  if(item == NULL)
  {
    return UI_ICON_GEAR;
  }

  switch(item->type)
  {
    case MENU_ITEM_SUBMENU:
      return UI_ICON_ARROW_RIGHT;

    case MENU_ITEM_FLOAT:
      return UI_ICON_UP_DOWN;

    case MENU_ITEM_BOOL:
      if((item->value != NULL) && (*(uint8_t *)item->value != 0U))
      {
        return UI_ICON_CHECK;
      }
      return UI_ICON_POWER;

    case MENU_ITEM_ENUM:
      return UI_ICON_MODE;

    case MENU_ITEM_ACTION:
      return UI_ICON_BOLT;

    case MENU_ITEM_CURVE_CUSTOM:
    case MENU_ITEM_CURVE_CAL:
      return UI_ICON_CURVE;

    default:
      return UI_ICON_GEAR;
  }
}

/* newlib-nano 未链接浮点 printf，这里用整数缩放格式化，避免数字变空 */
static void menu_format_float(char *text, uint16_t size, float value,
                              uint8_t decimals, const char *unit)
{
  uint32_t scale = 1U;
  uint32_t abs_scaled;
  uint32_t whole;
  uint32_t frac;
  int32_t scaled;
  const char *sign;
  const char *suffix;

  if((text == NULL) || (size == 0U))
  {
    return;
  }

  if(decimals > 2U)
  {
    decimals = 2U;
  }

  if(decimals == 1U)
  {
    scale = 10U;
  }
  else if(decimals == 2U)
  {
    scale = 100U;
  }

  scaled = (int32_t)((value * (float)scale) +
                     ((value >= 0.0f) ? 0.5f : -0.5f));
  sign = (scaled < 0) ? "-" : "";
  abs_scaled = (scaled < 0) ? (uint32_t)(-scaled) : (uint32_t)scaled;
  whole = abs_scaled / scale;
  frac = abs_scaled % scale;
  suffix = (unit != NULL) ? unit : "";

  if(decimals == 0U)
  {
    fmt_snprintf(text, size, "%s%lu%s", sign, (unsigned long)whole, suffix);
  }
  else if(decimals == 1U)
  {
    fmt_snprintf(text, size, "%s%lu.%01lu%s", sign, (unsigned long)whole,
             (unsigned long)frac, suffix);
  }
  else
  {
    fmt_snprintf(text, size, "%s%lu.%02lu%s", sign, (unsigned long)whole,
             (unsigned long)frac, suffix);
  }
}

static void menu_format_item_value(const menu_item_t *item,
                                   char *text, uint16_t size)
{
  if((item == NULL) || (text == NULL) || (size == 0U))
  {
    return;
  }

  text[0] = '\0';

  switch(item->type)
  {
    case MENU_ITEM_FLOAT:
      if(item->value != NULL)
      {
        menu_format_float(text, size, *(float *)item->value,
                          item->decimals,
                          (item->unit != NULL) ? item->unit : "");
      }
      break;

    case MENU_ITEM_BOOL:
      if(item->value != NULL)
      {
        fmt_snprintf(text, size, "%s",
                 (*(uint8_t *)item->value != 0U) ? "ON" : "OFF");
      }
      break;

    case MENU_ITEM_ENUM:
      if((item->value != NULL) && (item->options != NULL))
      {
        uint8_t index = *(uint8_t *)item->value;

        if(index < item->option_count)
        {
          fmt_snprintf(text, size, "%s", item->options[index]);
        }
        else
        {
          fmt_snprintf(text, size, "?");
        }
      }
      break;

    case MENU_ITEM_SUBMENU:
    case MENU_ITEM_CURVE_CUSTOM:
    case MENU_ITEM_CURVE_CAL:
    case MENU_ITEM_ACTION:
    default:
      break;
  }
}

static void menu_draw_list(void)
{
  uint8_t row;
  uint8_t index;
  uint8_t y;
  uint16_t color;
  uint16_t background;
  char value[24];
  const menu_item_t *item;
  uint8_t has_arrow;

  if((menu_nav.page == NULL) || (menu_nav.page->items == NULL))
  {
    return;
  }

  for(row = 0U; row < MENU_ROWS; row++)
  {
    index = (uint8_t)(menu_nav.first + row);

    if(index >= menu_nav.page->count)
    {
      break;
    }

    item = &menu_nav.page->items[index];
    y = (uint8_t)(MENU_ROW_Y + (row * MENU_ROW_H));
    color = (index == menu_nav.selected) ? UI_VALUE : UI_CYAN;
    background = (index == menu_nav.selected) ? UI_ACCENT : UI_BLACK;
    has_arrow = ((item->type == MENU_ITEM_SUBMENU) ||
                 (item->type == MENU_ITEM_CURVE_CUSTOM) ||
                 (item->type == MENU_ITEM_CURVE_CAL)) ? 1U : 0U;

    if(index == menu_nav.selected)
    {
      display_fill_rect(0U, y, 3U, MENU_ROW_H - 1U, UI_ORANGE);
      display_fill_rect(3U, y, (uint16_t)(DISPLAY_WIDTH - 3U),
                        MENU_ROW_H - 1U, UI_ACCENT);
    }

    ui_icon_draw(menu_item_icon(item), 4U, (uint16_t)(y + 1U),
                 (index == menu_nav.selected) ? UI_WHITE : UI_GRAY);
    display_draw_string(14U, (uint16_t)(y + 3U), item->label,
                        (index == menu_nav.selected) ? UI_WHITE : UI_GRAY,
                        background, 1U);

    menu_format_item_value(item, value, sizeof(value));

    if(value[0] != '\0')
    {
      uint16_t width = display_string_width(value, 1U);
      uint16_t x = (uint16_t)((has_arrow != 0U) ? (144U - width) :
                                                    (156U - width));

      display_draw_string(x, (uint16_t)(y + 3U), value, color,
                          background, 1U);
    }

    if(has_arrow != 0U)
    {
      menu_draw_arrow(150U, y, UI_GRAY);
    }
  }
}

static void menu_draw_curve(void)
{
  uint8_t row;
  uint8_t index;
  uint8_t y;
  char label[8];
  char value[24];
  uint16_t background;

  for(row = 0U; row < MENU_ROWS; row++)
  {
    index = (uint8_t)(menu_nav.first + row);

    if(index >= 10U)
    {
      break;
    }

    y = (uint8_t)(MENU_ROW_Y + (row * MENU_ROW_H));
    background = (index == menu_nav.selected) ? UI_ACCENT : UI_BLACK;

    if(index == menu_nav.selected)
    {
      display_fill_rect(0U, y, 3U, MENU_ROW_H - 1U, UI_ORANGE);
      display_fill_rect(3U, y, (uint16_t)(DISPLAY_WIDTH - 3U),
                        MENU_ROW_H - 1U, UI_ACCENT);
    }

    ui_icon_draw(UI_ICON_CURVE, 4U, (uint16_t)(y + 1U),
                 (index == menu_nav.selected) ? UI_WHITE : UI_GRAY);
    fmt_snprintf(label, sizeof(label), "%02u", (unsigned)(index + 1U));
    display_draw_string(14U, (uint16_t)(y + 3U), label, UI_WHITE,
                        background, 1U);

    if(menu_nav.curve_kind == (uint8_t)MENU_CURVE_CUSTOM)
    {
      char kp[12];

      menu_format_float(kp, sizeof(kp),
                        menu_config.custom_points[index].gain.kp, 2U, "");
      fmt_snprintf(value, sizeof(value), "T%d K%s",
               (int)(menu_config.custom_points[index].temp_c + 0.5f),
               kp);
    }
    else
    {
      fmt_snprintf(value, sizeof(value), "A%d T%d",
               (int)(menu_config.cal_adc[index] + 0.5f),
               (int)(menu_config.cal_temp[index] + 0.5f));
    }

    display_draw_string(34U, (uint16_t)(y + 3U), value,
                        (index == menu_nav.selected) ? UI_VALUE : UI_CYAN,
                        background, 1U);
  }
}

static void menu_draw_edit(void)
{
  char value[32];
  char range[64];
  char min_text[12];
  char max_text[12];
  char step_text[12];
  uint16_t width;
  uint16_t x;
  uint16_t scale = 3U;

  if(menu_edit_item == NULL)
  {
    return;
  }

  display_fill_rect(6U, 18U, 148U, 26U, UI_PANEL);
  display_draw_rect(6U, 18U, 148U, 26U, UI_ACCENT);

  menu_format_float(value, sizeof(value), menu_edit_value,
                    menu_edit_item->decimals,
                    (menu_edit_item->unit != NULL) ? menu_edit_item->unit : "");

  width = display_string_width(value, scale);

  if(width > 140U)
  {
    scale = 2U;
    width = display_string_width(value, scale);
  }

  x = (uint16_t)((DISPLAY_WIDTH - width) / 2U);
  display_draw_string(x, 23U, value, UI_VALUE, UI_PANEL, scale);

  menu_format_float(min_text, sizeof(min_text), menu_edit_item->min_value, 2U, "");
  menu_format_float(max_text, sizeof(max_text), menu_edit_item->max_value, 2U, "");
  menu_format_float(step_text, sizeof(step_text), menu_item_step(menu_edit_item), 2U, "");
  fmt_snprintf(range, sizeof(range), "MIN %s  MAX %s  STEP %s",
           min_text, max_text, step_text);
  display_draw_string(4U, 51U, range, UI_GRAY, UI_BLACK, 1U);

  menu_draw_footer("PREV/NXT ADJ  ENT OK  HOLD BACK");
}

static void menu_draw_header_counter(char *text, uint16_t size)
{
  uint8_t count;

  if(menu_nav.view == (uint8_t)MENU_VIEW_CURVE)
  {
    count = 10U;
  }
  else if(menu_nav.page != NULL)
  {
    count = menu_nav.page->count;
  }
  else
  {
    count = 0U;
  }

  fmt_snprintf(text, size, "%u/%u",
           (unsigned)(menu_nav.selected + 1U), (unsigned)count);
}

static void menu_draw_about(void)
{
  static uint16_t qr_line[78];
  uint16_t x;
  uint16_t y;
  uint16_t mx;
  uint16_t my;
  uint8_t bit;

  display_clear(UI_BLACK);

  display_set_window(0U, 0U, 78U, 78U);
  display_begin_pixel_stream();

  for(y = 0U; y < 78U; y++)
  {
    for(x = 0U; x < 78U; x++)
    {
      qr_line[x] = 0xFFFFU;
      if((x >= 6U) && (x < 72U) && (y >= 6U) && (y < 72U))
      {
        mx = (uint16_t)((x - 6U) / 2U);
        my = (uint16_t)((y - 6U) / 2U);
        bit = (uint8_t)((about_qr[my][mx >> 3] >>
                         (7U - (mx & 7U))) & 1U);
        if(bit != 0U)
        {
          qr_line[x] = 0x0000U;
        }
      }
    }
    display_write_pixel_stream_raw(qr_line, 78U);
  }
  display_end_pixel_stream();

  display_draw_string_transparent(80U, 3U, "CHILL245", UI_ACCENT, 2U);
  display_draw_string_transparent(80U, 14U, "SOLDERING", UI_GRAY, 1U);
  display_draw_string_transparent(80U, 25U, "AUTHOR", UI_GRAY, 1U);
  display_draw_string_transparent(80U, 31U, "COCORO", UI_WHITE, 1U);
  display_draw_string_transparent(80U, 37U, "CCR-01", UI_WHITE, 1U);
  display_draw_string_transparent(80U, 49U, "VERSION", UI_GRAY, 1U);
  display_draw_string_transparent(80U, 55U, FW_VERSION, UI_WHITE, 1U);
  display_draw_string_transparent(80U, 67U, "ISSUES", UI_GRAY, 1U);
  display_draw_string_transparent(80U, 72U, "WELCOME", UI_WHITE, 1U);
}

static void menu_draw(void)
{
  char counter[12];
  const char *title;

  if(menu_nav.view == (uint8_t)MENU_VIEW_ABOUT)
  {
    menu_draw_about();
    menu_need_draw = 0U;
    return;
  }

  display_clear(UI_BLACK);

  if(menu_nav.view == (uint8_t)MENU_VIEW_EDIT)
  {
    title = (menu_edit_item != NULL) ? menu_edit_item->label : "EDIT";
    menu_draw_header(title, "EDIT");
  }
  else if(menu_nav.view == (uint8_t)MENU_VIEW_CURVE)
  {
    title = (menu_nav.curve_kind == (uint8_t)MENU_CURVE_CUSTOM) ?
            "CUSTOM PID" : "TEMP CAL";
    menu_draw_header_counter(counter, sizeof(counter));
    menu_draw_header(title, counter);
  }
  else
  {
    title = (menu_nav.page != NULL) ? menu_nav.page->title : "SETTINGS";

    if(menu_nav.page == &menu_dynamic_page)
    {
      menu_draw_header(title, NULL);
    }
    else
    {
      menu_draw_header_counter(counter, sizeof(counter));
      menu_draw_header(title, counter);
    }
  }

  if(menu_nav.view == (uint8_t)MENU_VIEW_EDIT)
  {
    menu_draw_edit();
  }
  else if(menu_nav.view == (uint8_t)MENU_VIEW_CURVE)
  {
    menu_draw_curve();
    menu_draw_footer("ENT EDIT  PREV/NXT  HOLD BACK");
  }
  else
  {
    menu_draw_list();
    menu_draw_footer("ENT SEL  PREV/NXT  HOLD BACK");
  }

  menu_need_draw = 0U;
}

/* ------------------------------------------------------------------------- */
/* 按键                                                                       */
/* ------------------------------------------------------------------------- */

static void menu_on_button(button_id_t button, button_event_t event)
{
  if(event == BUTTON_EVENT_DOUBLE_CLICK)
  {
    if((button == BUTTON_MENU) && (menu_open_flag == 0U))
    {
      menu_open_quick_setpoint();
    }
    return;
  }

  if(menu_open_flag == 0U)
  {
    return;
  }

  if(menu_nav.view == (uint8_t)MENU_VIEW_ABOUT)
  {
    if((event == BUTTON_EVENT_CLICK) || (event == BUTTON_EVENT_REPEAT))
    {
      menu_nav.view = (uint8_t)MENU_VIEW_LIST;
      menu_need_draw = 1U;
    }
    return;
  }

  if(event == BUTTON_EVENT_CLICK)
  {
    if(button == BUTTON_MENU)
    {
      menu_enter_item();
    }
    else if(button == BUTTON_PREV)
    {
      menu_move(-1);
    }
    else if(button == BUTTON_NEXT)
    {
      menu_move(1);
    }
  }
  else if(event == BUTTON_EVENT_REPEAT)
  {
    if(button == BUTTON_PREV)
    {
      menu_move(-1);
    }
    else if(button == BUTTON_NEXT)
    {
      menu_move(1);
    }
  }
}

void menu_init(void)
{
  menu_open_flag = 0U;
  menu_need_draw = 0U;
  menu_long_handled = 0U;
  menu_nav_depth = 0U;

  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_CLICK,
                                  menu_on_button);
  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_DOUBLE_CLICK,
                                  menu_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_CLICK,
                                  menu_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_CLICK,
                                  menu_on_button);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_REPEAT,
                                  menu_on_button);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_REPEAT,
                                  menu_on_button);
}

void menu_update(uint32_t now_ms)
{
  if(buttons_is_pressed(BUTTON_MENU) != 0U)
  {
    if((buttons_get_press_duration(BUTTON_MENU, now_ms) >= MENU_LONG_MS) &&
       (menu_long_handled == 0U))
    {
      menu_long_handled = 1U;

      if(menu_open_flag != 0U)
      {
        menu_back();
      }
      else
      {
        menu_open();
      }
    }
  }
  else
  {
    menu_long_handled = 0U;
  }

  if(menu_open_flag != 0U)
  {
    power_notify_activity(now_ms);

    if(menu_need_draw != 0U)
    {
      menu_draw();
    }
  }
}
