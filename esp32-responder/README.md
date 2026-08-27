# ESP32 学生端答题器 v2

基于 ESP32-2432S028（Cheap Yellow Display 2.8" / ESP32-D0WD-V3）的局域网答题器。
横屏界面，触摸操作，HTTP 向内网服务器提交答案。

## v2 重要变更（修复触控无反应）

v1 用 TFT_eSPI 自带 `getTouch()` 读触摸——**但该驱动只走硬件 SPI 总线（屏幕共用的 13/12/14），而 CYD 的 XPT2046 接在独立 GPIO 25/32/39/33 上，根本不在 SPI 总线上**，所以读不到任何数据，触控完全无反应。

v2 改为自研 bit-bang 软 SPI 驱动（`src/touch.cpp`）：
- 直接操作 GPIO 25/32/39/33 的时钟/数据线
- 时钟约 200kHz（低于 1MHz，满足降速要求）
- Z1 压力检测 + 双采样窗口过滤，防误触
- 两点触摸校准（首次开机自动进入，校准值存 Preferences 持久化）
- 全程纯轮询，无 IRQ / 无 attachInterrupt

## 硬件针脚（与网络收音机项目一致）

| 功能 | 引脚 | 说明 |
|------|------|------|
| 屏幕 ILI9341 (SPI) | MOSI=13 MISO=12 SCK=14 CS=15 DC=2 RST=-1 BL=21 | 320x240 横屏 |
| 触摸 XPT2046 (软SPI) | CLK=25 CS=33 DIN=32 DO=39 IRQ=36 | **IRQ 未启用，纯轮询** |
| SD 卡 / 喇叭 / I2S | — | **全部禁用** |

## 烧录前

WiFi 可两种方式配置：
1. **设备上直接配**（推荐）：开机进入登录界面后，点右上角齿轮图标 → 设置界面 → 输入 WiFi 名/密码 → 保存。配置自动持久化，下次开机无需再设。
2. **改默认值**：编辑 `include/config.h` 中的 `WIFI_SSID` / `WIFI_PASSWORD`，再重新编译。

## 烧录方法

### 方法一：VS Code + PlatformIO（推荐）
1. VS Code 打开本文件夹（需 PlatformIO 插件）
2. 点击底部状态栏 `→` (Upload)

### 方法二：esptool 直接烧录 bin
```bash
esptool.py --chip esp32 --port COM5 --baud 921600 write_flash 0x0 firmware_merged.bin
```

## 首次开机流程

1. **触摸校准**（仅首次）：屏幕显示左上角圆点 → 点击 → 显示右下角圆点 → 点击 → 校准完成
   - 之后想重新校准：登录界面 → 齿轮图标 → 设置界面 → "校准" 按钮
2. **登录界面**：
   - 顶部 3 个输入框居中（`192.168.` 前缀隐藏）：IP段3 / IP段4 / 端口
   - 数字键盘放大占满屏幕（5×2 + 右侧确认键跨两行）
   - **右上角透明齿轮**：进入 WiFi 设置
   - 按数字键输入；填满自动跳格，点框重输
3. **答题界面**：
   - 顶部 10 格对应第 1-10 题
   - 第一行 A/B/C，第二行 D/勾/叉，右侧发送键
   - 输入答案后光标自动进格；点格子选中后输入即覆盖旧答案
   - 发送后光标回第 1 格，多次提交多次计数

## WiFi 设置界面

齿轮图标进入后：
- 上方两个输入框：网络名 / 密码
- 三排 QWERTY 键盘（小写 / 大写 / 数字符号三档切换）
- 功能行：`<-` 返回 / 校准 / 123(ABC) / 空格 / A/a / 保存
- 保存后自动断开当前 WiFi 并用新凭据重连，返回登录界面

## 服务器协议

```
POST http://192.168.<框1>.<框2>:<框3>/submit
Content-Type: application/x-www-form-urlencoded
answers=A,B,C,D,T,F,...&seq=N
```

- `answers`：10 个答案，逗号分隔；A/B/C/D 选项，`T`=对，`F`=错，`-`=未答
- `seq`：第几次提交（从 1 递增）
- 服务器返回 HTTP 2xx 视为成功

## 性能与内存策略

- 触摸纯轮询（无 attachInterrupt，XPT2046 IRQ 完全未启用），20ms 节流
- XPT2046 SPI 时钟约 200kHz（自研驱动，2us 沿间隔）
- 禁用 I2S / 音频 / 喇叭 / SD 卡 / 蓝牙 / OTA
- loop 节流：触摸 50Hz、WiFi 检查 1Hz、仅状态变化时重绘
- 无 LVGL；中文为自生成 24x24 点阵字库（89 字，6.4KB）
- 编译结果：**RAM 14.3% / Flash 62.3%**

## 文件结构

```
esp32-responder/
├── platformio.ini          构建配置（已移除 TFT_eSPI 触摸宏）
├── include/
│   ├── config.h            WiFi 默认值、UI 颜色、参数
│   └── touch.h             XPT2046 驱动接口
├── src/
│   ├── main.cpp            主程序（三界面 + 交互 + 网络提交）
│   ├── touch.cpp           XPT2046 bit-bang 驱动 + 校准存取
│   └── zh_font.c           自动生成的中文点阵字库
└── tools/
    └── gen_zh_font.py      字库生成脚本（修改界面文字后重跑）
```
