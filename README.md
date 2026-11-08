# Chill245 烙铁控制器（PlatformIO）

AT32F421G8U7 / 120 MHz / 64 KB Flash / 16 KB RAM。

## 时钟

硬件**没有外部晶振**，全系统使用内部时钟：

```text
HICK 8 MHz / 2 = 4 MHz  -> PLL x30 -> SCLK 120 MHz
AHB  /1 = 120 MHz
APB1 /1 = 120 MHz
APB2 /1 = 120 MHz
Flash wait = 3 WS
```

- PLL 源：`CRM_PLL_SOURCE_HICK`
- 不启用 HEXT / LEXT；LICK 仅作为内部低速时钟启用
- SysTick：AHB/8 = 1 MHz
- TMR1：120 MHz，ARR=5999 -> 20 kHz
- ADC：120 MHz / 6 = 20 MHz
- 显示使用 SPI1 硬件 4-line SPI（约 7.5 MHz）；保留 GPIO bit-bang 作为 `DISPLAY_USE_BITBANG=1` 回退
- USART1：115200 8N1，由 PCLK 分频

> HICK 是内部 RC，频率精度低于晶振；20 kHz PWM 和 ADC 采样会随 HICK 有少量频偏。当前代码没有 HEXT 初始化路径。

HICK 8 MHz
  └─ /2 = 4 MHz
       └─ PLL ×30 = 120 MHz SCLK
            ├─ AHB  /1 = 120 MHz
            ├─ APB1 /1 = 120 MHz
            └─ APB2 /1 = 120 MHz
Flash wait = 3 WS

## 构建与烧录

```bash
cd jbc245
pio run                 # 编译
pio run -t upload       # 烧录（板级默认 atlink，可在 platformio.ini 改 upload_protocol）
```

编译输出：

```text
.pio/build/genericAT32F421G8U7/firmware.elf
.pio/build/genericAT32F421G8U7/firmware.hex
.pio/build/genericAT32F421G8U7/firmware.bin
```

> `platformio.ini` 使用 `board_build.ldscript = linker/AT32F421x8_62K.ld`，程序区 **54 KB**，最后 **10 KB** 留给参数区和启动 Logo（2 KB 双扇区参数 + 8 KB Logo）。
>
> 全局编译使用 `-Os + -flto` 节省 Flash；`statemachine.c`、`pid.c`、`temp.c`、`adc.c`、`heater.c`、`verifyheater.c` 用 `#pragma GCC optimize("O2")` 保持 20 kHz 控制路径速度。

### 启动

- `APP_ENABLE_DISPLAY` 为 **1**：显示初始化和主界面刷新已恢复
- 串口调试系统已删除；PA9/PA10 改为启动图上传串口 USART1（115200 8N1）
- 保留 HardFault 快速闪 PA6、PLL 超时回退 HICK
- 若 PA6 快速连续闪：进入 `HardFault_Handler`，属于硬件异常
- PLL 锁定/切换超时会自动回退到 HICK 8 MHz，优先保证能启动
- 焊屏后 `APP_ENABLE_DISPLAY` 保持为 **1**

## 目录

```text
jbc245/
├── platformio.ini
├── include/
│   ├── adc.h / temp.h / verifyheater.h
│   ├── pid.h / heater.h / display.h
│   ├── statemachine.h / maindash.h / graph.h / autoroll.h / menu.h / eeprom.h
│   ├── led.h / hms.h / ui_icons.h / tempcal.h / onekeyboost.h
│   ├── at32f421_conf.h
│   ├── at32f421_int.h / at32f421_wk_config.h
│   └── wk_system.h / buttons.h / buzzer.h / powermanagement.h
├── linker/
│   └── AT32F421x8_62K.ld
├── src/
│   ├── main.c
│   ├── adc.c / temp.c / verifyheater.c / pid.c / heater.c / display.c
│   ├── statemachine.c / maindash.c / graph.c / autoroll.c / menu.c / eeprom.c
│   ├── led.c / hms.c / ui_icons.c / tempcal.c / onekeyboost.c
│   ├── at32f421_int.c / at32f421_wk_config.c / wk_system.c
│   ├── syscalls.c / sysmem.c
│   └── buttons.c / buzzer.c / led.c / hms.c / ui_icons.c / powermanagement.c
└── docs/
    └── hardware  map.epro2
```

ADC、温度、保护、PID、加热器、状态机、ST7735S 显示、主界面、曲线页面、按键、蜂鸣器、LED、HMS、电源管理和 Flash 参数保存已实现。

## 引脚

| 引脚 | 功能 | 说明 |
|---|---|---|
| PA0 | T-NTC，NTC 温度补偿/冷端 | ADC_IN0 |
| PA1 | T-ADC，热电偶放大信号 | ADC_IN1 |
| PA2 | V-ADC，输入电压采样 | ADC_IN2，10k/1k 分压（VCC/11），见电源管理 |
| PA3 | ST7735S 背光 | 高电平点亮（默认） |
| PA4 | ST7735S CS | 软件片选，低有效 |
| PA5 | ST7735S SCLK | SPI1_SCK，MUX_0 |
| PA7 | ST7735S MOSI | SPI1_MOSI，MUX_0 |
| PB0 | ST7735S RS / DC | 低=命令，高=数据 |
| PA6 | 运行指示灯 LED2 | TMR3_CH1，MUX_1，高电平点亮（10k 限流） |
| PB1 | 震动开关 SLEEP | 外部 10k 上拉，静止为高，震动拉低 |
| PB5 | 蜂鸣器 | TMR3_CH2，MUX_1，高电平经 Q7 NPN 驱动 |
| PA15 | MENU / ENTER 按键 | 内部上拉，按下拉低 |
| PB3 | PREV 按键 | 内部上拉，按下拉低 |
| PB4 | NEXT 按键 | 内部上拉，按下拉低 |
| PB6 | CH224A I2C_SCL | I2C1_SCL，MUX_1，外部 1k 上拉 |
| PB7 | CH224A I2C_SDA | I2C1_SDA，MUX_1，外部 1k 上拉 |
| PA8 | 加热器栅极驱动（Q8/Q5） | PWM 低电平导通 Q6 PMOS |
| PA9 | 未使用 | 原调试串口 TX，调试系统已删除 |
| PA10 | 未使用 | 原调试串口 RX，调试系统已删除 |

ADC 采用单通道逐路软件触发：PA0 -> PA1 -> PA2。每次转换前清除 CCE、切换通道并明确调用 `adc_ordinary_software_trigger_enable()`，不再反复关闭/开启 ADC。校准恢复为完整流程：ADC 使能、tSTAB、ADCALINIT、ADCAL。首次转换失败会打印一次 `[ADC] conv TIMEOUT` 和 `ctrl1/ctrl2/sts` 寄存器值。

## 显示驱动

[display.c](src/display.c:1) 驱动 ST7735S，**160(H) x 80(V) 横屏**，RGB565，4-line SPI。

- 显示：PA5=SCL，PA7=SDA，SPI1 硬件约 7.5 MHz，mode 0，MSB first；主界面 7 步非阻塞刷新 + 脏值缓存，只重画变化项
- PA4=CS，PB0=RS/DC，PA3=背光
- 默认横屏：`MADCTL=0xA8`，X offset=-2（左侧裁剪），Y offset=24
- 硬件 SPI 命令与参数之间释放 CS（参考 `ST7735S-master`）；bit-bang 回退约 1.7 MHz
- 若横屏镜像，可把 `DISPLAY_MADCTL_VALUE` 改为 `0x68`

> 硬件 SPI1 采用 `SPI_MCLK_DIV_16`（7.5 MHz）；如果长排线仍花屏，可先降到 `SPI_MCLK_DIV_32/64`，或把 `DISPLAY_USE_BITBANG` 改回 `1`。
- `display_init()` 初始化 SPI/GPIO 和面板，背光默认关闭
- `main.c` 已注册：
  ```c
  display_init();
  display_clear(DISPLAY_COLOR_BLACK);
  display_backlight_set(1U);
  ```
- 已提供 UI 基础接口：
  - `display_fill` / `display_clear`
  - `display_fill_rect`
  - `display_draw_pixel`
  - `display_draw_hline` / `display_draw_vline`
  - `display_set_window` + `display_write_pixels`（位图/批量）
  - `display_write_command` / `display_write_data`（扩展命令）

如果显示偏移，修改 `display.c` 的 `DISPLAY_X_OFFSET` / `DISPLAY_Y_OFFSET`；如果镜像或红蓝反了，修改 `DISPLAY_MADCTL_VALUE`；如果颜色整体反相，修改 `DISPLAY_INVERT_COLORS`；背光有效电平改 `DISPLAY_BL_ACTIVE_HIGH`。

> SOC0.96-A FPC：3=SDA、4=SCL、5=RS、6=RESET、7=CS、8=GND、10=VDD、11=LEDK、12=LEDA、13=GND。板上 RESET 直接接 3.3V，只能使用 SWRESET；PB2 不参与显示复位。

## 显示 UI

[maindash.c](src/maindash.c:1) 主界面，**160x80 横屏**：

```text
[====== 45% 功率进度条 ======]  <- 顶栏同时显示功率百分比
Chill245                       RUN
 300C        SET   300C
             MODE  STD
HEATING        SLEEP ● 300s
```

- 顶栏整条是**功率进度条**，按 `status->power` 填充，同时叠印标题、运行状态和功率百分比。
- 左侧只显示热端温度：3 位 **20×34 MV Boli 字形** + `C`，不再显示功率数字。
- 右侧一次显示两行，数值使用 **3 倍字体**，每 2 秒轮换：
  - `SET` / `MODE`
  - `VIN` / `COLD`
- 底部保留工作状态、错误信息和 `SLEEP` 遥测。
- 页面指示条仍在最底部 y=77。

### About 页面

`SYSTEM -> ABOUT` 打开关于页：

- 左侧为项目地址二维码（33×33 QR，2px 模块，白色静区）
- 右侧显示：
  - `CHILL245`
  - `SOLDERING`
  - 作者 `COCORO-CCR-01`
  - 版本 `Release-00.15.0020`
  - `ISSUES WELCOME`
- 任意按键返回 SYSTEM 列表
- 版本和作者定义在 [version.h](include/version.h)
- 二维码数据在 [about_qr.h](include/about_qr.h)

### 屏幕方向 / 浅色模式 / 按键

- `DISPLAY / UI -> ROTATE 180`：0/180° 旋转，支持左右手。
  - 0°：MADCTL `0xA0`，偏移 (-2, 24)
  - 180°：MADCTL `0x60`，偏移 (2, 28)
  - 设置保存到 EEPROM，菜单关闭后立即应用。
- 浅色模式使用独立默认配色：
  - primary 深红
  - secondary 深蓝
  - tertiary 深灰
  - quaternary 近黑
  - quinary 近白
  - 不依赖壁纸也有足够对比度。
- PREV 与 MENU 的物理按键功能已交换：
  - 原 PREV 位置的按键执行 MENU
  - 原 MENU 位置的按键执行 PREV
  - NEXT 不变

### 启动 Logo / 壁纸 / 主题色

启动图与 maindash 壁纸共用 Flash 末尾同一张 8 KB 图片；上传壁纸也会更新启动图。

```text
0x0800D800 ~ 0x0800DBFF   参数记录 0（1 KB 扇区）
0x0800DC00 ~ 0x0800DFFF   参数记录 1（1 KB 扇区）
0x0800E000 ~ 0x0800FFFF   启动图 / 壁纸（8 KB）
```

- 分辨率固定 **160×80**，**16 色**，4bpp 打包后 6400 B。
- 使用 PackBits RLE 压缩，数据上限 **8064 B**；最坏随机图约 6450 B，仍有容量余量。
- 每张图片包含 16 色 RGB565 调色板和 CRC32。
- 没有有效图片时 `bootlogo_is_valid()` / `wallpaper_is_available()` 返回 0，开机不显示，菜单延时设置也不会生效。
- 菜单 `DISPLAY / UI -> BOOT LOGO` 设置开机显示秒数，范围 0~10 s，默认 0；设置值不会再被图片校验/参数保存清零。
- 开机显示在 `eeprom_apply()` 之后、恢复加热之前，显示期间不加热。
- [wallpaper.c](src/wallpaper.c) 把同一张图片以 **8/16 亮度**绘制到 maindash，避免壁纸压过前景。
- [appcolors.c](src/appcolors.c) 提供五级主题色：
  - `primary`：温度、进度、主要强调
  - `secondary`：标签、次级信息
  - `tertiary`：状态、错误
  - `quaternary`：正文数值
  - `quinary`：面板底色，自动压暗为 surface
- 新版和旧版 maindash 都使用 appcolors；所有信息文字改为透明前景 + 1px 阴影，壁纸从字形内部和背景透出。
- 只有进度条和分隔线保留实色；信息行不再使用整块不透明面板。
- maindash 改为脏刷新：温度、SET/NOW、右侧两行、底部状态和功率条分别只重绘自己的矩形；壁纸变化/翻页时才整帧重绘。
- DISABLED 闪烁期间不再同时触发温度重绘，避免 DISABLED 和温度轮流出现。
- appcolors 已全局应用到 maindash、菜单、曲线页、页面指示、温度校准页和 HMS 故障页。
- 上位机 Monet 固定使用深色 UI：高亮强调 + 近白正文 + 深色 quinary surface，不再因亮壁纸自动把深色模式变浅。
- 壁纸压暗到 8/16；温度数字改为近白色，正文使用近白 + 双向 1px 阴影，提高壁纸上的对比度。
- 透明文字根据前景亮度自动选择近黑或近白阴影。
- 壁纸解码到 RAM 的 4bpp 缓存（6400 B），脏刷新用 `wallpaper_blit()` 恢复局部背景，不再每 250 ms 全屏重画。
- 180° 旋转下壁纸按 32×8 小块绘制，启动图按每行 32px 小块绘制，避免 MV/MX 大窗口在面板边界回卷造成倾斜拉伸。
- 壁纸/启动图使用 **raw 像素流**，绕过暗/亮主题颜色映射，避免图片颜色被主题改写。
- 面板存在红/蓝通道互换：`DISPLAY_SWAP_RB` 默认为 **1**，在 display.c 最底层把所有 RGB565 输出做 R/B 交换；MADCTL 仍保持 `0xA0`，不退回旧方向配置。
- 默认三级色改为中性灰，避免默认界面标签变成绿色；透明文字阴影使用 0x0001 近黑，避免亮色主题把阴影映射成白色。

#### 上位机

用浏览器打开：

[tools/bootlogo_uploader.html](tools/bootlogo_uploader.html)

- Chrome / Edge 的 Web Serial API，页面必须运行在 `https://` 或 `localhost`。
- 载入图片后可拖动 2:1 裁剪框、拖动右下角缩放、滚轮缩放。
- 导出时严格缩放到 160×80，做 16 色中位切分量化，再 PackBits RLE 压缩。
- 两种写入模式：
  - **壁纸 + 主题色**：默认用类似 Monet 的方式从壁纸提取五级色，也可逐级自定义。
  - **仅启动 Logo**：只更新图片，不改主题色。
- USB-TTL 接 `PA9`（TX）、`PA10`（RX）、`GND`，115200 8N1。
- 上传会擦除 8 KB Logo 区，期间强制关闭加热；完成后恢复。

#### 上传协议

```text
仅 Logo:
Host -> 'L'
Device 擦除图片区 -> 'R'
Host -> 40 B meta: data_len(LE32), crc32(LE32), palette[16](LE16)
Device -> 'M'

壁纸:
Host -> 'W'
Device 擦除图片区 -> 'R'
Host -> 50 B meta: data_len, crc32, colors[5](LE16), palette[16](LE16)
Device -> 'M'

公共:
Host -> 128 B chunks, 每包等待 0x06 ACK
Host -> 0x04 EOT
Device -> 'K'（失败返回 'E'）
```

`data_len` 为实际 RLE 字节数，最后一包补 0 到 128 B；CRC32 只覆盖实际数据。

壁纸模式使用 `'W'` 命令，meta 为 50 B：`data_len(4) + crc32(4) + colors[5](10) + palette[16](32)`。五级色写入 EEPROM，EEPROM 格式版本升到 **11**。

### 新版全屏主界面

在 `DISPLAY / UI -> ALT DASH` 中切换，旧主界面完整保留。

```text
NOW
 300
    MODE STD
[■■■■■■■■■■        ]
```

- 热端温度使用独立生成的 **46×56 大号 MV Boli 字形**（2bpp 抗锯齿），所有数字统一字高，不再由小字体拉伸。
- 所有主界面默认使用**纯黑背景**；`DISPLAY / UI -> LIGHT MODE` 可切换浅色主题（黑底变白底、白字变黑字，强调色重新映射以保证可读性）。
- 左上角 `NOW` / `SET` 使用 2 倍字体；快速调温后 2 秒内显示 `SET`，数字切换为设定温度。
- 左下角每 2 秒轮换：PID 模式 / 冷端温度 / 输入电压，使用 2 倍字体。
- 有保护故障或电源故障时，左下角固定显示错误文本，不再轮换。
- 最下方 y=78、高 2 px 的进度条直接显示**当前加热功率 0~100%**，与上方文字留出间隔。
- 休眠、加热关闭或 `SM_STATE_DISABLED` 时，主区域显示闪烁的 `DISABLED`，底部信息和进度条仍正常刷新，`DISABLED` 使用灰色以区别于粉色温度。
- `ALT DASH` 设置写入 EEPROM。

[graph.c](src/graph.c:1) 曲线页：

| 页面 | 内容 |
|---|---|
| `TEMP/PWR` | 热端温度（红）+ 功率占比（绿）同一图表，温度 0~450°C，功率 0~100% |
| `VOLTAGE` | 输入电压曲线（青），自动量程 |
| `COLD` | 冷端温度曲线（黄），自动量程 |

- 80 点历史，250 ms/点，约 20 秒窗口
- 曲线每 250 ms 重绘
- 绘图区使用 **4bpp 离屏缓冲（125×49）**
- 每帧整块重写绘图区：一次 `display_set_window(125×49)` + 一次 RAMWR，逐行连续送像素
- 不再做“逐行比较、只写变化行”的差分刷新，彻底消除旧曲线残留/拖影
- 标题、坐标标签、底部区域只在进入曲线页/换页时清一次，之后用定宽空格覆盖
- 顶部标题保持 2 倍字体：`TEMP/PWR` / `VOLTAGE` / `COLD`
- 坐标轴文字放大到 **2 倍字体**：左侧 Y 轴数值右对齐，底部 `20S` 也放大
- 右上角固定 8 字符显示当前量：温度+功率 / 当前电压 / 当前冷端温度
- 图表左侧显示当前量程，底部显示 20 秒窗口

[autoroll.c](src/autoroll.c:1) 页面管理已改为**纯手动**，定时自动翻页已删除：

```c
autoroll_next();               /* MENU 短按：下一页 */
autoroll_prev();               /* PREV 短按：上一页 */
autoroll_set_page(AUTOROLL_PAGE_VOLTAGE);
```

页面顺序：主界面 → TEMP/PWR → VOLTAGE → COLD → 主界面。

## 设置菜单

[menu.c](src/menu.c:1) 实现 160x80 横屏多级菜单：

- **MENU 长按 2 s**：进入菜单
- **MENU 单击**：进入子菜单 / 确认编辑
- **PREV / NEXT**：移动选择 / 修改数值
- **主界面短按 PREV/NEXT**：按 `TEMP STEP` 减/加设定温度；Boost 状态下 NEXT 只退出 Boost，PREV 不改温；菜单内不触发
- **MENU 长按 2 s**：返回上一级；根页面关闭菜单
- 菜单打开时暂停页面切换，并阻止休眠计时
- 菜单数值使用整数缩放格式化，不依赖 newlib 浮点 printf
- 配色：深色标题栏 + 橙色选中条 + 深青选中行 + 琥珀色数值

分类：

```text
SETTINGS
├── TEMPERATURE    设定温度、PID 模式、Kp/Ki/Kd、自定义曲线、温度标定、整定
├── BOOST          Boost 温度增量、掉温阈值、超时、设置步进
├── SLEEP / POWER  休眠时间、休眠温度、关加热/背光时间、SLEEP NOW、CH224A、电压保护
├── PROTECTION     过温、温跳、升温失败、满功率、失温阈值
├── DISPLAY / UI   蜂鸣器开关/音量、回主界面、ALT DASH、LIGHT MODE
└── SYSTEM         整定、设定/时间步进、加热开关、清故障、HMS 自检、热端校准、恢复默认、退出
```
### 10 点曲线

- **CUSTOM PID**：10 个温度点，每点 `温度 + Kp + Ki + Kd`，运行时按当前温度线性插值
- **TEMP CAL**：10 个 `ADC + 实际温度` 点，启用后替代线性热电偶换算，并保留冷端补偿

曲线编辑：

1. 在曲线列表用 PREV/NEXT 选点，MENU 进入点编辑
2. 选 `TEMP / KP / KI / KD`（标定曲线为 `ADC / TEMP`）
3. MENU 进入数值编辑，PREV/NEXT 调整，MENU 确认
4. 长按 MENU 返回

曲线点的温度/ADC 会自动限制在前后点之间，保证严格递增。

休眠时间、关加热/背光时间单位均为 **秒**，步进 **5 s**。

> 菜单关闭时会应用设置并写入 Flash 参数区；自动整定完成后也会自动保存 PID 结果。


## 参数保存

[eeprom.c](src/eeprom.c:1) 使用片内 Flash 最后 **2 KB** 保存全部设置：

```text
sector 62: 0x0800F800 ~ 0x0800FBFF
sector 63: 0x0800FC00 ~ 0x0800FFFF
```

- 自定义链接脚本 [AT32F421x8_62K.ld](linker/AT32F421x8_62K.ld:1) 把程序区限制为 **54 KB**
- 双扇区交替写入，始终保留上一份有效记录
- 记录包含 `magic + version + size + sequence + CRC32 + commit`
- 写入前 `heater_force_off()`，并短暂关闭中断，避免 Flash 操作期间加热
- 参数没有变化时不擦写 Flash
- 自动整定完成后自动保存 PID 结果
- 上电先关闭加热，参数和 CH224A 档位应用完成后再恢复加热

保存内容：设定温度、PID 模式/标准参数、10 点自定义 PID 曲线、10 点温度标定、休眠/电压/CH224A、保护阈值、蜂鸣器开关/音量、Boost 增量/掉温阈值/超时、设置步进。

> 当前参数格式为 **v5**。旧 v1~v4 记录会按版本不匹配处理，首次升级使用默认值。固件下载/整片擦除会清掉参数区；正常复位不会。

- CRC 损坏但 Flash 可写：自动写回默认值并显示 `EEPROM RECOVERED`
- Flash 写失败：HMS `EEPROM WRITE`，持续显示并强制关闭加热
- `SYSTEM -> DEFAULTS`：连续按两次，擦除两个参数扇区并复位，恢复出厂设置

[main.c](src/main.c:63) 注册：

```c
hms_init();
buttons_init();
power_init();
menu_init();

maindash_init();
graph_init();
autoroll_init(5000U);
sm_init();
sm_set_heater_enabled(0);

led_init();
buzzer_init();
tempcal_init();
onekeyboost_init();

eeprom_init();
eeprom_apply();

if(hms_has_fatal() == 0U)
{
  sm_set_heater_enabled(1);
}

sm_heater_watchdog_init();

buzzer_sync_state();
if(hms_has_fatal() == 0U)
{
  buzzer_play(BUZZER_SOUND_BOOT);
}

while(1)
{
  uint32_t now_ms;

  sm_slow_update();
  sm_heater_watchdog_task();
  now_ms = sm_get_uptime_ms();

  buttons_update(now_ms);

  if((menu_is_open() != 0U) ||
     (onekeyboost_is_active() != 0U))
  {
    power_notify_activity(now_ms);
  }

  power_update(now_ms);
  onekeyboost_task(now_ms);
  buzzer_task(now_ms);
  led_task(now_ms);

  if(hms_task(now_ms) != 0U)
  {
    if(menu_is_open() != 0U)
    {
      menu_force_close();
    }

    power_wake();
    if(hms_has_fatal() != 0U)
    {
      sm_set_heater_enabled(0);
    }
  }
  else
  {
    if(tempcal_is_active() != 0U)
    {
      if(menu_is_open() != 0U)
      {
        menu_force_close();
      }

      power_notify_activity(now_ms);
      tempcal_task(now_ms);
    }
    else
    {
      menu_update(now_ms);

      if(menu_is_open() == 0U)
      {
        autoroll_update(now_ms);
        eeprom_task(now_ms);
      }
    }
  }

  wk_delay_ms(SM_SLOW_PERIOD_MS);
}
```

电压换算已移到 [powermanagement.c](src/powermanagement.c:254)，公式：

```text
V-ADC = VCC * 1/(10+1)
VCC   = V-ADC * 11
```

对应 [powermanagement.h](include/powermanagement.h:11) 的 `POWER_VIN_DIVIDER_R1/R2`、`POWER_VIN_VREF_MV`。修改分压比只改这三个宏。

全屏刷新约 13~20 ms，控制 20 kHz 中断仍会正常抢占；图表和主界面分别约 2 Hz/5 Hz 刷新。

## 电源管理

[powermanagement.c](src/powermanagement.c:1) 负责输入电压、震动休眠、电源保护和 CH224A。

### 自动休眠/关机

PB1 接震动开关，外部 10k 上拉，静止为高、震动时拉低。状态机：

```text
ACTIVE --静止 5 min--> REDUCED（降温到 150°C）
REDUCED --继续静止 10 min--> HEATER_OFF（关闭加热）
HEATER_OFF --继续静止 5 min--> BACKLIGHT_OFF（关闭背光）
```

- `REDUCED` 状态检测到震动会恢复原目标温度并回到 `ACTIVE`
- `HEATER_OFF` 状态检测到震动会直接唤醒并恢复休眠前加热状态
- 手动 `HEATER OFF` / `SM_STATE_DISABLED` 状态检测到震动会直接恢复加热
- `BACKLIGHT_OFF` 状态只能按键唤醒
- 任意按键 `PRESS` 都会唤醒；是否恢复加热取决于进入休眠的方式
- 按键唤醒由 `power_init()` 自动注册到 `buttons.h`
- 菜单 `SLEEP / POWER -> SLEEP NOW` 可立即手动休眠；也可调用 `power_sleep()`
- 主界面长按 PREV 在“加热器开/关”之间切换：关闭时只关加热，屏幕保持；再次长按恢复加热
- 深度休眠改由菜单 `SLEEP NOW` / 自动休眠触发
- 菜单 SLEEP NOW / 自动休眠会关闭加热和背光，唤醒后恢复休眠前设定温度和加热状态
- 休眠时 **NEXT 短按** 唤醒；NEXT 不再用按下沿唤醒，避免按下瞬间误触发。MENU/PREV 仍按下唤醒

可修改参数：

```c
power_set_stationary_timeout_ms(300000U);
power_set_reduced_temperature(150.0f);
power_set_reduced_off_timeout_ms(600000U);
power_set_backlight_off_timeout_ms(300000U);
```

### 电压保护

默认欠压 **18.0 V**、过压 **30.0 V**，持续 100 ms 锁存保护；电压回到安全区并保持 2 s 后自动恢复：

```c
power_set_voltage_limits(18.0f, 30.0f);
power_set_voltage_hysteresis(1.0f);
power_set_voltage_trip_time_ms(100U);
power_set_voltage_recovery_time_ms(2000U);
```

保护动作：立即 `sm_set_heater_enabled(0)` 关闭加热。主界面和蜂鸣器直接轮询 `power_get_fault()`；故障回调为可选扩展：

```c
static void on_power_fault(power_fault_t fault, uint16_t voltage_mv)
{
  /* 蜂鸣器 / UI 报错 / 记录日志 */
}

power_register_fault_callback(on_power_fault);
```

故障也可手动清除：

```c
if(power_clear_fault() == 0U)
{
  /* 电压仍不在安全范围，清除失败 */
}
```

查询接口：

```c
power_get_voltage_mv();   /* 最近一次输入电压 */
power_get_state();        /* ACTIVE / REDUCED / HEATER_OFF / BACKLIGHT_OFF */
power_get_fault();        /* NONE / UNDERVOLTAGE / OVERVOLTAGE */
power_wake();             /* 手动唤醒 */
```

- 欠压/过压时主界面顶部显示 `FLT`，底部显示 `V LOW` / `V HIGH`，VIN 数值变红
- 进入休眠前会记录加热器是否开启；唤醒时只恢复原本开启的加热，不会把用户手动关闭的加热重新打开
- 电压故障恢复后按故障前的加热状态决定是否恢复输出

上电/CH224A 重新协商有 2 s 保护屏蔽期，避免 5V 起步阶段误触发欠压。

### CH224A 电压诱骗

- I2C1：**PB6=SCL，PB7=SDA**，MUX_1，100 kHz，外部 1k 上拉
- CH224A 7 位地址：**0x22 或 0x23**，初始化时自动探测
- 电压寄存器：**0x0A**
- 写入值：`0=5V, 1=9V, 2=12V, 3=15V, 4=20V, 5=28V`

```c
power_ch224a_set_voltage(20.0f);
power_ch224a_set_voltage_index(4U);   /* 直接指定档位 */
```

> CH224A 固定档位没有 24V；请求 24V 会落到 20V 档。默认初始化到 20V。

## 按键

[buttons.c](src/buttons.c:1) 扫描三个低电平按键：

| 按键 | 引脚 | 功能 |
|---|---|---|
| MENU / ENTER | PA15 | 非菜单短按轮换页面；长按 2 s 进入菜单；菜单内单击确认 |
| PREV | PB3 | 短按关加热并休眠；长按深度休眠（关加热+屏幕）；菜单内上移、减小 |
| NEXT | PB4 | 长按启动 Boost；短按取消 Boost；休眠时短按唤醒；菜单内下移、增大 |

- 内部上拉，按下拉低
- 20 ms 消抖
- 长按 2000 ms
- 长按后 200 ms 连发
- 事件：`PRESS` / `RELEASE` / `CLICK` / `LONG_PRESS` / `REPEAT`

注册回调：

```c
static void my_button(button_id_t button, button_event_t event)
{
  if((button == BUTTON_MENU) && (event == BUTTON_EVENT_CLICK))
  {
    /* 打开菜单 */
  }
}

buttons_register_callback(BUTTON_MENU,
                          BUTTON_EVENT_CLICK,
                          my_button);
```

`buttons_init()` 会清空回调表，所以注册必须放在初始化之后。

当前按键回调：

- [menu.c](src/menu.c:1)：菜单打开时 MENU/PREV/NEXT 的 `CLICK`，PREV/NEXT 的 `REPEAT`
- [main.c](src/main.c:46)：非菜单界面的 MENU/PREV/NEXT 仲裁
- [onekeyboost.c](src/onekeyboost.c:1)：NEXT 长按启动 Boost；NEXT 短按由主按键仲裁处理
- [powermanagement.c](src/powermanagement.c:1)：三个按键的 `PRESS` 唤醒、NEXT 短按休眠唤醒
- autoroll 不再注册按键回调；没有定时自动翻页

非菜单界面按键：

| 操作 | 功能 |
|---|---|
| MENU 短按 | 单按键轮换页面 |
| MENU 双击 | 直接打开 SETPOINT 快速编辑，退出后回主界面 |
| MENU 长按 2 s | 进入菜单 |
| NEXT 长按 | 启动 Boost |
| NEXT 短按 | Boost 状态下只退出 Boost，不调温；非 Boost 状态增加一个 `TEMP STEP` |
| PREV 短按 | 减少一个 `TEMP STEP`；Boost 状态下不动作 |
| PREV 长按 | 关闭/恢复加热器，屏幕保持；不再关闭屏幕 |

菜单打开时，PREV/NEXT 仍用于菜单导航和数值调整。

> PA15/PB3/PB4 在部分 AT32 型号上是 JTAG 引脚。AT32F421 使用 SWD 调试时这三个脚可作 GPIO；如果调试器占用了它们，需要把调试口配置为 SWD-only。

## 蜂鸣器

[buzzer.c](src/buzzer.c:1) 使用 **PB5 / TMR3_CH2（MUX_1）** 驱动蜂鸣器：

- 原理图：PB5 -> R35 -> Q7 MMBT3904 NPN -> 4 kHz 蜂鸣器 -> 3V3
- **高电平导通**
- TMR3 计数 1 MHz，频率 0.5~5 kHz
- 音量 0~100：0 静音，100 对应 50% PWM 占空比最大音量
- 所有音效由 `buzzer_task()` 每 1 ms 非阻塞推进，不使用 `delay`
- 高优先级音效可打断低优先级；故障音最高

| 音效 | 触发 | 节奏/音调 |
|---|---|---|
| 开机 | 上电初始化完成 | 2000/2600/3200/4000 上行 |
| 按键 | 任意按键按下 | 4000 短鸣 18 ms |
| 到达预设温度 | 加热中首次进入 `SET-3°C` | 2600/3200/4000 上行三连 |
| 休眠 | 进入降温休眠 | 3000 长、2200 更长，下行 |
| 深度休眠 | 关闭加热器 | 1800 双短 + 1200 长 |
| 关机 | 关闭背光 | 2500/1900/1200 下行长音 |
| 唤醒 | 从任一休眠回到 ACTIVE | 1800/2400/3200 上行 |
| 故障 | 保护/电源故障 | 3800/1900 交替，每 5 s 重复 |
| 切换模式 | STD/BOOST/CUSTOM 改变 | 2800/3600 双音 |
| 打开菜单 | 菜单打开 | 2200/3000/3800 |
| 关闭菜单 | 菜单关闭 | 3200/2200 |
| 整定开始 | 自动整定 RUNNING | 2000/2600/3200 |
| 整定完成 | DONE | 3000/4000 |
| 整定失败/取消 | FAILED 或取消 | 1500/900 下行 |
| 操作错误 | 例如整定启动失败 | 1200/800 低音 |

- 到达温度每次“从低于 `SET-10°C` 重新升温到 `SET-3°C`”只响一次
- 故障持续期间每 5 s 重复警报
- 菜单 `DISPLAY / UI -> BUZZER` 可开关，`VOLUME` 可调 0~100，设置写入 Flash
- 若实际是有源蜂鸣器，音调变化会变成节奏差异；当前按无源 4 kHz 蜂鸣器设计

## 指示灯

[led.c](src/led.c:1) 驱动 PA6 LED2：

- 原理图：PA6 -> R66 10k -> LED2 -> GND，**高电平点亮**
- PA6 = TMR3_CH1，与蜂鸣器 TMR3_CH2 共用 ARR
- 蜂鸣器改变音调时调用 `led_refresh_pwm()`，LED 占空比保持
- 运行（ACTIVE 且加热开启）：呼吸，60% 最大亮度
- 待机/休眠/加热关闭：1.5 s 周期慢闪
- 故障/HMS：250 ms 周期快闪

## HMS 与加热看门狗

[hms.c](src/hms.c:1) 是持续故障屏幕：

- 不可恢复错误：EEPROM 写失败、加热控制看门狗触发
- HMS 占用屏幕后不再绘制菜单和主界面，并强制关闭加热
- EEPROM CRC 损坏但 Flash 仍可写：自动写回默认值，显示约 3 s `EEPROM RECOVERED`
- 设置 `SYSTEM -> HMS SELF TEST`：无故障显示 `ALL OK`；有可恢复故障显示 `FAULT ACTIVE`

加热看门狗：

- 20 kHz 控制中断维护 `sm_fast_sequence`
- 主循环 `sm_heater_watchdog_task()` 若约 80 次循环没有看到序列变化，立即 `heater_force_off()` 并进入 HMS
- 主循环重装硬件 WDT；`SM_ENABLE_HARDWARE_WDT` 为 **1**，超时约 4 s
- 启动时 `boot_wdt_relax()` 会先放宽并重装可能残留的 WDT，避免上电复位循环
- 硬件 WDT 使用 LICK，独立于 120 MHz 系统时钟

## OneKeyBoost

[onekeyboost.c](src/onekeyboost.c:1)：

- **NEXT 长按**：切到 BOOST 曲线，设定温度增加 `BOOST TEMP`
  - 默认 +100°C，范围 0~200°C
  - 若重复长按，只重新计时，不重复叠加
- **温度突降自动 Boost**
  - 只有当前温度 **低于设定温度** 时才允许自动 Boost
  - 正常加热时，温度在约 1 s 内相对峰值下降超过 `DROP TEMP`
  - 默认 20°C，范围 5~100°C
  - 掉温需持续 **150 ms** 才确认，过滤单次采样尖峰
  - 自动抬高 `BOOST TEMP` 并切 BOOST 曲线
- **NEXT 短按**：退出 Boost，恢复原设定温度和原 PID 模式
- **退出冷却**：手动/自动退出后 **5 s 内禁止掉温自动重入**，避免刚退出又立刻 Boost；NEXT 长按仍可手动立即重新进入
- **超时自动退出**：默认 300 s，可设 10~1800 s
- 菜单打开、热端校准、HMS、故障、休眠、加热关闭都会退出/禁止 Boost
- Boost 期间阻止休眠计时，主界面显示 `BST` / `BOOST`

菜单：

```text
BOOST
├── BOOST TEMP   温度增量
├── DROP TEMP    掉温触发阈值
└── BOOST TIME   超时秒数

SYSTEM
├── START AUTOTUNE
├── CANCEL TUNE
├── TEMP STEP    设定温度步进 1~100°C
├── TIME STEP    秒级时间步进 1~600s
├── ...
```

- 主界面热端温度和设定温度固定按 1°C 分度显示，不做额外取整
- `TEMP STEP` 对所有单位为 `°C` 的温度项生效：设定温度、休眠温度、保护温度、Boost 温度、掉温阈值等
- `TIME STEP` 只作用于单位为 `s` 的时间项（休眠时间、关加热时间、关屏时间、Boost 时间、保护时间等）
- `TEMP STEP` / `TIME STEP` 自己始终按 1 调整
- **双击 MENU**：直接打开 `TEMPERATURE -> SETPOINT` 编辑，NEXT/PREV 按 `TEMP STEP` 调整，MENU 确认并退出
- 电压、PID、ms 级时间等其它项保留原来的物理步进
- 长按 NEXT 启动 Boost；Boost 状态下短按 NEXT 退出 Boost；非 Boost 状态短按 PREV/NEXT 调整设定温度
- 加热关闭（DISABLED/HEATER_OFF）时短按 NEXT 恢复加热；深度休眠的第一次 NEXT 只唤醒屏幕，第二次恢复加热

## 加热器输出

- 功率管 **Q6 = S-LP7413DT3WG，P 沟道 40V/47A**，源极接 VCC，漏极接加热端。
- PA8 复用为 **TMR1_CH1**（GPIO_MUX_2），经 **Q8 2N7002W + Q5 MMBT3904** 栅极驱动网络控制 Q6，实测 **高电平导通、低电平关断**。
- 注意：Q8 是栅极驱动 NMOS，不是加热功率管；Q8/Q5 两级驱动后 PA8 极性已反相，以实测极性为准。
- `heater_init()` 先用 GPIO 把 PA8 置低，再配置 TMR1 为 20 kHz PWM，最后把 PA8 的 GPIO 模式改为 `GPIO_MODE_MUX` 并写 MUX；只调用 `gpio_pin_mux_config()` 不会切换模式，TMR1_CH1 不会输出到引脚。
- PWM 参数：
  - 频率：**20 kHz**（50 µs 周期）
  - 计数时钟：120 MHz，ARR = 5999，CCR = 0~6000
  - 0%：CCR=0，PA8 恒低，Q6 关断
  - 100%：CCR=6000，PA8 恒高，Q6 导通
  - 直接写 CCR，20 kHz 每周期的脉宽由占空比决定
  - 占空比分辨率约 0.017%
- 保护模块故障时 `statemachine.c` 调用 `heater_force_off()`，下一拍 CCR=0。

> 硬件注意：PA8 只驱动 Q8/Q5 栅极网络，不是直接接 24V PMOS 栅极。必须确认 Q5 的集电极/上拉能把 Q6 栅极可靠拉到 VCC 关断；如果控温阶段 MOS 仍然明显发热，需要用示波器看 Q6 栅极波形，重点检查栅极电阻、Q8/Q5 驱动能力和 PMOS 是否完全增强。

## 当前已实现

- ADC1 三通道扫描，**200 Hz 消隐采样**；每 5 ms 先关加热 0.5 ms 再读取。
- 冷端 NTC 参数集中在 [preset.h](include/preset.h)，默认 100 kΩ / B=3950 / 10 kΩ 上拉。
- 开机时由 `temp_init()` 按 Beta 方程动态生成 -20~100°C、2°C 步进的电阻表，不再使用写死的常量表。
- 默认冷端偏高约 10°C：`PRESET_NTC_CAL_OFFSET_C = -10.0f`。
- 更换 NTC 时只需修改 preset.h 中的 R25、B 值、分压电阻、拓扑、校准偏移和表范围，然后重新编译。
- 上电或 ADC 恢复后的初始热端温度直接继承冷端温度，避免出现 0°C/负温跳变。
- 热电偶默认曲线可用 `TEMP_TC_TRIM_GAIN/OFFSET_C` 微调；全量程精修建议使用 7 点 `TIP CAL`。
- 热电偶默认公式：`x = 240*N/(1+a)`，`T = -4.73e-7*x^2 + 0.12*x + 23.78`。
- 热端电阻 `a` 在 `include/preset.h` 的 `PRESET_TC_RESISTANCE_KOHM` 配置，默认 **150 kΩ**，与旧公式等价。
- 100~400°C 七点校准可生成 10 点查表，校准表优先于默认公式。
- 温度平滑：热电偶约 2 Hz 一阶低通，NTC 约 1.6 Hz 一阶低通；ADC 无效/NTC 故障时滤波器重新初始化。
- 加热器保护：热电偶开路/短路、30 s 不升温、温度突变、严重失温、>500°C、温度数据无效、持续 100% 满功率 30 s；保护检测 1 kHz，故障锁存后下一拍立即切断 PWM。
- 热上电保护屏蔽：上电前 **5 s** 不判断温度突变和过温，避免热烙铁上电建立过程误报；5 s 后恢复检测。
- PID 控制：标准曲线、boost 加强曲线、10 点温度增益自定义曲线；普通 PID 以 **200 Hz** 运行，输出由 20 kHz PWM 执行。
- 热端校准：`SYSTEM -> TIP CAL`，100/150/200/250/300/350/400°C 七点，实测值生成 10 点曲线。
- OneKeyBoost：NEXT 长按 +100°C/BOOST 曲线，1 s 内掉温超阈值自动 Boost，NEXT 短按退出，默认 300 s 超时；设置步进可在菜单中设为 1~100°C 任意整数。
- PMOS 控制：PA8 / TMR1_CH1，**20 kHz 硬件 PWM**，故障时立即强制关断。
- 状态机统一调度：HEATING / AUTOTUNE / DISABLED / FAULT；控制中断 20 kHz，后台任务 1 kHz。
- ST7735S **160x80 横屏** 4-line SPI 显示驱动；主界面、温度/功率/电压/冷端曲线和 MENU 短按单键轮换已实现。
- 三按键：PA15 MENU / PB3 PREV / PB4 NEXT，20 ms 消抖，支持单击、长按和连发；非菜单界面由 MENU 短按轮换页面。
- 设置菜单：MENU 长按 2 s，多级分类，10 点自定义 PID 曲线、10 点热电偶标定、休眠/电压/保护/UI 参数。
- 蜂鸣器：PB5 / TMR3_CH2，非阻塞多音效；开机、按键、到温、休眠/深睡/关机、唤醒、故障、模式、菜单、整定和错误音；音量 0~100 可调。
- 指示灯：PA6 / TMR3_CH1，运行时呼吸，待机/休眠慢闪，故障/HMS 快闪。
- HMS：EEPROM 写失败、加热控制看门狗等不可恢复错误持续占用屏幕；设置可自检显示 ALL OK。
- 参数保存：Flash 末尾 10 KB 中，前 2 KB 双扇区交替记录参数（CRC32 + commit），后 8 KB 存启动图/壁纸；菜单关闭/自动整定完成自动保存；恢复默认会同时擦除两个参数扇区和 8 KB 图片区。
- 电源管理：PA2 电压换算（10k/1k 分压，VCC/11）、PB1 震动休眠、18~30V 保护、PB6/PB7 CH224A 电压诱骗（默认 20V）；欠压/过压时主界面显示 `V LOW` / `V HIGH`。

## 热电偶、消隐采样与热端校准

默认热电偶拟合（N = PA1 原始 ADC）：

```text
T = -1.1948e-6 * N^2 + 0.19073 * N + 23.78
```

- 拟合参考冷端为 23.78°C，运行时按 `T + (cold_junction - 23.78)` 补偿
- 未启用标定表时使用该二次拟合
- 启用 10 点标定表后改为 ADC 查表插值 + 冷端补偿

C245 热电偶与加热驱动存在耦合，采样改为 **5 ms 周期 + 0.5 ms 消隐**：

```text
phase 0 .. 9   : 强制关加热，等待 0.5 ms
phase 10       : 读取 ADC、温度、PID，恢复 20 kHz PWM
phase 11 .. 199: 保持 PWM
```

因此：

- 20 kHz 仍然是 TMR1 PWM 载波
- 温度采样和 PID 更新为 **200 Hz**
- 每个 5 ms 周期最大有效加热占空比约 90%
- 自动整定同样以 200 Hz 推进

### 热端校准

[tempcal.c](src/tempcal.c:1) 提供 7 点校准：

```text
100 / 150 / 200 / 250 / 300 / 350 / 400 °C
```

流程：

1. 自动设定目标，等待温度进入 ±3°C 并稳定 3 s
2. 用消隐后的 ADC 平均 1 s
3. 屏幕提示输入外部温度计实测值
4. 七点完成后生成 10 点 ADC->温度曲线，启用并保存到 EEPROM

- UI：`SYSTEM -> TIP CAL`；PREV/NEXT 调整实测值，MENU 确认，长按 MENU 取消

校准曲线在 100~400°C 内使用实测点，范围外按默认二次拟合平移延伸，避免室温附近被钳到 100°C。

## 温度平滑

[temp.c](src/temp.c:350) 在 ADC 换算之后做 **3 点中值 + 一阶 IIR/EMA** 平滑：

```c
y_n = alpha * x_n + (1.0f - alpha) * y_last;
```

| 信号 | 采样率 | alpha | 截止频率 | 说明 |
|---|---:|---:|---:|---|
| 热电偶烙铁头 | 200 Hz | 0.06 | 约 2 Hz | 消隐后采样，抑制残余耦合 |
| NTC 冷端 | 200 Hz | 0.05 | 约 1.6 Hz | 冷端变化慢 |

- 首次有效采样直接取原始值，避免上电从 0°C 慢慢爬升
- ADC 无效或 NTC 开路/短路时重置滤波器
- 消隐采样已经去掉了大部分 PWM 耦合，再用 3 点中值 + 2 Hz 平滑压制噪声
- ADC 采样时间从 41.5 周期提高到 **71.5 周期**（20 MHz 下约 4.2 µs），降低采样噪声
- 主界面温度显示增加 ±0.7°C 回差，避免在整数边界来回跳 1°C

## 状态机与 20 kHz 调度

[statemachine.c](src/statemachine.c:1) 是唯一的模块调度者：

- **20 kHz 控制中断**：TMR1 更新中断，50 µs 周期
- **200 Hz 测温/PID**：每 5 ms 消隐 0.5 ms 后采样
- **1 kHz 保护/快照**：verifyheater、快照

控制中断顺序：

```text
phase 0 关加热 -> phase 10 ADC -> 温度 -> PID(200 Hz) -> 保护(1 kHz) -> PWM 恢复
```

1 kHz 慢任务：

| 时隙 | 任务 | 频率 |
|---|---|---|
| tick 0 | verifyheater 保护 | 1 kHz |

主循环：

```c
sm_init();
while(1)
{
  buttons_update(now_ms);
  power_update(now_ms);
  buzzer_task(now_ms);
  led_task(now_ms);
  menu_update(now_ms);
  autoroll_update(now_ms);
  wk_delay_ms(SM_SLOW_PERIOD_MS);
}
```

关键规则：

- 故障时立即进入 `FAULT`，关 PWM、复位 PID，直到 `faultreset` + `heateron`
- `faultreset` 清除故障后保持 `DISABLED`，不会自动恢复加热
- 自动整定期间拒绝改设定温度和曲线
- 温度无效时关闭输出并复位 PID，避免恢复时微分冲击

## PID 曲线与整定（模块级）

以下 API 由状态机在内部调用，菜单/按键负责修改控制状态。

```c
/* 初始化 */
pid_init();
pid_set_setpoint(300.0f);

/* 普通 PID：200 Hz，固定 5 ms 周期 */
float power = pid_update_fixed(tip_c, temp_valid, 0.01f);

/* 自动整定（200 Hz 推进，10%/90% PWM 双电平激励） */
pid_autotune_start(300.0f);

/* 整定完成后读取结果 */
pid_autotune_result_t result;
pid_autotune_get_result(&result);

/* 切换 boost 加强曲线 */
pid_set_mode(PID_MODE_BOOST);

/* 10 点自定义 PID 增益曲线（按温度线性插值） */
pid_custom_point_t custom[PID_CUSTOM_POINTS];
pid_get_custom_points(custom);

for(uint8_t i = 0U; i < PID_CUSTOM_POINTS; i++)
{
  custom[i].temp_c = 50.0f * i;
  custom[i].gain.kp = 2.0f;
  custom[i].gain.ki = 0.08f;
  custom[i].gain.kd = 1.5f;
}

pid_set_custom_points(custom);
sm_set_pid_mode(PID_MODE_CUSTOM);
```

也可以在 [statemachine.c](src/statemachine.c:38) 把 `SM_AUTOTUNE_ON_BOOT` 改为 `1`，让上电自动整定。

默认 `SM_AUTOTUNE_ON_BOOT = 0`，避免每次上电都执行 1~3 分钟整定。整定完成后标准 PID 曲线会自动写入 Flash 参数区。

自动整定默认在 300°C 附近进行，10%/90% 两个 PWM 电平需要能分别让温度下降到目标以下和上升到目标以上；如果要在其他温度整定且不收敛，可调整 `pid.c` 的 `PID_AT_PWM_LOW` / `PID_AT_PWM_HIGH`。

超调抑制：

- 默认 PID 为 `Kp=1.2 / Ki=0.04 / Kd=1.2`
- 设定温度前馈：按 `0.05 %/°C` 给出接近稳态的基础功率（最高 30%），最后 5°C 线性收敛到 0
- 最后 10°C 增加功率下限：`4 %/°C`（10°C→40%，5°C→20%，1°C→4%），避免满功率冲过头
- 积分允许负向修正到 -50%；过冲超过 0.5°C 时清掉正积分，不再维持小功率
- 任何设定温度变化都会调用 `pid_reset_runtime()`，清空积分、微分滤波、上次温度、输出和时间基准
- 积分分离：误差超过 ±30°C 时停止积分累积
- 距离目标超过 15°C 时直接 **100% 满功率升温**；进入最后 15°C 才交回 PID
- 自动整定改用 `Kp=Ku/2.8`、`Ti=2.8·Pu`，并限制整定后的初始积分不超过 35%

> 旧 EEPROM 里保存的自动整定曲线不会被自动改写；烧录后建议重新执行一次 `START AUTOTUNE`，或恢复默认设置后再整定。

## 迁移说明

- 启动文件、链接脚本、CMSIS、AT32F421 外设驱动由 PlatformIO 的 `at32firmlib` 框架提供，未重复复制。
- 原工程的 `syscalls.c` / `sysmem.c` 已一并迁移；显示/菜单的整数格式化使用 newlib `snprintf`。
- 全局 `-Os` + `-flto` 压缩 Flash；`statemachine.c`、`pid.c`、`temp.c`、`adc.c`、`heater.c`、`verifyheater.c` 继续用 `#pragma GCC optimize("O2")` 保持 20 kHz 控制速度。
- LTO 下启动文件的 TMR1 中断弱符号会覆盖真实 ISR，已用 `src/irq_tmr1.S` 强符号跳板指向 `sm_fast_update_irq()`，避免中断被优化掉。
- 当前 Flash 约 **54044 B（52.8 KiB）**，RAM 约 **14172 B**（壁纸 4bpp 缓存 6400 B），程序区 54 KB。
- `at32f421_conf.h` 放在 `include/`，并通过 `platformio.ini` 的 `-I$PROJECT_INCLUDE_DIR` 让框架驱动源码也能找到它。

## 硬件标定提醒

- NTC 分压默认按“10 kΩ 上拉到 VREF、NTC 下拉到 GND”计算；实际相反时修改 `include/preset.h` 的 `PRESET_NTC_CONNECT_TO_GND`。
- 输入电压默认按 **10 kΩ / 1 kΩ 分压（VCC/11）**；实际电阻不同时修改 `powermanagement.h` 的 `POWER_VIN_DIVIDER_R1/R2`。
- 震动开关默认 PB1 静止为高、震动拉低；唤醒使用持续 **100 ms** 的稳定电平变化，短噪声不唤醒。
- 休眠不再由震动决定：ACTIVE 状态累计“低功率”的时间，高功率按 3 倍速泄放；低功率占绝对多数并累积到休眠时间后进入 REDUCED。
- `SLEEP / POWER -> SLEEP PWR` 可设置低功率阈值，默认 **13%**，范围 0~100%。
- 按键手动休眠后 **30 秒内屏蔽震动唤醒**，30 秒后恢复。
- 按键、菜单、Boost 会清零低功率累计。
- CH224A 默认地址 0x22/0x23 自动探测，电压寄存器 0x0A；如果模块地址或寄存器不同，修改 `powermanagement.h` 的 `POWER_CH224A_*`。
- 热电偶默认使用 `x=240*N/(1+a)`、`T=-4.73e-7*x^2+0.12*x+23.78`，热端电阻 `a` 由 preset.h 配置，默认 150 kΩ；上板后优先用 `SYSTEM -> TIP CAL` 做七点校准。
- 保护阈值集中在 `src/verifyheater.c` 顶部宏，按实际加热器调整。
