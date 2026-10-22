/*
 * eink-nes - Kindle Paperwhite 3 (i.MX6 SoloLite) NES emulator
 *
 * 直接在 LCD/eink framebuffer 上渲染，物理按键 + 虚拟按键输入。
 * 无 X11、无 GTK、无 SDL —— 只依赖 /dev/fb0 和 /dev/input/event*。
 *
 * 编译: arm-linux-gnueabihf-g++ (静态链接)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <algorithm>

#include "Nes_Emu.h"
#include "abstract_file.h"
#include "Data_Reader.h"
#include "eink_refresh.h"

/* ---------- NES 按键位 ---------- */
#define JOY_A      1
#define JOY_B      2
#define JOY_SELECT 4
#define JOY_START  8
#define JOY_UP     0x10
#define JOY_DOWN   0x20
#define JOY_LEFT   0x40
#define JOY_RIGHT  0x80

/* ---------- Linux input 键值 ---------- */
#ifndef KEY_BACK
#define KEY_BACK 158
#endif
#ifndef KEY_MENU
#define KEY_MENU 139
#endif

static volatile sig_atomic_t g_quit = 0;
static void on_sigint(int) { g_quit = 1; }

/* ================= Framebuffer ================= */
struct FB {
    int fd = -1;
    unsigned char* mem = nullptr;
    size_t maplen = 0;
    int w = 0, h = 0;
    int bits = 0;
    int line_bytes = 0;
    int is_16bpp = 0;
};

static bool fb_open(FB* fb, const char* path) {
    fb->fd = open(path, O_RDWR);
    if (fb->fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return false; }

    struct fb_var_screeninfo vi;
    struct fb_fix_screeninfo fi;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vi) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &fi) < 0) {
        fprintf(stderr, "FBIOGET_*: %s\n", strerror(errno));
        close(fb->fd); fb->fd = -1; return false;
    }
    fb->w = vi.xres;
    fb->h = vi.yres;
    fb->bits = vi.bits_per_pixel;
    fb->line_bytes = fi.line_length;
    fb->is_16bpp = (fb->bits == 16);

    // 只 map 一屏
    fb->maplen = (size_t)fb->line_bytes * fb->h;
    fb->mem = (unsigned char*)mmap(nullptr, fb->maplen, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fb->fd, 0);
    if (fb->mem == MAP_FAILED) {
        fprintf(stderr, "mmap fb: %s\n", strerror(errno));
        fb->mem = nullptr; close(fb->fd); fb->fd = -1; return false;
    }
    fprintf(stderr, "[fb] %s %dx%d %dbpp line=%d\n",
            path, fb->w, fb->h, fb->bits, fb->line_bytes);
    return true;
}

/* 往 fb 写一个灰度像素 (0-255, 0=黑) */
static inline void fb_put_gray(FB* fb, int x, int y, int g) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    if (fb->is_16bpp) {
        // RGB565 灰阶: 三通道必须等值。R/B 是 5bit, G 是 6bit。
        // 取 g 的高 5 位作为 n, 则 R=B=n, G=n<<1, 三通道亮度一致。
        unsigned char* p = fb->mem + (size_t)y * fb->line_bytes + x * 2;
        unsigned int n = (unsigned int)g >> 3;      // 5bit
        unsigned short v = (unsigned short)((n << 11) | ((n << 1) << 5) | n);
        *(unsigned short*)p = v;
    } else {
        // 8bpp / 灰度: 直接写
        fb->mem[(size_t)y * fb->line_bytes + x] = (unsigned char)g;
    }
}

/* 矩形填充 */
static void fb_fill_rect(FB* fb, int x, int y, int w, int h, int g) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= fb->h) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= fb->w) continue;
            if (fb->is_16bpp) {
                unsigned char* p = fb->mem + (size_t)j * fb->line_bytes + i * 2;
                unsigned int n = (unsigned int)g >> 3;
                *(unsigned short*)p = (unsigned short)((n << 11) | ((n << 1) << 5) | n);
            } else {
                fb->mem[(size_t)j * fb->line_bytes + i] = (unsigned char)g;
            }
        }
    }
}

/* ================= 布局 ================= */
/* 屏幕 1072x1448 (PW3 竖屏)。NES 256x240, 4x 放大 = 1024x960 */
struct Layout {
    int scale = 4;
    int game_w, game_h;
    int game_x, game_y;      // 游戏画面左上角
    int pad_area_y;          // 虚拟键区顶部 y
    int pad_area_h;

    // 方向键 (十字, 左)
    int dpad_cx, dpad_cy, dpad_arm;   // 中心 + 臂宽
    // AB 按钮 (右)
    int btn_a_cx, btn_a_cy, btn_r;
    int btn_b_cx, btn_b_cy;
    // start/select (中)
    int btn_sx, btn_sy, btn_sw, btn_sh;
    int btn_tx, btn_ty;
};

/*
 * 布局策略:
 *   虚拟键区优先要够大 (至少占屏高 30%, 且 >= 340px), 剩余高度给游戏画面。
 *   scale 从 5 往 1 试, 取第一个能同时满足 [画面 + 键区] 的值。
 *   这样 768p / 1448p / 1072p 各种屏幕都能自适应。
 */
static void layout_init(Layout* L, int sw, int sh) {
    const int MIN_PAD_H = (sh < 900) ? 260 : 380;   // 虚拟键区最小高度
    const int GAP = 8;

    int best_scale = 1;
    for (int s = 5; s >= 1; s--) {
        int gw = 256 * s, gh = 240 * s;
        if (gw <= sw && gh + GAP + MIN_PAD_H <= sh) { best_scale = s; break; }
    }

    L->scale = best_scale;
    L->game_w = 256 * best_scale;
    L->game_h = 240 * best_scale;
    L->game_x = (sw - L->game_w) / 2;
    L->game_y = GAP;

    L->pad_area_y = L->game_y + L->game_h + GAP;
    L->pad_area_h = sh - L->pad_area_y;
    if (L->pad_area_h < 0) L->pad_area_h = 0;

    int cy = L->pad_area_y + L->pad_area_h * 58 / 100;

    // 方向键尺寸: 跟随键区高度, 但限制上下界
    int arm = L->pad_area_h * 22 / 100;
    if (arm < 34) arm = 34;
    if (arm > 88) arm = 88;
    L->dpad_arm = arm;
    L->dpad_cx = sw * 22 / 100;
    L->dpad_cy = cy;

    // AB 按钮半径
    int r = L->pad_area_h * 17 / 100;
    if (r < 32) r = 32;
    if (r > 72) r = 72;
    L->btn_r = r;
    L->btn_b_cx = sw * 71 / 100;
    L->btn_b_cy = cy + L->pad_area_h * 8 / 100;
    L->btn_a_cx = sw * 86 / 100;
    L->btn_a_cy = cy - L->pad_area_h * 7 / 100;

    // start / select: 中上
    int bw = sw * 11 / 100;
    if (bw < 76) bw = 76;
    if (bw > 130) bw = 130;
    L->btn_sw = bw;
    L->btn_sh = L->pad_area_h * 13 / 100;
    if (L->btn_sh < 34) L->btn_sh = 34;
    if (L->btn_sh > 62) L->btn_sh = 62;
    L->btn_sy = L->pad_area_y + L->pad_area_h * 14 / 100;
    L->btn_sx = sw / 2 - L->btn_sw - 12;
    L->btn_tx = sw / 2 + 12;
    L->btn_ty = L->btn_sy;
}

/* 命中测试: 触屏点 → joypad bits */
static int layout_hit(Layout* L, int x, int y) {
    int bits = 0;

    // 方向键十字
    int a = L->dpad_arm;
    int dx = x - L->dpad_cx, dy = y - L->dpad_cy;
    if (abs(dx) <= a && abs(dy) <= a * 3) {
        if (dy < -a / 2) bits |= JOY_UP;
        else if (dy > a / 2) bits |= JOY_DOWN;
        else if (dx < -a / 2) bits |= JOY_LEFT;
        else if (dx > a / 2) bits |= JOY_RIGHT;
    }

    // AB
    int ddx = x - L->btn_a_cx, ddy = y - L->btn_a_cy;
    if (ddx * ddx + ddy * ddy <= L->btn_r * L->btn_r) bits |= JOY_A;
    ddx = x - L->btn_b_cx; ddy = y - L->btn_b_cy;
    if (ddx * ddx + ddy * ddy <= L->btn_r * L->btn_r) bits |= JOY_B;

    // start
    if (x >= L->btn_sx && x < L->btn_sx + L->btn_sw &&
        y >= L->btn_sy && y < L->btn_sy + L->btn_sh) bits |= JOY_START;
    // select
    if (x >= L->btn_tx && x < L->btn_tx + L->btn_sw &&
        y >= L->btn_ty && y < L->btn_ty + L->btn_sh) bits |= JOY_SELECT;

    return bits;
}

/* 绘制虚拟键（静态部分，只画一次） */
static void draw_controls(FB* fb, Layout* L, int* held) {
    int fg = 90;       // 深灰

    // 方向键: 十字, 用 5 个方块拼
    int a = L->dpad_arm, t = a * 2 / 3;
    int cx = L->dpad_cx, cy = L->dpad_cy;
    // 竖条
    fb_fill_rect(fb, cx - t/2, cy - a*3/2, t, a*3, fg);
    // 横条
    fb_fill_rect(fb, cx - a*3/2, cy - t/2, a*3, t, fg);

    // AB 圆（用内接方 + 四角近似）
    fb_fill_rect(fb, L->btn_a_cx - L->btn_r + 12, L->btn_a_cy - L->btn_r + 12,
                 L->btn_r*2 - 24, L->btn_r*2 - 24, fg);
    fb_fill_rect(fb, L->btn_b_cx - L->btn_r + 12, L->btn_b_cy - L->btn_r + 12,
                 L->btn_r*2 - 24, L->btn_r*2 - 24, fg);

    // start / select
    fb_fill_rect(fb, L->btn_sx, L->btn_sy, L->btn_sw, L->btn_sh, fg);
    fb_fill_rect(fb, L->btn_tx, L->btn_ty, L->btn_sw, L->btn_sh, fg);

    (void)held;
}

/* ================= 输入 ================= */
struct TouchState {
    int fd = -1;
    int x = 0, y = 0;
    int down = 0;
    int max_x = 1072, max_y = 1448;
};

static bool touch_open(TouchState* T, const char* path, int sw, int sh) {
    T->fd = open(path, O_RDONLY | O_NONBLOCK);
    if (T->fd < 0) return false;
    T->max_x = sw; T->max_y = sh;
    // 读 ABS 范围
    unsigned long absbit[ABS_MAX/8+1] = {0};
    if (ioctl(T->fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0) {
        struct input_absinfo ai;
        if (ioctl(T->fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) >= 0 && ai.maximum > 0) T->max_x = ai.maximum;
        if (ioctl(T->fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) >= 0 && ai.maximum > 0) T->max_y = ai.maximum;
        if (ioctl(T->fd, EVIOCGABS(ABS_X), &ai) >= 0 && ai.maximum > 0 && T->max_x == sw) T->max_x = ai.maximum;
        if (ioctl(T->fd, EVIOCGABS(ABS_Y), &ai) >= 0 && ai.maximum > 0 && T->max_y == sh) T->max_y = ai.maximum;
    }
    fprintf(stderr, "[touch] %s range %dx%d (screen %dx%d)\n", path, T->max_x, T->max_y, sw, sh);
    return true;
}

/* 读触摸事件，更新 T->x/y/down，返回 1 表示有变化 */
static int touch_poll(TouchState* T, int sw, int sh) {
    if (T->fd < 0) return 0;
    struct input_event ev;
    int changed = 0;
    while (read(T->fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X)
                T->x = (int)((long)ev.value * sw / T->max_x);
            else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y)
                T->y = (int)((long)ev.value * sh / T->max_y);
            changed = 1;
        } else if (ev.type == EV_KEY && (ev.code == BTN_TOUCH || ev.code == BTN_LEFT)) {
            T->down = ev.value;
            changed = 1;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            if (changed) return 1;
        }
    }
    return changed;
}

/* ================= 灰度渲染 ================= */
/*
 * NES 输出 8bit 调色板索引 + 每帧调色板 (short RGB555)。
 * 转灰度: 标准亮度公式, 然后做简单有序抖动 (4x4 Bayer) 提升层次感。
 * eink 16 级灰度, Bayer 抖动能让明暗过渡不那么突兀。
 */
static const unsigned char BAYER4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* 预计算调色板灰度 */
static unsigned char g_gray_lut[256];

static void build_gray_lut(Nes_Emu::frame_t const& f) {
    for (int i = 0; i < f.palette_size; i++) {
        int c = f.palette[i] & 0xFFFF;
        int r = ((c >> 10) & 0x1F) * 255 / 31;
        int g = ((c >> 5) & 0x1F) * 255 / 31;
        int b = (c & 0x1F) * 255 / 31;
        // 亮度 (人眼感知加权)
        int y = (r * 77 + g * 150 + b * 29) >> 8;
        g_gray_lut[i] = (unsigned char)y;
    }
}

static void render_frame(FB* fb, Layout* L, Nes_Emu::frame_t const& f) {
    int sc = L->scale;
    int ox = L->game_x, oy = L->game_y;

    const unsigned char* base = f.pixels;
    long pitch = f.pitch;

    for (int sy = 0; sy < 240; sy++) {
        const unsigned char* row = base + (long)sy * pitch;
        int dy0 = oy + sy * sc;
        for (int sx = 0; sx < 256; sx++) {
            int idx = row[sx];
            int g = g_gray_lut[idx & 0xFF];

            int dx0 = ox + sx * sc;
            for (int j = 0; j < sc; j++) {
                int yy = dy0 + j;
                if (yy < 0 || yy >= fb->h) continue;
                for (int i = 0; i < sc; i++) {
                    int xx = dx0 + i;
                    if (xx < 0 || xx >= fb->w) continue;
                    // Bayer 抖动: 根据屏幕坐标加扰动, 模拟 16 级之间的中间调
                    int th = BAYER4[(yy & 3)][(xx & 3)] * 16;   // 0..240
                    int v = g + (th - 120) / 8;
                    if (v < 0) v = 0; else if (v > 255) v = 255;
                    if (fb->is_16bpp) {
                        unsigned char* p = fb->mem + (size_t)yy * fb->line_bytes + xx * 2;
                        unsigned int n = (unsigned int)v >> 3;
                        *(unsigned short*)p = (unsigned short)((n << 11) | ((n << 1) << 5) | n);
                    } else {
                        fb->mem[(size_t)yy * fb->line_bytes + xx] = (unsigned char)v;
                    }
                }
            }
        }
    }
}

/* 把当前 framebuffer 存成 PPM, 用于无真机时验证渲染 */
static void fb_save_ppm(FB* fb, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "写 %s 失败: %s\n", path, strerror(errno)); return; }
    fprintf(f, "P6\n%d %d\n255\n", fb->w, fb->h);
    for (int y = 0; y < fb->h; y++) {
        for (int x = 0; x < fb->w; x++) {
            int g;
            if (fb->is_16bpp) {
                unsigned short v = *(unsigned short*)(fb->mem + (size_t)y * fb->line_bytes + x * 2);
                int r = ((v >> 11) & 0x1F) * 255 / 31;
                int gg = ((v >> 5) & 0x3F) * 255 / 63;
                int b = (v & 0x1F) * 255 / 31;
                fputc(r, f); fputc(gg, f); fputc(b, f);
                continue;
            } else {
                g = fb->mem[(size_t)y * fb->line_bytes + x];
            }
            fputc(g, f); fputc(g, f); fputc(g, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[ppm] %s (%dx%d)\n", path, fb->w, fb->h);
}

/* ================= ROM 扫描 ================= */
static bool ends_with_ines(const char* n) {
    size_t l = strlen(n);
    return l > 4 && (strcasecmp(n + l - 4, ".nes") == 0);
}

static void scan_roms(const std::vector<std::string>& dirs, std::vector<std::string>& out) {
    for (size_t d = 0; d < dirs.size(); d++) {
        DIR* dp = opendir(dirs[d].c_str());
        if (!dp) continue;
        struct dirent* e;
        while ((e = readdir(dp)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            if (!ends_with_ines(e->d_name)) continue;
            std::string full = dirs[d] + "/" + e->d_name;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                out.push_back(full);
        }
        closedir(dp);
    }
    std::sort(out.begin(), out.end());
}

/* 整个 ROM 读进内存, 用 Mem_File_Reader 喂给核心
 * (QuickNES 的 blargg_config.h 里 DISABLE_AUTO_FILE=1, 路径构造被禁用) */
struct RomBuf {
    std::vector<unsigned char> data;
    bool load(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "fopen %s: %s\n", path, strerror(errno)); return false; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz <= 0) { fclose(f); fprintf(stderr, "空文件\n"); return false; }
        fseek(f, 0, SEEK_SET);
        data.resize((size_t)sz);
        size_t rd = fread(data.data(), 1, (size_t)sz, f);
        fclose(f);
        if (rd != (size_t)sz) { fprintf(stderr, "读取不完整\n"); return false; }
        return true;
    }
};

/* ================= main ================= */
static void usage(const char* argv0) {
    fprintf(stderr,
        "用法: %s [选项] <rom.nes>\n"
        "  -f <fb>       framebuffer 设备 (默认 /dev/fb0)\n"
        "  -t <event>    触摸设备 (默认自动检测)\n"
        "  -s <1-5>      缩放倍数 (默认按屏幕自动)\n"
        "  -r <dir>      ROM 目录 (可多次)\n"
        "  -j <帧跳>     每 N 帧渲染 1 次 (默认 1)\n"
        "  -b            跳过开场, 直接跑基准测试\n"
        "  -p <file.ppm> 退出前把屏幕存成 PPM (调试用)\n"
        "  -n <帧数>     跑够 N 帧后自动退出 (调试用)\n"
        "  -d            干跑模式: 不碰 framebuffer, 只验证核心和渲染逻辑\n",
        argv0);
}

int main(int argc, char** argv) {
    const char* fbdev = "/dev/fb0";
    const char* todev = nullptr;
    int force_scale = 0;
    int frame_skip = 1;
    int bench = 0;
    const char* ppm_path = nullptr;
    long max_frames = 0;
    int dry_run = 0;
    std::vector<std::string> romdirs;
    std::string rompath;

    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:r:j:p:n:dbh")) != -1) {
        switch (opt) {
            case 'f': fbdev = optarg; break;
            case 't': todev = optarg; break;
            case 's': force_scale = atoi(optarg); break;
            case 'r': romdirs.push_back(optarg); break;
            case 'j': frame_skip = atoi(optarg); if (frame_skip < 1) frame_skip = 1; break;
            case 'b': bench = 1; break;
            case 'p': ppm_path = optarg; break;
            case 'n': max_frames = atol(optarg); break;
            case 'd': dry_run = 1; break;
            case 'h': default: usage(argv[0]); return 1;
        }
    }
    if (optind < argc) rompath = argv[optind];

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    FB fb;
    std::vector<unsigned char> dry_mem;

    if (dry_run) {
        // 干跑: 用内存缓冲冒充 PW3 屏幕, 不碰任何设备
        fb.w = 1072;
        fb.h = 1448;
        fb.bits = 16;
        fb.is_16bpp = 1;
        fb.line_bytes = fb.w * 2;
        fb.maplen = (size_t)fb.line_bytes * fb.h;
        dry_mem.assign(fb.maplen, 0);
        fb.mem = dry_mem.data();
        // 触摸直接用内存假变量 (保留空 fd)
        fprintf(stderr, "[dry] 干跑模式: 模拟 1072x1448 16bpp framebuffer, 不触碰设备\n");
    } else if (!fb_open(&fb, fbdev)) {
        fprintf(stderr, "无法打开 framebuffer，试试 sudo 或确认设备路径\n");
        return 1;
    }

    // Kindle eink 刷新 (非 Kindle 环境会自动退化)
    EinkRefresh eink;
    eink_init(&eink, fb.fd, 15);

    Layout L;
    layout_init(&L, fb.w, fb.h);
    if (force_scale > 0) {
        L.scale = force_scale;
        L.game_w = 256 * L.scale;
        L.game_h = 240 * L.scale;
        L.game_x = (fb.w - L.game_w) / 2;
        L.game_y = 10;
    }
    fprintf(stderr, "[layout] game %dx%d @ (%d,%d) scale=%d, pad_y=%d h=%d\n",
            L.game_w, L.game_h, L.game_x, L.game_y, L.scale, L.pad_area_y, L.pad_area_h);

    // 触摸设备
    TouchState touch;
    if (dry_run) {
        touch.fd = -1;
    } else if (!todev) {
        const char* cands[] = {"/dev/input/event1", "/dev/input/event0",
                               "/dev/input/event2", "/dev/input/event3"};
        for (int i = 0; i < 4; i++) {
            if (touch_open(&touch, cands[i], fb.w, fb.h)) { todev = cands[i]; break; }
        }
        if (!todev) fprintf(stderr, "[touch] 没找到触摸设备，仅支持物理键\n");
    } else {
        if (!touch_open(&touch, todev, fb.w, fb.h))
            fprintf(stderr, "[touch] 打不开 %s\n", todev);
    }

    // 没有 ROM: 扫描并提示
    if (rompath.empty()) {
        if (romdirs.empty()) {
            romdirs.push_back("/mnt/us/roms");
            romdirs.push_back("/mnt/us/documents/roms");
            romdirs.push_back("./roms");
        }
        std::vector<std::string> found;
        scan_roms(romdirs, found);
        if (found.empty()) {
            fprintf(stderr, "没找到 .nes 文件。用法: %s /mnt/us/roms/game.nes\n", argv[0]);
            return 1;
        }
        if (found.size() == 1) {
            rompath = found[0];
        } else {
            fprintf(stderr, "找到 %zu 个 ROM:\n", found.size());
            for (size_t i = 0; i < found.size(); i++)
                fprintf(stderr, "  [%zu] %s\n", i + 1, found[i].c_str());
            fprintf(stderr, "请用参数指定其中一个\n");
            return 1;
        }
    }

    // 初始化模拟器
    Nes_Emu emu;
    // buffer_height() 是运行时值, 用 vector
    std::vector<unsigned char> vidbuf((size_t)Nes_Emu::buffer_width * 512, 0);
    emu.set_pixels(vidbuf.data(), Nes_Emu::buffer_width);

    RomBuf rom;
    if (!rom.load(rompath.c_str())) return 1;
    Mem_File_Reader reader(rom.data.data(), (long)rom.data.size());
    blargg_err_t err = emu.load_ines(Auto_File_Reader(reader));
    if (err) {
        fprintf(stderr, "ROM 加载失败: %s\n", err);
        return 1;
    }
    emu.set_sample_rate(44100);
    fprintf(stderr, "[rom] %s  (%ld bytes) mapper=%d\n",
            rompath.c_str(), (long)rom.data.size(), (int)emu.cart()->mapper_code());

    // 调试: 跑一帧, dump 调色板和像素索引分布
    if (getenv("EINK_NES_DEBUG")) {
        // 跑 10 帧, 让 ROM 的初始化代码执行完
        for (int i = 0; i < 10; i++) emu.emulate_frame(0, 0);
        Nes_Emu::frame_t const& df = emu.frame();
        fprintf(stderr, "[dbg] palette_size=%d begin=%d top=%d pitch=%ld\n",
                df.palette_size, df.palette_begin, df.top, df.pitch);
        for (int i = 0; i < df.palette_size && i < 32; i++)
            fprintf(stderr, "[dbg] pal[%d] = 0x%04X\n", i, (unsigned short)df.palette[i]);
        int hist[256] = {0};
        for (int y = 0; y < 240; y++) {
            const unsigned char* r = df.pixels + (long)y * df.pitch;
            for (int x = 0; x < 256; x++) hist[r[x]]++;
        }
        fprintf(stderr, "[dbg] pixel index histogram (nonzero):\n");
        for (int i = 0; i < 256; i++) if (hist[i]) fprintf(stderr, "   idx %3d: %6d\n", i, hist[i]);
        build_gray_lut(df);
        fprintf(stderr, "[dbg] gray lut:");
        for (int i = 0; i < df.palette_size && i < 32; i++) fprintf(stderr, " %d", g_gray_lut[i]);
        fprintf(stderr, "\n");
        return 0;
    }

    // 画初始界面并全刷一次 (虚拟键只在启动时刷, 之后不重复刷)
    fb_fill_rect(&fb, 0, 0, fb.w, fb.h, 250);
    draw_controls(&fb, &L, nullptr);
    eink_full(&eink, fb.w, fb.h);

    // 主循环
    int joy = 0;
    unsigned long frames = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_quit) {
        // 输入
        int ch = touch_poll(&touch, fb.w, fb.h);
        if (ch) {
            joy = touch.down ? layout_hit(&L, touch.x, touch.y) : 0;
        }

        err = emu.emulate_frame(joy, 0);
        if (err) {
            fprintf(stderr, "emulate_frame: %s\n", err);
            break;
        }
        frames++;

        if (frames % (unsigned long)frame_skip == 0) {
            Nes_Emu::frame_t const& f = emu.frame();
            build_gray_lut(f);
            render_frame(&fb, &L, f);

            // 通知 eink 刷新。只刷游戏区 (虚拟键是静态的, 不参与每帧刷新,
            // 否则会大幅拖慢帧率且造成无谓残影)
            eink_tick(&eink, L.game_x, L.game_y, L.game_w, L.game_h, fb.w, fb.h);
        }

        if (max_frames > 0 && frames >= (unsigned long)max_frames) break;

        if (bench && frames % 60 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            fprintf(stderr, "[bench] %lu frames in %.2fs = %.1f fps\n", frames, dt, frames / dt);
            if (frames >= 600) break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    fprintf(stderr, "\n[exit] %lu frames, %.2fs, %.1f fps avg\n", frames, dt, frames / dt);

    if (ppm_path) fb_save_ppm(&fb, ppm_path);

    if (dry_run) {
        // 自检: 统计游戏区有多少个不同的灰度值
        int hist[256] = {0};
        for (int y = L.game_y; y < L.game_y + L.game_h && y < fb.h; y++) {
            for (int x = L.game_x; x < L.game_x + L.game_w && x < fb.w; x++) {
                unsigned short v = *(unsigned short*)(fb.mem + (size_t)y * fb.line_bytes + x * 2);
                unsigned int n = (v >> 11) & 0x1F;
                hist[n * 255 / 31]++;
            }
        }
        int levels = 0;
        for (int i = 0; i < 256; i++) if (hist[i]) levels++;
        fprintf(stderr, "[dry] 游戏区灰度层次: %d\n", levels);
        if (levels < 3) {
            fprintf(stderr, "[dry] 失败: 灰度层次不足 (%d < 3), 渲染管线异常\n", levels);
            return 2;
        }
        fprintf(stderr, "[dry] PASS\n");
    }

    eink_finish(&eink, fb.w, fb.h);

    if (!dry_run) {
        munmap(fb.mem, fb.maplen);
        close(fb.fd);
    }
    if (touch.fd >= 0) close(touch.fd);
    return 0;
}
