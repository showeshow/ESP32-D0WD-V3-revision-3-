#pragma once
#include <Arduino.h>

// ============================================================
//  XPT2046 bit-bang 软 SPI 驱动（CYD 独立触摸引脚，纯轮询，无 IRQ）
//
//  背景：TFT_eSPI 的 getTouch() 只走硬件 SPI 总线（与屏幕共享的
//  13/12/14），而 CYD 的 XPT2046 接在独立 GPIO 25/32/39/33 上，
//  库驱动根本读不到——必须自己 bit-bang。
//
//  v3 修正：
//   - 时序改为 8 命令 + 1 空时钟(启动转换) + 12 数据 = 21 时钟
//     （v2 只有 8+12=20，漏了启动转换位，读到无效残留）
//   - 压力改用 Z1+Z2 差分公式 Z = Z1 + 4096 - Z2（数据手册推荐）
//   - 阈值/窗口大幅放宽，校准点内缩避开电阻屏边缘非线性区
//
//  性能：时钟沿间隔 3us（约 150kHz，低于 1MHz）；上层 20ms 轮询，全程无中断。
// ============================================================

#define TP_CLK 25
#define TP_CS  33
#define TP_DIN 32
#define TP_DO  39

// 压力阈值（Z = Z1 + 4096 - Z2，未按下≈0，按下随力度增大）
#define TP_Z_MIN       80      // 压力阈值：低于此视为未按下（放宽，边缘也能触发）
#define TP_RAW_MIN    100      // 原始值合理窗口下限（放宽）
#define TP_RAW_MAX   4000      // 原始值合理窗口上限（放宽）
#define TP_DEV        150      // 两次采样最大允许偏差（放宽，适应轻触抖动）

// 校准目标点（屏幕像素，**大幅内缩避开电阻屏边缘非线性区**）
// v2 用 20/16/300/224 离边缘太近，物理上点不动 → 四角校准必失败
// v3 内缩到 50/50/270/190，离边缘至少 50 像素
#define TP_CAL_X0  50
#define TP_CAL_Y0  50
#define TP_CAL_X1 270
#define TP_CAL_Y1 190

// XPT2046 命令字节（12位模式，差分，PD=00；bit6-3 通道选择）
#define TP_CMD_X   0x90        // XP 通道（屏幕 X）
#define TP_CMD_Y   0xD0        // YP 通道（屏幕 Y）
#define TP_CMD_Z1  0xB0        // Z1 压力通道
// Z2 通道 = 0xC0（在 tpPressed 中用 TP_CMD_Z1 + 0x10 计算）

// 校准数据：ax0/ax1 = 屏幕 X 轴在 TP_CAL_X0 / TP_CAL_X1 处的原始通道值
//           ay0/ay1 = 屏幕 Y 轴在 TP_CAL_Y0 / TP_CAL_Y1 处的原始通道值
//           swap   = 1 时屏幕 X 对应 rawY 通道（横屏旋转 90° 常见）
struct TouchCal {
    uint16_t ax0, ax1, ay0, ay1;
    uint8_t  swap;
};

void tpInit();                                  // 引脚初始化
uint16_t tpReadChannel(uint8_t cmd);            // 读一个通道（0..4095）
bool tpPressed();                               // 压力判断（Z1+Z2 差分公式）
bool tpReadRaw(uint16_t* rx, uint16_t* ry);     // 稳定原始坐标（双采样+窗口过滤）
void tpDebugDump();                             // 串口输出原始值（诊断用）

void tpSetCal(const TouchCal& cal);
bool tpLoadCal(TouchCal* cal);
void tpStoreCal(const TouchCal& cal);
bool tpMap(uint16_t rx, uint16_t ry, uint16_t* sx, uint16_t* sy);
bool tpReadScreen(uint16_t* sx, uint16_t* sy);  // 一站式：按下判断+读坐标+映射

bool tpWaitStableRaw(uint16_t* rx, uint16_t* ry, uint32_t timeoutMs);
