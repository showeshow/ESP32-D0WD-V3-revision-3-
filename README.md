# ESP32-D0WD-V3-revision-3-
4M 0PSRAM   XPT2046 触摸  触控定位不准 — bit-bang 时序漏了一个时钟  压力检测换公式：从单 Z1 阈值改为 XPT2046 数据手册推荐的差分公式 Z = Z1 + 4096 - Z2，不受按压力度影响，更稳
