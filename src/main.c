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
#include "graph.h"
#include "autoroll.h"
#include "statemachine.h"

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

/* 非菜单界面按键仲裁 */
static void main_buttons(button_id_t button, button_event_t event)
{
  if((menu_is_open() != 0U) || (tempcal_is_active() != 0U))
  {
    return;
  }

  /* 短按 MENU：单按键轮换页面 */
  if((button == BUTTON_MENU) && (event == BUTTON_EVENT_CLICK))
  {
    autoroll_next();
    return;
  }

  /* 短按 NEXT：取消 Boost；加热关闭时恢复加热；不做页面切换 */
  if((button == BUTTON_NEXT) && (event == BUTTON_EVENT_CLICK))
  {
    /* 深度休眠的第一次 NEXT 只负责唤醒屏幕 */
    if(power_get_state() == POWER_STATE_BACKLIGHT_OFF)
    {
      return;
    }

    /* 短按 PREV 的 HEATER_OFF 状态屏幕还在，NEXT 直接唤醒并恢复加热 */
    if(power_get_state() == POWER_STATE_HEATER_OFF)
    {
      power_wake();
    }

    if(onekeyboost_is_active() != 0U)
    {
      onekeyboost_cancel();
      return;
    }

    if(sm_is_heater_enabled() == 0U)
    {
      if(sm_set_heater_enabled(1U) != 0U)
      {
        buzzer_play(BUZZER_SOUND_KEY);
      }
    }

    return;
  }

  /* 短按 PREV：只关闭加热器，屏幕保持；长按才关屏幕 */
  if((button == BUTTON_PREV) && (event == BUTTON_EVENT_CLICK))
  {
    if(onekeyboost_is_active() != 0U)
    {
      onekeyboost_cancel();
    }

    power_heater_off();
    buzzer_play(BUZZER_SOUND_KEY);
    return;
  }

  /* 长按 PREV：彻底关闭加热器和屏幕 */
  if((button == BUTTON_PREV) && (event == BUTTON_EVENT_LONG_PRESS))
  {
    if(onekeyboost_is_active() != 0U)
    {
      onekeyboost_cancel();
    }

    power_sleep_deep();
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
  eeprom_init();
  eeprom_apply();

  /* 参数和输入电压档位应用完成后再恢复加热；HMS 致命错误保持关闭 */
  if((hms_has_fatal() == 0U) &&
     (sm_get_state() != SM_STATE_AUTOTUNE))
  {
    (void)sm_set_heater_enabled(1U);
  }

  /* 主循环开始喂狗；控制中断停滞时会触发 HMS */
  sm_heater_watchdog_init();

  /* 应用参数后同步蜂鸣器状态，避免把上电参数变化当模式切换 */
  buzzer_sync_state();

  /* 先把主界面画完，再播放开机音，避免首帧阻塞拉长提示音 */
  if(hms_has_fatal() == 0U)
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

      /* 保持背光，并确保致命错误时加热不会恢复 */
      power_wake();

      if(hms_has_fatal() != 0U)
      {
        (void)sm_set_heater_enabled(0U);
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
