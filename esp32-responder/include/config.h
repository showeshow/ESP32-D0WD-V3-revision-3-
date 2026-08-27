#pragma once

// ============================================================
//  学生端答题器 - 配置
// ============================================================

// ---------- WiFi（默认值，可在设备设置界面修改并持久化）----------
#define WIFI_SSID        "YourSSID"       // <-- 初始 WiFi 名称（之后可用设备上的齿轮图标修改）
#define WIFI_PASSWORD    "YourPassword"   // <-- 初始 WiFi 密码
#define WIFI_TIMEOUT_MS  15000            // 连接超时

// ---------- 答题 ----------
#define ANS_COUNT        10               // 题目数量（固定 10 格）
#define HTTP_TIMEOUT_MS  4000             // 提交超时

// ---------- 触摸（bit-bang 轮询，驱动参数见 touch.h）----------
#define TOUCH_POLL_MS    20               // 轮询间隔 20ms = 50Hz，配合 loop 节流
#define TOUCH_DEBOUNCE_MS 60              // 释放后的去抖时间

// ---------- 设置界面 ----------
#define SET_TEXT_MAX     32               // SSID / 密码最大长度

// ---------- UI 颜色（RGB565）----------
#define C_BG        0x0000                // 背景：黑
#define C_PANEL     0x10A2                // 深蓝灰
#define C_FRAME     0x39E7                // 普通边框：灰蓝
#define C_FRAME_HI  0xF800                // 当前光标边框：红
#define C_KEY_BG    0x3A69                // 按键底色：蓝
#define C_KEY_BG2   0x9CD1                // 特殊按键底色：青蓝
#define C_KEY_TEXT  0xFFFF                // 按键文字：白
#define C_TEXT      0xFFFF                // 正文：白
#define C_TEXT_DIM  0x8410                // 次要文字：灰
#define C_OK        0x07E0                // 成功：绿
#define C_ERR       0xF800                // 失败：红
#define C_ANS       0xFFE0                // 答案文字：黄
