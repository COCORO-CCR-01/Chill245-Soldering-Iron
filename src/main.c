/**
  **************************************************************************
  * @file     main.c
  * @brief    Chill245 烙铁控制器主程序
  **************************************************************************
  * 所有模块由 statemachine 统一调度：
  *   TMR1 更新中断 20 kHz：ADC -> 温度 -> PID -> 保护 -> PWM
  *   主循环 1 kHz：控制快照 -> 调试串口 -> 按键/电源/菜单/蜂鸣器/UI -> EEPROM 后台任务
  *
  * 上电时先关闭加热，加载并应用 EEPROM 参数后再恢复输出。
  **************************************************************************
  */

#include "at32f421_wk_config.h"
#include "wk_system.h"
#include "display.h"
#include "bootlogo.h"
#include "appcolors.h"
#include "buttons.h"
#include "powermanagement.h"
#include "menu.h"
#include "eeprom.h"
#include "buzzer.h"
#include "led.h"
#include "hms.h"
#include "tempcal.h"
#include "onekeyboost.h"
#include "maindash.h"
#include "wallpaper.h"
#include "graph.h"
#include "autoroll.h"
#include "statemachine.h"
#include "temp.h"
#include "bootguard.h"

#define AUTOROLL_INTERVAL_MS            (5000U)

/* 屏幕显示恢复 */
#define APP_ENABLE_DISPLAY              (1U)

/* 若上一次运行遗留硬件 WDT，先把超时放宽并重装，避免启动过程被复位 */
static void boot_wdt_relax(void)
{
  wdt_register_write_enable(TRUE);
  wdt_divider_set(WDT_CLK_DIV_64);
  wdt_reload_value_set(2499U);
  wdt_counter_reload();
}

/* 快速调温记忆：最后一次调节后保持 N 秒再写入 EEPROM */
static uint32_t quick_setpoint_due_ms;
static uint8_t quick_setpoint_pending;

static void main_quick_setpoint_arm(uint32_t now_ms)
{
  float hold_s = g_eeprom_params.quick_setpoint_save_s;

  if(hold_s < 1.0f)
  {
    hold_s = 30.0f;
  }
  if(hold_s > 600.0f)
  {
    hold_s = 600.0f;
  }

  quick_setpoint_due_ms = now_ms + (uint32_t)((hold_s * 1000.0f) + 0.5f);
  quick_setpoint_pending = 1U;
}

static void main_quick_setpoint_task(uint32_t now_ms)
{
  sm_status_t status;
  float setpoint;
  float diff;

  if(quick_setpoint_pending == 0U)
  {
    return;
  }
  if((int32_t)(quick_setpoint_due_ms - now_ms) > 0)
  {
    return;
  }
  if((menu_is_open() != 0U) || (onekeyboost_is_active() != 0U))
  {
    return;
  }

  sm_get_status(&status);
  if(status.autotune_state == PID_AUTOTUNE_RUNNING)
  {
    return;
  }

  setpoint = sm_get_setpoint();
  diff = setpoint - g_eeprom_params.setpoint_c;
  if(diff < 0.0f)
  {
    diff = -diff;
  }

  if(diff >= 0.05f)
  {
    float old_setpoint = g_eeprom_params.setpoint_c;
    g_eeprom_params.setpoint_c = setpoint;
    if(eeprom_save() == 0U)
    {
      g_eeprom_params.setpoint_c = old_setpoint;
      quick_setpoint_due_ms = now_ms + 1000U;
      return;
    }
  }

  quick_setpoint_pending = 0U;
}

/* 非菜单界面按键仲裁 */
static void main_adjust_setpoint(int8_t direction)
{
  float setpoint = sm_get_setpoint();
  float step = onekeyboost_get_setpoint_step();

  setpoint += (step * (float)direction);

  if(setpoint < 0.0f)
  {
    setpoint = 0.0f;
  }

  if(setpoint > 450.0f)
  {
    setpoint = 450.0f;
  }

  if(sm_set_setpoint(setpoint) != 0U)
  {
    maindash_notify_setpoint_adjusted();
    main_quick_setpoint_arm(sm_get_uptime_ms());
    power_notify_activity(sm_get_uptime_ms());
    buzzer_play(BUZZER_SOUND_KEY);
  }
}

static void main_buttons(button_id_t button, button_event_t event)
{
  if(bootguard_is_active() != 0U)
  {
    return;
  }

  if((menu_is_open() != 0U) || (tempcal_is_active() != 0U))
  {
    return;
  }

  /* 短按 MENU：单按键轮换页面；双击 MENU 由菜单模块处理 */
  if((button == BUTTON_MENU) && (event == BUTTON_EVENT_CLICK))
  {
    autoroll_next();
    return;
  }

  if((button == BUTTON_NEXT) && (event == BUTTON_EVENT_CLICK))
  {
    /* 深度休眠的第一次 NEXT 只负责唤醒屏幕 */
    if(power_get_state() == POWER_STATE_BACKLIGHT_OFF)
    {
      power_wake();
      return;
    }

    if(power_get_state() == POWER_STATE_REDUCED)
    {
      power_wake();
    }

    /* Boost 状态下短按 NEXT 只退出 Boost，不调整温度 */
    if(onekeyboost_is_active() != 0U)
    {
      onekeyboost_cancel();
      return;
    }

    main_adjust_setpoint(1);
    return;
  }

  if((button == BUTTON_PREV) && (event == BUTTON_EVENT_CLICK))
  {
    if(power_get_state() == POWER_STATE_BACKLIGHT_OFF)
    {
      power_wake();
      return;
    }

    if(power_get_state() == POWER_STATE_REDUCED)
    {
      power_wake();
    }

    /* Boost 状态下短按 PREV 不修改设定温度，避免意外退出 Boost */
    if(onekeyboost_is_active() != 0U)
    {
      return;
    }

    main_adjust_setpoint(-1);
    return;
  }

  /* 长按 PREV：关闭/恢复加热器，不再关闭屏幕 */
  if((button == BUTTON_PREV) && (event == BUTTON_EVENT_LONG_PRESS))
  {
    if((power_get_state() == POWER_STATE_BACKLIGHT_OFF) ||
       (power_get_state() == POWER_STATE_REDUCED))
    {
      power_wake();
      return;
    }

    if(onekeyboost_is_active() != 0U)
    {
      onekeyboost_cancel();
    }

    if((power_get_state() == POWER_STATE_HEATER_OFF) ||
       (sm_is_heater_enabled() == 0U))
    {
      power_wake();
      (void)sm_set_heater_enabled(1U);
    }
    else
    {
      power_heater_off();
    }

    buzzer_play(BUZZER_SOUND_KEY);
    return;
  }
}

/**
  * @brief  main function.
  * @param  none
  * @retval none
  */
int main(void)
{
  /* 先执行最早的 PA6 心跳 + PB5 方波，证明 main 已经开始运行 */

  /* 再处理可能残留的 WDT，避免后续启动过程被复位 */
  boot_wdt_relax();

  /* system clock config. */
  wk_system_clock_config();

  /* config periph clock. */
  wk_periph_clock_config();

  /* nvic config. */
  wk_nvic_config();

  /* timebase config for
     void wk_delay_us(uint32_t delay);
     void wk_delay_ms(uint32_t delay); */
  wk_timebase_init();

  /* 串口尽早初始化，后续每个阶段都能打印 */
  /* 时钟/时基已运行，再给一次短闪并重装 WDT */
  wdt_counter_reload();

#if APP_ENABLE_DISPLAY
  /* ST7735S 160x80 横屏 4-line SPI 显示驱动 */
  display_init();
  display_backlight_set(1U);

  display_clear(DISPLAY_COLOR_BLACK);
  display_draw_string(2U, 2U, "Chill245", DISPLAY_COLOR_WHITE,
                      DISPLAY_COLOR_BLACK, 2U);
#else
#endif

  /* HMS 故障屏幕状态 */
  hms_init();

  /* 按键：PA15=MENU，PB3=PREV，PB4=NEXT */
  buttons_init();
  (void)buttons_register_callback(BUTTON_MENU, BUTTON_EVENT_CLICK,
                                  main_buttons);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_CLICK,
                                  main_buttons);
  (void)buttons_register_callback(BUTTON_PREV, BUTTON_EVENT_LONG_PRESS,
                                  main_buttons);
  (void)buttons_register_callback(BUTTON_NEXT, BUTTON_EVENT_CLICK,
                                  main_buttons);

  /* 电源管理：电压换算、震动休眠、保护、CH224A */
  power_init();

  /* 设置菜单 */
  menu_init();

  /* 主界面 / 曲线 / 自动轮换 */
  maindash_init();
  graph_init();
  autoroll_init(AUTOROLL_INTERVAL_MS);

  /* 初始化所有模块并启动 20 kHz 控制定时器 */
  /* 按 preset.h 生成冷端 NTC 动态查表 */
  temp_init();

  sm_init();

  /* 参数加载完成前先关加热；若启用了上电自动整定则保留整定状态 */
  if(sm_get_state() != SM_STATE_AUTOTUNE)
  {
    (void)sm_set_heater_enabled(0U);
  }

  /* LED 与蜂鸣器共用 TMR3，先初始化 LED 通道，再初始化蜂鸣器 */
  led_init();

  /* 蜂鸣器需要读取系统状态，放在 sm_init 之后 */
  buzzer_init();

  /* 热端校准：注册屏幕按键和调试命令 */
  tempcal_init();

  /* OneKeyBoost：NEXT 长按 / PREV 短按 */
  onekeyboost_init();

  /* 加载 Flash 参数并应用；无有效记录时使用模块默认值 */
  appcolors_init();
  eeprom_init();
  eeprom_apply();

  bootlogo_init();
  bootguard_init();
  if(hms_has_fatal() == 0U)
  {
    bootlogo_boot(g_eeprom_params.bootlogo_delay_s);
  }

  /* 主循环开始喂狗；控制中断停滞时会触发 HMS */
  sm_heater_watchdog_init();

  /* 应用参数后同步蜂鸣器状态，避免把上电参数变化当模式切换 */
  buzzer_sync_state();

  /* 先把主界面画完，再播放开机音，避免首帧阻塞拉长提示音 */
  if((hms_has_fatal() == 0U) && (bootguard_is_active() == 0U))
  {
    sm_status_t boot_status;
    sm_get_status(&boot_status);
    maindash_draw(&boot_status);
  }

  /* 开机提示音；HMS 致命错误时只保留故障提示 */
  if(hms_has_fatal() == 0U)
  {
          buzzer_play(BUZZER_SOUND_BOOT);
  }
  else
  {
  }

  uint8_t hms_was_active = 0U;
  uint8_t tempcal_was_active = 0U;

  while(1)
  {
    uint32_t now_ms;

    /* 后台 1 kHz：读取控制快照并处理调试串口 */
    /* 加热独立看门狗：控制序列停滞则强制关加热并进入 HMS */
    sm_heater_watchdog_task();

    now_ms = sm_get_uptime_ms();

    /* 按键扫描：PREV/NEXT 翻页；任意按键按下可唤醒 */
    buttons_update(now_ms);

    /* 菜单或 Boost 期间保持唤醒，避免休眠计时触发 */
    if((menu_is_open() != 0U) || (onekeyboost_is_active() != 0U))
    {
      power_notify_activity(now_ms);
    }

    /* 震动休眠、电压保护、输入电压更新 */
    power_update(now_ms);

    /* 快速调温保持 N 秒后写入 EEPROM */
    main_quick_setpoint_task(now_ms);

    /* 启动图上传：收到上位机命令时阻塞接收并写 Flash */
    if(bootlogo_poll() != 0U)
    {
      (void)wallpaper_prepare();
      maindash_invalidate();
    }

    /* OneKeyBoost：掉温检测、超时、异常退出 */
    onekeyboost_task(now_ms);

    /* 蜂鸣器音效推进与状态提示 */
    buzzer_task(now_ms);

    /* LED：运行呼吸 / 待机慢闪 / 故障快闪 */
    led_task(now_ms);

    /*
     * HMS 占用屏幕时不再绘制菜单/主界面。
     * 致命错误持续显示；自检 ALL OK 约 3 s 后自动返回。
     */
    if(hms_task(now_ms) != 0U)
    {
      if(menu_is_open() != 0U)
      {
        menu_force_close();
      }

      /* 保持背光，并确保故障时加热不会恢复 */
      power_wake();

      {
        sm_status_t hms_status;
        sm_get_status(&hms_status);
        if((hms_has_fatal() != 0U) ||
           (hms_status.fault != VERIFY_HEATER_FAULT_NONE) ||
           (hms_status.state == SM_STATE_FAULT) ||
           (power_get_fault() != POWER_FAULT_NONE))
        {
          bootguard_abort();
          (void)sm_set_heater_enabled(0U);
        }
      }

      hms_was_active = 1U;
    }
    else
    {
      if(hms_was_active != 0U)
      {
        hms_was_active = 0U;
        autoroll_request_redraw();
      }

      /*
       * HMS 已让出屏幕后才处理开机联锁，保证 HMS 永远在最上层。
       */
      if(bootguard_is_active() != 0U)
      {
        sm_status_t guard_status;

        power_notify_activity(now_ms);
        bootguard_task(now_ms);

        if(bootguard_is_active() != 0U)
        {
          wk_delay_ms(SM_SLOW_PERIOD_MS);
          continue;
        }

        sm_get_status(&guard_status);
        maindash_draw(&guard_status);
        autoroll_request_redraw();
      }

      if(tempcal_is_active() != 0U)
      {
        if(menu_is_open() != 0U)
        {
          menu_force_close();
        }

        /* 校准期间禁止休眠，保持背光和加热 */
        power_notify_activity(now_ms);
        tempcal_task(now_ms);
        tempcal_was_active = 1U;
      }
      else
      {
        if(tempcal_was_active != 0U)
        {
          tempcal_was_active = 0U;
          autoroll_request_redraw();
        }

        /* 菜单：长按 MENU 2s 进入；菜单打开时暂停轮换绘制 */
        menu_update(now_ms);

        if(menu_is_open() == 0U)
        {
          /* 主界面和图表自动轮换 */
          autoroll_update(now_ms);

          /* 自动整定完成后保存 PID 结果 */
          eeprom_task(now_ms);
        }
      }
    }

    wk_delay_ms(SM_SLOW_PERIOD_MS);
  }
}
