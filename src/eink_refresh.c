#include "eink_refresh.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>

static void msleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int try_send(EinkRefresh* er, struct kindle_update_data* ud, int cmd) {
    return ioctl(er->fd, cmd, ud);
}

int eink_init(EinkRefresh* er, int fb_fd, int full_interval) {
    memset(er, 0, sizeof(*er));
    er->fd = fb_fd;
    er->marker = 1;
    er->full_interval = (full_interval > 0) ? full_interval : 15;

    // Kindle 用 'F' (0x46) magic, 但不同固件 nr 不同。依次探测。
    // 用一次 1x1 的无效区域试探, 只要不是 EINVAL/ENOTTY 就说明这个号存在。
    struct kindle_update_data probe;
    memset(&probe, 0, sizeof(probe));
    probe.update_region.top = 0;
    probe.update_region.left = 0;
    probe.update_region.width = 1;
    probe.update_region.height = 1;
    probe.waveform_mode = WAVEFORM_GC16;
    probe.update_mode = UPDATE_MODE_PARTIAL;
    probe.update_marker = er->marker++;
    probe.temp = 0x1000;   // PW3 常见默认温区

    int cands[2] = { MXCFB_SEND_UPDATE_2, MXCFB_SEND_UPDATE_1 };
    int wait_cands[2] = { MXCFB_WAIT_FOR_UPDATE_COMPLETE_2, MXCFB_WAIT_FOR_UPDATE_COMPLETE_1 };

    for (int i = 0; i < 2; i++) {
        errno = 0;
        int r = try_send(er, &probe, cands[i]);
        if (r == 0) {
            er->send_update_cmd = cands[i];
            er->wait_cmd = wait_cands[i];
            er->works = 1;
            // 等这次探测刷新完成, 免得污染后续
            uint32_t m = probe.update_marker;
            ioctl(er->fd, er->wait_cmd, &m);
            fprintf(stderr, "[eink] ioctl 可用 (cmd=0x%lx), 全刷间隔 %d 帧\n",
                    (unsigned long)cands[i], er->full_interval);
            return 0;
        }
        int e = errno;
        // EINVAL/ENOTTY/ENOSYS/EFAULT 都表示"这个 fb 不是 Kindle eink"
        if (e != EINVAL && e != ENOTTY && e != ENOSYS && e != EFAULT) {
            er->send_update_cmd = cands[i];
            er->wait_cmd = wait_cands[i];
            er->works = 1;
            fprintf(stderr, "[eink] ioctl 存在但探测返回 %s (cmd=0x%lx)\n",
                    strerror(e), (unsigned long)cands[i]);
            return 0;
        }
    }

    er->works = 0;
    fprintf(stderr, "[eink] 未检测到 Kindle eink ioctl —— 按普通 framebuffer 处理\n");
    return -1;
}

static int do_update(EinkRefresh* er, int x, int y, int w, int h,
                     uint32_t waveform, uint32_t mode, int wait) {
    if (!er->works) return -1;
    if (w <= 0 || h <= 0) return 0;

    struct kindle_update_data ud;
    memset(&ud, 0, sizeof(ud));
    ud.update_region.top    = (uint32_t)y;
    ud.update_region.left   = (uint32_t)x;
    ud.update_region.width  = (uint32_t)w;
    ud.update_region.height = (uint32_t)h;
    ud.waveform_mode        = waveform;
    ud.update_mode          = mode;
    ud.update_marker        = er->marker++;
    ud.temp                 = 0x1000;

    int r = try_send(er, &ud, er->send_update_cmd);
    if (r < 0 && errno != ENOSYS && errno != ENOTTY) {
        // temp 不被接受时重试一次 (某些固件要求 -1)
        ud.temp = -1;
        r = try_send(er, &ud, er->send_update_cmd);
    }
    if (r < 0 && (errno == ENOSYS || errno == ENOTTY)) {
        // 运行时发现不支持 —— 永久关闭, 别再打扰
        er->works = 0;
        fprintf(stderr, "[eink] 运行时确认不支持 eink ioctl, 已关闭刷新\n");
        return -1;
    }
    if (r < 0) {
        fprintf(stderr, "[eink] SEND_UPDATE 失败: %s (region %d,%d %dx%d wave=%u mode=%u)\n",
                strerror(errno), x, y, w, h, waveform, mode);
        return -1;
    }

    if (wait) {
        uint32_t m = ud.update_marker;
        // 最多等 1.5s, 防止驱动卡死
        for (int i = 0; i < 30; i++) {
            if (ioctl(er->fd, er->wait_cmd, &m) == 0) break;
            msleep(50);
        }
    }
    return 0;
}

int eink_update(EinkRefresh* er, int x, int y, int w, int h, uint32_t waveform) {
    // 对齐到 4 像素, 某些驱动要求
    x &= ~3;
    w = (w + 3) & ~3;
    return do_update(er, x, y, w, h, waveform, UPDATE_MODE_PARTIAL, 0);
}

int eink_full(EinkRefresh* er, int sw, int sh) {
    er->frames_since_full = 0;
    if (!er->works) return -1;
    return do_update(er, 0, 0, sw, sh, WAVEFORM_GC16, UPDATE_MODE_FULL, 1);
}

/*
 * 每帧调度:
 *   普通帧 -> 只刷游戏区脏区 (PARTIAL + GC16, 保住灰阶)
 *   每 full_interval 帧 -> 全屏 GC16 清残影
 *
 * 注意: GC16 局部刷新也有 ~200-300ms 延迟, 所以实际帧率会被 eink 拖到 3-5fps。
 * 这是墨水屏物理上限, 不是代码问题。UI 区 (虚拟键) 只在布局变化时刷, 不参与每帧。
 */
int eink_tick(EinkRefresh* er, int x, int y, int w, int h, int sw, int sh) {
    if (!er->works) return 0;

    er->frames_since_full++;
    if (er->frames_since_full >= er->full_interval) {
        eink_full(er, sw, sh);
        return 1;
    }
    eink_update(er, x, y, w, h, WAVEFORM_GC16);
    return 0;
}

void eink_finish(EinkRefresh* er, int sw, int sh) {
    if (!er->works) return;
    // 退出前全刷一次, 把残影清掉, 屏幕回到干净状态
    do_update(er, 0, 0, sw, sh, WAVEFORM_GC16, UPDATE_MODE_FULL, 1);
    fprintf(stderr, "[eink] 退出全刷完成\n");
}

/* ================= 诊断: 暴力探测 ================= */

/*
 * 为什么需要这个:
 *   Kindle 各代固件的 MXCFB_SEND_UPDATE 编号不统一, 光靠猜会失败。
 *   探测失败时 eink_init 会把 works 设为 0, 之后整个程序不再刷新屏幕,
 *   表现就是"屏幕闪一下就静止"。所以在真机上先跑 --probe 拿到真实编号。
 *
 * 做法:
 *   对 'F' magic 的 nr 0..63 逐个发 ioctl, 传一个 1x1 的真实 update 结构。
 *   判定:
 *     ENOTTY / EINVAL(部分) -> 该命令不存在
 *     0                     -> 可用
 *     EPERM / 其他           -> 命令存在但参数或权限不对, 也可能可用
 */
void eink_probe(int fb_fd) {
    fprintf(stderr, "[probe] fd=%d sizeof(kindle_update_data)=%zu\n",
            fb_fd, sizeof(struct kindle_update_data));

    /* 先用 FBIOGET_* 把 framebuffer 的真实情况打出来 */
    struct {
        unsigned long cmd;
        const char* name;
    } fbioc[] = {
        { 0x4600, "FBIOGET_VSCREENINFO" },
        { 0x4601, "FBIOGET_FSCREENINFO" },
    };
    (void)fbioc;

    struct kindle_update_data ud;
    memset(&ud, 0, sizeof(ud));
    ud.update_region.top = 0;
    ud.update_region.left = 0;
    ud.update_region.width = 1;
    ud.update_region.height = 1;
    ud.waveform_mode = WAVEFORM_GC16;
    ud.update_mode = UPDATE_MODE_PARTIAL;
    ud.update_marker = 1;
    ud.temp = 0x1000;

    fprintf(stderr, "[probe] 扫描 'F' magic, nr=0..63 ...\n");
    int found = 0;
    for (int nr = 0; nr < 64; nr++) {
        unsigned long cmd = _IOWR('F', nr, struct kindle_update_data);
        errno = 0;
        int r = ioctl(fb_fd, cmd, &ud);
        int e = errno;
        if (r == 0) {
            fprintf(stderr, "[probe]   nr=%-3d cmd=0x%08lx -> OK\n", nr, cmd);
            found++;
        } else if (e != ENOTTY && e != EINVAL && e != ENOSYS) {
            fprintf(stderr, "[probe]   nr=%-3d cmd=0x%08lx -> 存在? %s (%d)\n",
                    nr, cmd, strerror(e), e);
            found++;
        }
        errno = 0;
    }

    /* 同时试一下不带 size 编码的裸命令 (有些老驱动只认数字) */
    for (int nr = 40; nr < 50; nr++) {
        errno = 0;
        int r = ioctl(fb_fd, nr, &ud);
        int e = errno;
        if (r == 0 || (e != ENOTTY && e != EINVAL && e != ENOSYS))
            fprintf(stderr, "[probe]   裸 nr=%d -> %s\n", nr, r == 0 ? "OK" : strerror(e));
    }

    fprintf(stderr, "[probe] 共 %d 个候选\n", found);
    if (found == 0) {
        fprintf(stderr, "[probe] 结论: 这个 fb 不认 'F' magic 的 MXCFB ioctl。\n");
        fprintf(stderr, "[probe] 可能原因: (1) framework 没停, fb 权限不够\n");
        fprintf(stderr, "[probe]           (2) 该固件用 EPDC 而非 MXCFB\n");
        fprintf(stderr, "[probe]           (3) 需要先 FBIOBLANK 解锁\n");
    }
}
