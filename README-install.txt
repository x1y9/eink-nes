eink-nes — Kindle Paperwhite 3 NES 模拟器
==========================================

安装
----
1. 用 USB 连接 Kindle，确保已越狱并装了 KUAL
2. 把 dist/eink-nes 拷到  /mnt/us/extensions/eink-nes/eink-nes
3. 把 dist/extensions/eink-nes/ 整个目录拷到 /mnt/us/extensions/
4. 把 .nes ROM 放进 /mnt/us/roms/  （没有这个目录就新建）
5. 在 Kindle 上打开 KUAL -> eink-nes -> 启动 NES 模拟器

目录结构（Kindle 上）
--------------------
/mnt/us/extensions/eink-nes/
    eink-nes              <- 可执行文件（ARM 静态，752KB）
    eink-nes.sh           <- 启动脚本
    menu.json
    showlog.sh
/mnt/us/roms/*.nes        <- 游戏 ROM

操作
----
触摸屏幕下半部分的虚拟键：方向键在左、A/B 在右、start/select 居中。
退出：Kindle 返回键，或 SSH 里 kill。

命令行参数（SSH 调试用）
-----------------------
  -f <fb>        framebuffer 设备（默认 /dev/fb0）
  -t <event>     触摸设备（默认自动检测）
  -s <1-5>       缩放倍数（默认按屏幕自动选，PW3 上为 4）
  -r <dir>       ROM 目录（可多次）
  -j <N>         每 N 帧渲染一次（默认 1，墨水屏建议 3-5）
  -p <file.ppm>  退出时存截图
  -n <帧数>      跑够 N 帧自动退出
  -b             基准测试模式

调试环境变量
------------
  EINK_NES_DEBUG=1   打印调色板、像素直方图、灰度 LUT 后退出

已知限制
--------
- 墨水屏刷新率是物理上限，实际 3-8 fps。适合 RPG / 战棋 / 棋牌，不适合动作游戏
- 无声音（PW3 无扬声器）
- GC16 波形保灰阶但慢；换 A2 快 5 倍但只剩黑白二值
- 触摸是"按下即触发"，没有连发；需要连发按住的游戏要另外加

技术细节
--------
- 模拟核心：QuickNES (Nes_Emu 0.7.0)，36 个 C++ 源文件交叉编译
- 编译目标：armv7-a / cortex-a9 / NEON / hard-float，静态链接（兼容 Kindle glibc 2.12）
- 渲染：NES 8bit 调色板索引 -> 亮度公式 -> 4x4 Bayer 有序抖动 -> framebuffer
- 刷新：MXCFB_SEND_UPDATE ioctl 探测式适配，GC16 局部刷 + 每 15 帧全屏清残影
- 布局：按屏幕尺寸自动选 scale，优先保证虚拟键区高度
