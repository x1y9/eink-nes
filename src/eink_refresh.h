/*
 * Kindle eink 刷新控制
 *
 * Kindle 的 framebuffer 不是普通 LCD —— 写完 /dev/fb0 不会自动上屏。
 * 必须通过 ioctl MXCFB_SEND_UPDATE 通知 eink 控制器刷新指定区域。
 *
 * 关键结构 (来自 Kindle 内核 linux/mxcfb.h, K5 之后一直沿用):
 *   mxcfb_update_data {
 *     update_region;      // 刷新区域
 *     waveform_mode;      // 波形: INIT / DU / GC16 / A2 ...
 *     update_mode;        // 刷新模式: PARTIAL / FULL / PARTIAL_DITHERED
 *     update_marker;      // 序号, 用于 wait_for_update 同步
 *     temp;               // 温度 (PW3 需要, 否则可能报错)
 *     ...
 *   }
 *
 * 刷新策略 (墨水屏的核心):
 *   GC16  -> 16 级灰度全刷, 无残影, 但 ~600ms 且全屏闪黑
 *   DU    -> 直接更新, ~100ms, 只适合黑白二值内容, 灰度会脏
 *   A2    -> 最快 ~50ms, 纯二值动画, 残影重
 *   PARTIAL -> 局部刷新, 只刷脏区, 但累计 N 次后必须 GC16 清一次
 *
 * NES 是彩色游戏转灰度, 必须用 GC16 (DU/A2 会丢灰阶)。
 * 所以每帧都 GC16 代价太大 -> 用"脏区局部 GC16 + 每 N 帧全屏 GC16"策略。
 */

#ifndef EINK_REFRESH_H
#define EINK_REFRESH_H

#include <stdint.h>

/* ---- Kindle mxcfb 结构 (手写, 避免依赖内核头文件) ---- */
struct kindle_rect {
    uint32_t top;
    uint32_t left;
    uint32_t width;
    uint32_t height;
};

struct kindle_update_data {
    struct kindle_rect update_region;
    uint32_t waveform_mode;
    uint32_t update_mode;
    uint32_t update_marker;
    int      temp;
    uint32_t flags;
    int      dither_mode;
    uint32_t quant_bit;
    uint32_t alt_buffer_data_phys;
    uint32_t alt_buffer_data_virt;
    uint32_t alt_buffer_data_width;
    uint32_t alt_buffer_data_height;
    uint32_t unk1;
    uint32_t unk2;
};

/* ioctl 号: 0x4048 开头 + size. 各机型略有差异, 运行时探测 */
#define MXCFB_SEND_UPDATE_1  _IOWR('F', 42, struct kindle_update_data)  /* K4/K5 */
#define MXCFB_SEND_UPDATE_2  _IOWR('F', 46, struct kindle_update_data)  /* PW3 */
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE_1 _IOWR('F', 43, uint32_t)
#define MXCFB_WAIT_FOR_UPDATE_COMPLETE_2 _IOWR('F', 45, uint32_t)

/* 波形模式 */
#define WAVEFORM_INIT       0
#define WAVEFORM_DU         1
#define WAVEFORM_GC16       2
#define WAVEFORM_GC4        3
#define WAVEFORM_A2         4

/* 更新模式 */
#define UPDATE_MODE_PARTIAL          0
#define UPDATE_MODE_FULL             1
#define UPDATE_MODE_PARTIAL_DITHERED 2
#define UPDATE_MODE_FULL_DITHERED    3

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      fd;
    int      send_update_cmd;   /* 探测到的 ioctl 号 */
    int      wait_cmd;
    uint32_t marker;
    int      works;             /* 探测结果: 1 = 支持 eink ioctl */
    int      frames_since_full; /* 距离上次全刷的帧数 */
    int      full_interval;     /* 每多少帧强制全刷一次清残影 */
} EinkRefresh;

/* 初始化: 探测 ioctl 是否可用。失败则退化为普通 fb 写入 (桌面调试用) */
int  eink_init(EinkRefresh* er, int fb_fd, int full_interval);

/* 诊断: 暴力扫描 'F' magic 的 ioctl 号 + 打印 fb 信息。真机上用来确定正确 cmd。*/
void eink_probe(int fb_fd);

/* 刷新一个区域。waveform: WAVEFORM_GC16 等 */
int  eink_update(EinkRefresh* er, int x, int y, int w, int h, uint32_t waveform);

/* 刷新整屏, 并重置残影计数 */
int  eink_full(EinkRefresh* er, int sw, int sh);

/* 每帧调度: 自动决定局部刷 / 全刷。返回 1 = 执行了全刷 */
int  eink_tick(EinkRefresh* er, int x, int y, int w, int h, int sw, int sh);

/* 收尾: 退出前做一次全刷, 否则屏幕停在脏状态 */
void eink_finish(EinkRefresh* er, int sw, int sh);

#ifdef __cplusplus
}
#endif

#endif /* EINK_REFRESH_H */
