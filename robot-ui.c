/* robot-ui.c — 3.1" 480x800(panel) LVGL UI for the RK3566 robot assistant,
 * rendered LANDSCAPE 800x480 (rotated blit into the portrait fbdev,
 * GAME_UI_ROT=90 mapping; ROBOT_UI_ROT=270 flips the other way).
 *
 * Screen: /dev/fb0 (rockchipdrmfb, XRGB8888 32bpp) driven directly;
 * touch: first evdev device with ABS_MT_POSITION_X (my_touch, whose
 * pointer emulation provides ABS_X/ABS_Y + BTN_TOUCH).
 *
 * Pages:
 *   home  — launcher with two app tiles (voice assistant / camera)
 *   voice — 待机/聆听/思考/播报 state chips + expression placeholder +
 *           scrolling dialogue log; driven by /tmp/robot-ui.fifo lines:
 *             state idle|wake|listen|think|speak
 *             user <text>     (ASR result)
 *             robot <text>    (reply being spoken)
 *           (va/board_main.py writes these events)
 *   cam   — camera preview (v4l2 capture placeholder in this version)
 *
 * The service unbinds fbcon before start so printk cannot scribble over
 * the UI; console output stays on the serial port.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/videodev2.h>

#include "lvgl.h"
#include "grok_face_data.h"

/* UI 逻辑分辨率：横屏 800x480；fb 实际仍是竖屏 480x800，flush 时旋转 */
#define HRES 800
#define VRES 480
#define UI_FIFO "/tmp/robot-ui.fifo"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 旋转方向（90 默认，与游戏机 GAME_UI_ROT=90 同款；270 = 另一侧横握） */
static int ROT = 90;

LV_FONT_DECLARE(lv_font_cn20);

/* ---------------- time ---------------- */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static uint32_t tick_cb(void) { return (uint32_t)now_ms(); }

/* ---------------- fbdev ---------------- */
static struct {
    int fd;
    uint8_t *map;
    size_t map_len;
    uint32_t w, h, stride, bpp;
} FB = { .fd = -1 };

static void fb_open(const char *dev)
{
    FB.fd = open(dev, O_RDWR);
    if (FB.fd < 0) { fprintf(stderr, "robot-ui: open %s: %s\n", dev, strerror(errno)); exit(1); }
    struct fb_fix_screeninfo fin;
    struct fb_var_screeninfo vin;
    if (ioctl(FB.fd, FBIOGET_FSCREENINFO, &fin) < 0 ||
        ioctl(FB.fd, FBIOGET_VSCREENINFO, &vin) < 0) {
        fprintf(stderr, "robot-ui: fb ioctls failed\n"); exit(1);
    }
    FB.w = vin.xres; FB.h = vin.yres;
    FB.bpp = vin.bits_per_pixel;
    FB.stride = fin.line_length;
    FB.map_len = fin.smem_len;
    FB.map = mmap(NULL, FB.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, FB.fd, 0);
    if (FB.map == MAP_FAILED) { fprintf(stderr, "robot-ui: mmap failed\n"); exit(1); }
    fprintf(stderr, "robot-ui: fb %s %ux%u %ubpp stride %u\n",
            dev, FB.w, FB.h, FB.bpp, FB.stride);
    if (FB.bpp != 32) { fprintf(stderr, "robot-ui: need 32bpp fb (got %u)\n", FB.bpp); exit(1); }
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    int x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    const uint32_t *src = (const uint32_t *)px_map;
    uint32_t *fb = (uint32_t *)FB.map;
    size_t stride = FB.stride / 4;
    int w = x2 - x1 + 1;
    if (ROT == 90) {
        /* UI(x,y) -> fb(col=y, row=HRES-1-x) */
        for (int y = y1; y <= y2; y++) {
            const uint32_t *s = &src[(size_t)(y - y1) * w];
            uint32_t *d = &fb[(size_t)(HRES - 1 - x2) * stride + y];
            for (int x = x2; x >= x1; x--)
                d[stride * (size_t)(x2 - x)] = s[x - x1];
        }
    } else if (ROT == 270) {
        /* UI(x,y) -> fb(col=VRES-1-y, row=x) */
        for (int y = y1; y <= y2; y++) {
            const uint32_t *s = &src[(size_t)(y - y1) * w];
            uint32_t *d = &fb[(size_t)x1 * stride + (VRES - 1 - y)];
            for (int x = x1; x <= x2; x++)
                d[stride * (size_t)(x - x1)] = s[x - x1];
        }
    } else {
        for (int y = y1; y <= y2; y++)
            memcpy(&fb[(size_t)y * stride + x1], &src[(size_t)(y - y1) * w], (size_t)w * 4);
    }
    lv_display_flush_ready(disp);
}

/* ---------------- evdev touch (via pointer emulation axes) ---------------- */
static struct {
    int fd;
    int tmax_x, tmax_y;
} TS = { .fd = -1 };

static struct { int x, y; char pressed; } touch;
static uint64_t press_ms;
static char pending_release;

static void ts_init(void)
{
    for (int i = 0; i < 32 && TS.fd < 0; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char absbits[(ABS_MAX + 1) / 8] = {0};
        unsigned char evbits[(EV_MAX + 1) / 8] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0 &&
            (evbits[EV_ABS / 8] & (1 << (EV_ABS % 8))) &&
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 &&
            (absbits[ABS_MT_POSITION_X / 8] & (1 << (ABS_MT_POSITION_X % 8)))) {
            struct input_absinfo ai;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0) TS.tmax_x = ai.maximum;
            if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0) TS.tmax_y = ai.maximum;
            TS.fd = fd;
            fprintf(stderr, "robot-ui: touch %s (abs max %d,%d)\n", path, TS.tmax_x, TS.tmax_y);
            continue;
        }
        close(fd);
    }
    if (TS.fd < 0) fprintf(stderr, "robot-ui: no evdev touchscreen found\n");
}

static void ts_map(int rx, int ry)
{
    /* raw panel coords -> fb coords -> (rotated) UI coords */
    int fx = (int)((int64_t)rx * FB.w / (TS.tmax_x + 1));
    int fy = (int)((int64_t)ry * FB.h / (TS.tmax_y + 1));
    if (ROT == 90) { touch.x = HRES - 1 - fy; touch.y = fx; }
    else if (ROT == 270) { touch.x = fy; touch.y = VRES - 1 - fx; }
    else { touch.x = fx; touch.y = fy; }
}

static void ts_poll(void)
{
    static uint64_t last_try;
    if (TS.fd < 0) {
        uint64_t now = now_ms();
        if (now - last_try >= 1000) { last_try = now; ts_init(); }
        if (TS.fd < 0) return;
    }
    struct input_event ev;
    static int rx, ry, have_xy;
    while (read(TS.fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) { rx = ev.value; have_xy |= 1; }
            else if (ev.code == ABS_Y) { ry = ev.value; have_xy |= 2; }
            if (touch.pressed && have_xy == 3) ts_map(rx, ry);
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (!have_xy) continue;
            ts_map(rx, ry);
            if (ev.value) {
                touch.pressed = 1; press_ms = now_ms(); pending_release = 0;
            } else {
                if (now_ms() - press_ms < 40) pending_release = 1;
                else touch.pressed = 0;
            }
        }
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = touch.x;
    data->point.y = touch.y;
    data->state = touch.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---------------- colors / styles ---------------- */
#define COL_BG      lv_color_hex(0x101418)
#define COL_CARD    lv_color_hex(0x1c2430)
#define COL_ACCENT  lv_color_hex(0x2f9dff)
#define COL_ACCENT2 lv_color_hex(0x18b77a)
#define COL_TEXT    lv_color_hex(0xe8eef5)
#define COL_DIM     lv_color_hex(0x8894a3)
#define COL_USER    lv_color_hex(0x2f9dff)
#define COL_ROBOT   lv_color_hex(0x222c38)

/* ---------------- screens ---------------- */
static lv_obj_t *scr_home, *scr_voice, *scr_cam;

static void style_base(lv_obj_t *obj, lv_obj_t *parent)
{
    lv_obj_set_size(obj, HRES, VRES);
    lv_obj_set_style_bg_color(obj, COL_BG, 0);
    lv_obj_center(obj);
    (void)parent;
}

static lv_obj_t *label_cn(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *lb = lv_label_create(parent);
    lv_label_set_text(lb, txt);
    lv_obj_set_style_text_font(lb, &lv_font_cn20, 0);
    lv_obj_set_style_text_color(lb, COL_TEXT, 0);
    return lb;
}

/* ============ home ============ */
static void goto_voice(lv_event_t *e);
static void goto_cam(lv_event_t *e);

static lv_obj_t *make_tile(lv_obj_t *parent, const char *title, const char *sub,
                           lv_color_t accent, lv_event_cb_t cb)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_size(tile, 350, 300);
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(tile, COL_CARD, 0);
    lv_obj_set_style_border_color(tile, accent, 0);
    lv_obj_set_style_border_width(tile, 3, 0);
    lv_obj_set_style_radius(tile, 24, 0);
    lv_obj_t *lb = label_cn(tile, title);
    lv_obj_set_style_text_color(lb, accent, 0);
    lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *sb = label_cn(tile, sub);
    lv_obj_set_style_text_color(sb, COL_DIM, 0);
    lv_obj_align(sb, LV_ALIGN_TOP_MID, 0, 190);
    return tile;
}

static void home_build(void)
{
    scr_home = lv_obj_create(NULL);
    style_base(scr_home, NULL);
    lv_obj_t *title = label_cn(scr_home, "机器人助手");
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_t *tile_v = make_tile(scr_home, "语音助手", "唤醒词对话 · 聆听/思考/播报",
                                  COL_ACCENT, goto_voice);
    lv_obj_align(tile_v, LV_ALIGN_LEFT_MID, 40, 30);
    lv_obj_t *tile_c = make_tile(scr_home, "摄像头", "OV5695 实时预览",
                                  COL_ACCENT2, goto_cam);
    lv_obj_align(tile_c, LV_ALIGN_RIGHT_MID, -40, 30);
    lv_obj_t *foot = label_cn(scr_home, "robot-assistant  v0.1");
    lv_obj_set_style_text_color(foot, COL_DIM, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* ============ shared top bar with back ============ */
static void voice_engine(int on);   /* 语音页门控（定义在 voice 段） */

static void back_to_home(lv_event_t *e)
{
    (void)e;
    voice_engine(0);   /* 退回首页即停语音链（stop 已停的服务无害，cam 页共用） */
    lv_screen_load_anim(scr_home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

static void topbar_build(lv_obj_t *scr, const char *title)
{
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 96, 56);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 12);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_color(back, COL_DIM, 0);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_to_home, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = label_cn(scr, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 24);
}

/* ============ voice page ============ */
enum voice_state { ST_IDLE, ST_WAKE, ST_LISTEN, ST_THINK, ST_SPEAK };
static const char *st_names[] = { "待机", "唤醒", "聆听", "思考", "播报" };
static lv_obj_t *st_badge;      /* 标题行右侧：仅显示当前状态 */
static lv_obj_t *st_badge_lb;
static lv_obj_t *st_spinner;
static lv_obj_t *dlg_cont;

/* ============ expression engine（Grok Bot 角色移植） ============
 * 几何数据/动画参数源：Grok Bot.app v0.18.0（经 blessonism/grok-icon-study
 * 提取研究；素材归 xAI/相应权利人，本项目个人学习用途，不商用不再分发）。
 * tools/gen_grok_face.py 生成 grok_face_data.h；本段为引擎 C 移植：
 * 弹簧姿态机 + 眼形播放列表 + 眨眼队列 + 视线游走 + 眼睛圆盘 3D 投影
 * （body 随 spin 旋转而眼睛层不转 = Grok 质感的来源）。
 * 状态映射：idle→idle  wake→waking  listen→listening
 *           think→thinking  speak→happy（无嘴设计：笑眼+弹跳表达播报） */
#define EXPR_W 400
#define EXPR_H 400
#define SS 2                        /* 2x 超采样抗锯齿 */
#define COV_W (EXPR_W * SS)
#define COV_H (EXPR_H * SS)
/* vendor 脸=blob 圆心(114.86,114.23) 半径~114；VIEW_K 使脸直径=EXPR_W-28，
 * 中心钉在画布中心（gf_view_to_ss / fx_v2c 统一此变换） */
#define VIEW_K ((EXPR_W - 28.0) / 230.0)
#define SPAN_E 160

enum grok_mood { GM_IDLE, GM_WAKE, GM_LISTEN, GM_THINK, GM_HAPPY };

/* 每状态：眼形播放列表（索引见 GROK_EYES）/ 保持时长 ms / 眨眼节奏 ms。
 * idle 16 形大轮播（用户要求"明显"）：平静锚点(0,8) + curious(3,21)/
 * searching(9,20,12,18,15)/working(7,16,11,10)/happy(2,17)/
 * suspicious(14)，随机步进不重复，换形更快更弹 */
static const int GM_PLAY[5][16] = {
    { 0, 3, 21, 9, 20, 12, 18, 15, 7, 16, 11, 10, 2, 17, 14, 8 }, /* idle */
    { 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 10, 1, 19, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 8, 16, 14, 17, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 2, 11, 17, 19, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
};
static const int GM_PLAY_N[5] = { 16, 1, 3, 5, 4 };
/* 眼形中文名（按 vendor EYE_PLAYLIST 归属归类：0/8=idle 平静、3/21=
 * curious/surprised 好奇、15/9/20/12/18=searching 寻找、7/16/11/10=
 * working 专注、2/17=happy 开心、14/5=suspicious/thinking、1/19=
 * listening、13=waking、4/22=sleeping、23=suspicious、24=shy；6 未用） */
static const char *GF_SHAPE_NAMES[25] = {
    "平静", "聆听", "开心", "好奇", "困倦", "思考", "神秘", "专注",
    "平静", "寻找", "专注", "专注", "寻找", "唤醒", "怀疑", "寻找",
    "专注", "开心", "寻找", "聆听", "寻找", "好奇", "困倦", "怀疑", "害羞",
};
static const int GM_HOLD[5][2] = {
    { 3000, 5000 }, { 800, 800 }, { 2800, 5000 },
    { 2000, 3600 }, { 2500, 4500 },
};
static const int GM_BLINK[5][2] = {
    { 6000, 14000 }, { 0, 0 }, { 3000, 7000 },
    { 3500, 7000 }, { 2500, 5000 },
};

/* SPRINGS：频率 / 阻尼（tables.js 原值） */
enum { G_SPIN, G_TX, G_TY, G_SQUASH, G_BLINK, G_ESCL, G_GZX, G_GZY };
static const double GM_SPR[8][2] = {
    { 5, 0.9 },   { 3.5, 1 }, { 4, 1 },    { 10, 0.8 },
    { 26, 1 },    { 9, 0.85 }, { 13, 1 },  { 13, 1 },
};

/* FACE_TUNE（loginWrap） */
#define FT_GAP   1.18
#define FT_H     1.0
#define FT_EW    0.96
#define FT_EH    0.92

typedef struct { double x, v, t; } GSpring;

static void gs_step(GSpring *s, double freq, double damp, double dt)
{
    s->v += (-2 * damp * freq * s->v - freq * freq * (s->x - s->t)) * dt;
    s->x += s->v * dt;
    if (!isfinite(s->x) || !isfinite(s->v)) {
        s->x = s->t;
        s->v = 0;
    }
}

static double dclamp(double v, double lo, double hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}
static double drand(double a, double b)
{
    return a + (double)rand() / (double)RAND_MAX * (b - a);
}
static double dstep(double t)          /* Dke smoothstep */
{
    t = dclamp(t, 0, 1);
    return t * t * (3 - 2 * t);
}

static struct {
    int mood;
    uint64_t t0, state_at, last_tick;
    GSpring s[8];
    GSpring morph;                     /* 眼形 morph 0→1 */
    int eye_from, eye_to, eye_idx, stiff, from_snap;
    float from_polys[2][GROK_VERT_N][2];
    uint64_t eye_until, blink_until, gaze_until;
    struct { uint64_t at; double v; } bq[8];
    int bq_n;
    uint64_t nod_until, nod_end;
    int waking_blinked, wake_blink;
    double pose_lid;
    double span_l[SPAN_E], span_r[SPAN_E], span_top, span_bot;
    uint8_t cov[COV_W * COV_H];
} GF;

/* spinTurn 整圈旋转弹簧（FX 段用，tables.js spinTurn [6.2,1]；gf_render 叠加） */
static GSpring FX_ST;
static int FX_ST_on;

static uint32_t expr_buf[EXPR_W * EXPR_H];
static lv_obj_t *expr_canvas;
static lv_obj_t *expr_name_lb;   /* 表情上方：当前眼形名 */

/* blob 静态轮廓行跨度表（paintEyes x 限位用；math.js buildSpan 移植） */
static void gf_span_init(void)
{
    double ymin = 1e9, ymax = -1e9;
    for (int i = 0; i < GROK_BLOB_N; i++) {
        double y = GROK_BLOB_PTS[i][1];
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    GF.span_top = ymin;
    GF.span_bot = ymax;
    for (int m = 0; m < SPAN_E; m++) {
        double fy = ymin + (ymax - ymin) * (m + 0.5) / SPAN_E;
        double h = -1e18, r = 1e18;
        for (int i = 0; i < GROK_BLOB_N; i++) {
            const float *p = GROK_BLOB_PTS[i];
            const float *q = GROK_BLOB_PTS[(i + 1) % GROK_BLOB_N];
            if ((p[1] <= fy) == (q[1] <= fy)) continue;
            double ex = p[0] + (q[0] - p[0]) * (fy - p[1]) / (q[1] - p[1]);
            if (ex <= GROK_RE) { if (ex > h) h = ex; }
            else if (ex < r) r = ex;
        }
        GF.span_l[m] = h < -1e17 ? GROK_RE : h;
        GF.span_r[m] = r > 1e17 ? GROK_RE : r;
    }
}

static void gf_span_at(double y, double *l, double *r)
{
    double t = dclamp((y - GF.span_top) / (GF.span_bot - GF.span_top)
                      * SPAN_E - 0.5, 0, SPAN_E - 1);
    int k = (int)t;
    double f = t - k;
    int k2 = k + 1 < SPAN_E ? k + 1 : SPAN_E - 1;
    *l = GF.span_l[k] + (GF.span_l[k2] - GF.span_l[k]) * f;
    *r = GF.span_r[k] + (GF.span_r[k2] - GF.span_r[k]) * f;
}

/* 眨眼队列（eyes.js queueBlink 移植；14% 概率双眨） */
static void gf_bq_push(uint64_t at, double v)
{
    if (GF.bq_n >= 8) return;
    GF.bq[GF.bq_n].at = at;
    GF.bq[GF.bq_n].v = v;
    GF.bq_n++;
}
static void gf_blink_queue(uint64_t now)
{
    gf_bq_push(now, 0.05);
    gf_bq_push(now + 70, 0.05);
    gf_bq_push(now + 150, 1.08);
    gf_bq_push(now + 300, 1.0);
    if (rand() % 100 < 14) {
        gf_bq_push(now + 370, 0.05);
        gf_bq_push(now + 480, 1.0);
    }
}

/* 眼形 morph（_morphEyes/_currentPolys 移植：打断时快照当前插值） */
static void gf_current_polys(double t, float out[2][GROK_VERT_N][2])
{
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < GROK_VERT_N; j++) {
            const float *f = GF.from_snap
                ? GF.from_polys[i][j] : GROK_EYES[GF.eye_from][i][j];
            const float *to = GROK_EYES[GF.eye_to][i][j];
            out[i][j][0] = f[0] + (to[0] - f[0]) * (float)t;
            out[i][j][1] = f[1] + (to[1] - f[1]) * (float)t;
        }
}

static void gf_morph(int idx, int stiff)
{
    if (expr_name_lb) lv_label_set_text(expr_name_lb, GF_SHAPE_NAMES[idx]);
    if (idx == GF.eye_to && GF.morph.t == 1) return;
    double t = dclamp(GF.morph.x, 0, 1);
    GF.eye_from = GF.eye_to;
    gf_current_polys(t, GF.from_polys);
    GF.from_snap = 1;
    GF.eye_to = idx;
    GF.morph.x = 0;
    GF.morph.v = 0;
    GF.morph.t = 1;
    GF.stiff = stiff;
}

static void fx_on_mood(int mood);   /* FX 粒子层（见 gf_render 之后） */

static void gf_set_mood(int mood, int reset_eyes)
{
    if (mood == GF.mood && !reset_eyes) return;
    uint64_t now = now_ms();
    GF.mood = mood;
    GF.state_at = now;
    GF.eye_idx = 0;
    if (reset_eyes) {
        GF.eye_from = GM_PLAY[mood][0];
        GF.eye_to = GM_PLAY[mood][0];
        GF.from_snap = 0;
        GF.morph.x = 1; GF.morph.v = 0; GF.morph.t = 1;
    } else if (mood != GM_WAKE) {
        gf_morph(GM_PLAY[mood][0], 8);
    }
    GF.eye_until = now + (uint64_t)drand(GM_HOLD[mood][0], GM_HOLD[mood][1]);
    GF.blink_until = GM_BLINK[mood][0]
        ? now + (uint64_t)drand(1500, 7000) : (uint64_t)-1;
    GF.gaze_until = now + (uint64_t)drand(500, 1400);
    GF.nod_until = now + 1800;
    GF.nod_end = 0;
    GF.waking_blinked = 0;
    GF.wake_blink = 0;
    if (mood != GM_WAKE)
        gf_blink_queue(now);
    fx_on_mood(mood);
}

/* 姿态（pose.js applyPose 五分支移植；waking 的 wakeEye/wakeBlink 副作用
 * 以 gf_morph/标志位实现，粒子/burst 不移植） */
static void gf_pose(double mt, double dts, uint64_t now,
                    double *spin, double *tx, double *ty, double *squash,
                    double *lid, double *boost)
{
    *lid = 1; *boost = 1; *spin = 0; *tx = 0; *ty = 0; *squash = 1;
    switch (GF.mood) {
    case GM_IDLE:
        *spin = sin(mt * 0.5) * 1.5 + sin(mt * 0.17) * 0.6;
        *tx = sin(mt * 0.27) * 1;
        *ty = sin(mt * 0.85) * 1.2;
        *squash = 1 + sin(mt * 0.85) * 0.007;
        break;
    case GM_WAKE:
        if (dts < 0.5) {
            *lid = 0.07; *ty = 6;
            gf_morph(3, 12);
        } else if (dts < 1.2) {
            *lid = 1; *boost = 1.12; *ty = -5; *squash = 1.04;
        } else if (dts < 2.2) {
            gf_morph(0, 7);
            if (dts < 1.4) GF.wake_blink = 1;
        } else {
            double et = dclamp((dts - 2.2) / 0.8, 0, 1);
            gf_morph(0, 7);
            *spin = sin(et * M_PI * 3) * 6 * (1 - et);
            *ty = sin(mt * 0.9) * 2;
        }
        break;
    case GM_LISTEN: {
        *spin = 8 + sin(mt * 0.5) * 1.5;
        *tx = 2;
        *ty = -2 + sin(mt * 0.8) * 0.8;
        *squash = 1.015;
        if (now >= GF.nod_until) {
            GF.nod_until = now + (uint64_t)drand(1800, 3200);
            GF.nod_end = now + 380;
        }
        if (now < GF.nod_end) {
            double et = 1 - (double)(GF.nod_end - now) / 380;
            *ty += sin(et * M_PI) * 4.5;
            *spin += sin(et * M_PI) * 2;
        }
        break;
    }
    case GM_THINK:
        *spin = -9 + sin(mt * 0.35) * 5;
        *tx = sin(mt * 0.3) * 5;
        *ty = sin(mt * 0.6) * 2.5;
        break;
    case GM_HAPPY: {
        double et = sin(mt * 2.4);
        *spin = sin(mt * 1.2) * 3;
        *tx = sin(mt * 1.1) * 2.5;
        *ty = -fabs(et) * 3;
        *squash = 1 + et * 0.02;
        *boost = 1.05;
        break;
    }
    }
}

/* 视线游走（pose.js nextGaze 移植） */
static void gf_next_gaze(double *gx, double *gy, double *hold)
{
    switch (GF.mood) {
    case GM_IDLE:
        *gx = 0; *gy = 0; *hold = drand(2500, 5500); break;
    case GM_WAKE:
        *gx = drand(-0.4, 0.4) * 15; *gy = drand(-0.3, 0.3) * 9;
        *hold = drand(2500, 5000); break;
    case GM_LISTEN:
        *gx = drand(-0.3, 0.3) * 15; *gy = drand(-0.25, 0.25) * 9;
        *hold = drand(2200, 4200); break;
    case GM_THINK:
        *gx = (rand() % 2 ? -1.0 : 1.0) * drand(0.5, 1) * 15;
        *gy = -drand(0.4, 1) * 9;
        *hold = drand(1500, 2800); break;
    default:                          /* happy */
        *gx = drand(-0.7, 0.7) * 15; *gy = -drand(0, 0.6) * 9;
        *hold = drand(1800, 3400); break;
    }
}

static void gf_view_to_ss(double vx, double vy, float *ox, float *oy)
{
    *ox = (float)(((vx - GROK_RE) * VIEW_K + EXPR_W * 0.5) * SS);
    *oy = (float)(((vy - GROK_RE) * VIEW_K + EXPR_H * 0.5) * SS);
}

/* 多边形扫描线填充进超采样覆盖缓冲（set=1 填 / 0 擦=镂空眼睛） */
static void gf_cov_fill(const float pts[][2], int n, int set)
{
    float ymin = 1e18f, ymax = -1e18f;
    for (int i = 0; i < n; i++) {
        if (pts[i][1] < ymin) ymin = pts[i][1];
        if (pts[i][1] > ymax) ymax = pts[i][1];
    }
    int y0 = (int)ymin, y1 = (int)ymax;
    if (y1 < 0 || y0 > COV_H - 1) return;
    if (y0 < 0) y0 = 0;
    if (y1 > COV_H - 1) y1 = COV_H - 1;
    float xs[64];
    for (int y = y0; y <= y1; y++) {
        float yc = y + 0.5f;
        int nx = 0;
        for (int i = 0; i < n && nx < 64; i++) {
            const float *p = pts[i];
            const float *q = pts[(i + 1) % n];
            if ((p[1] <= yc) == (q[1] <= yc)) continue;
            xs[nx++] = p[0] + (q[0] - p[0]) * (yc - p[1]) / (q[1] - p[1]);
        }
        for (int a = 1; a < nx; a++) {          /* 插入排序 */
            float v = xs[a];
            int b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < nx; a += 2) {
            int x0 = (int)xs[a], x1 = (int)xs[a + 1];
            if (x1 < 0 || x0 > COV_W - 1) continue;
            if (x0 < 0) x0 = 0;
            if (x1 > COV_W - 1) x1 = COV_W - 1;
            uint8_t *row = GF.cov + (size_t)y * COV_W + x0;
            if (set) memset(row, 255, (size_t)(x1 - x0 + 1));
            else     memset(row, 0,   (size_t)(x1 - x0 + 1));
        }
    }
}

static void gf_render(uint64_t now)
{
    memset(GF.cov, 0, sizeof(GF.cov));

    /* 身体：translate(R+tx,R+ty) rotate(spin) scale(1,squash)（_paint 移植） */
    static float body[GROK_BLOB_N][2];
    double rad = GF.s[G_SPIN].x * M_PI / 180.0
               + (FX_ST_on ? FX_ST.x : 0);   /* spinTurn 整圈（rad） */
    double cs = cos(rad), sn = sin(rad);
    for (int i = 0; i < GROK_BLOB_N; i++) {
        double px = GROK_BLOB_PTS[i][0] - GROK_RE;
        double py = (GROK_BLOB_PTS[i][1] - GROK_RE) * GF.s[G_SQUASH].x;
        double rx = px * cs - py * sn + GROK_RE + GF.s[G_TX].x;
        double ry = px * sn + py * cs + GROK_RE + GF.s[G_TY].x;
        gf_view_to_ss(rx, ry, &body[i][0], &body[i][1]);
    }
    gf_cov_fill(body, GROK_BLOB_N, 1);

    /* 眼睛（eyes.js paintEyes use3d 路径移植；turn/notify/overlay 不用） */
    double morphT = dclamp(GF.morph.x, 0, 1);
    double pulse = 1 + 0.07 * sin(morphT * M_PI);
    static float polys[2][GROK_VERT_N][2];
    gf_current_polys(morphT, polys);

    double cents[2][2], hw[2];
    for (int i = 0; i < 2; i++) {
        double sx = 0, sy = 0;
        for (int j = 0; j < GROK_VERT_N; j++) {
            sx += polys[i][j][0];
            sy += polys[i][j][1];
        }
        cents[i][0] = sx / GROK_VERT_N;
        cents[i][1] = sy / GROK_VERT_N;
        hw[i] = 0;
        for (int j = 0; j < GROK_VERT_N; j++) {
            double d = fabs(polys[i][j][0] - cents[i][0]);
            if (d > hw[i]) hw[i] = d;
        }
    }
    double sep = fabs(cents[1][0] - cents[0][0]) * FT_GAP;
    double ee = (hw[0] + hw[1] > 0.5) ? dclamp(sep / (hw[0] + hw[1]), 0.35, 4) : 4;
    double oX = dclamp(dclamp(GF.s[G_ESCL].x, 0.2, 2), 0.02, ee / pulse);
    double hee = dclamp(oX * FT_EW, 0.02, ee / pulse);
    double u1 = oX * FT_EH;

    static float eye_ss[GROK_VERT_N][2];
    for (int i = 0; i < 2; i++) {
        double gn = cents[i][0], ti = cents[i][1];
        double lid = GF.s[G_BLINK].x > 0.04 ? GF.s[G_BLINK].x : 0.04;
        double xr = (gn - GROK_RE) / GROK_RE;
        double fr = (GROK_RE - ti) / GROK_RE;
        double ia = sqrt(dclamp(1 - xr * xr - fr * fr, 0, 1));
        if (ia < 0.02) ia = 0.02;
        double li = GROK_CR[0] * xr + GROK_CR[1] * fr + GROK_CR[2] * ia;
        double bl = GROK_CR[3] * xr + GROK_CR[4] * fr + GROK_CR[5] * ia;
        double io = GROK_CR[6] * xr + GROK_CR[7] * fr + GROK_CR[8] * ia;
        double wo = li * GROK_RE * FT_GAP;
        double sre = dclamp(GROK_RE - bl * GROK_RE * FT_H,
                            GROK_TOP + 2, GROK_BOTTOM - 2);
        double uo = -fr * xr, tl = 1 - fr * fr, zi = -fr * ia;
        double yn = sqrt(uo * uo + tl * tl + zi * zi);
        if (yn < 1e-6) { uo = 0; tl = 0; zi = 1; }
        else { uo /= yn; tl /= yn; zi /= yn; }
        double md = fr * zi - ia * tl, oc = ia * uo - xr * zi,
               yu = xr * tl - fr * uo;
        double vm  = GROK_CR[0] * uo + GROK_CR[1] * tl + GROK_CR[2] * zi;
        double hme = GROK_CR[3] * uo + GROK_CR[4] * tl + GROK_CR[5] * zi;
        double nre = GROK_CR[0] * md + GROK_CR[1] * oc + GROK_CR[2] * yu;
        double ere = GROK_CR[3] * md + GROK_CR[4] * oc + GROK_CR[5] * yu;
        double cre = md, ha = -oc, ci = uo, ys = -tl;
        double ku = cre * ys - ci * ha;
        if (fabs(ku) < 1e-6) ku = 1e-6;
        double ql = ys / ku, gee = -ci / ku, bm = -ha / ku, ire = cre / ku;
        double km = nre * ql + vm * bm;
        double fee = nre * gee + vm * ire;
        double ree = -ere * ql + -hme * bm;
        double zee = -ere * gee + -hme * ire;
        double c1 = sqrt(km * km + ree * ree);
        if (c1 < 0.02) c1 = 0.02;
        double vre = sqrt(fee * fee + zee * zee);
        if (vre < 0.02) vre = 0.02;
        double tre = dstep(io / 0.5);
        double tnow = (double)now;
        double kj = sin(tnow * 42e-5 + i) * 1.4
                  + sin(tnow * 0.001 + i * 2) * 0.5 + GF.s[G_GZX].x;
        double ko = sin(tnow * 58e-5 + i) * 0.9 + GF.s[G_GZY].x;
        double vee = dclamp(c1 * hee * pulse, 0.02, 2.4);
        double ysc = dclamp(vre * lid * u1 * pulse, 0.02, 2.4);
        double ume = GROK_G9E * ysc + 2;
        double vl = dclamp(sre + ko * FT_H, GROK_TOP + ume, GROK_BOTTOM - ume);
        double o2 = -1e18, xl = 1e18;
        for (int p = 0; p < GROK_VERT_N; p += 2) {
            double frp = (polys[i][p][0] - gn) * vee;
            double sl, sr;
            gf_span_at(vl + (polys[i][p][1] - ti) * ysc, &sl, &sr);
            if (sl - frp > o2) o2 = sl - frp;
            if (sr + frp < xl) xl = sr + frp;
        }
        double xre = GROK_RE + wo + kj * FT_GAP;
        double lx = o2 <= xl ? dclamp(xre, o2, xl) : (o2 + xl) / 2;
        double dd = lx + (xre - lx) * (1 - tre);
        double frm = dclamp(hee * pulse, 0.02, 2.4);
        double iam = dclamp(lid * u1 * pulse, 0.02, 2.4);
        double ma = km * frm, mb = ree * frm, mc = fee * iam, md2 = zee * iam;
        if (io > 0.02) {
            for (int j = 0; j < GROK_VERT_N; j++) {
                double ex = polys[i][j][0] - gn, ey = polys[i][j][1] - ti;
                double vx = ma * ex + mc * ey + dd;
                double vy = mb * ex + md2 * ey + vl;
                gf_view_to_ss(vx, vy, &eye_ss[j][0], &eye_ss[j][1]);
            }
            gf_cov_fill(eye_ss, GROK_VERT_N, 0);   /* 底色镂空眼睛 */
        }
    }

    /* 降采样：coverage → 橙色渐变墨水（顶 0xFF9E2C→底 ~0.76 比率），AA 混合；
     * 背景与页面 COL_BG(0x101418) 同色，画布融入页面无框感 */
    uint32_t *dst = expr_buf;
    for (int y = 0; y < EXPR_H; y++) {
        double gt = (double)y / (EXPR_H - 1);
        uint32_t ir = (uint32_t)(255 + (194 - 255) * gt + 0.5);
        uint32_t ig = (uint32_t)(158 + (120 - 158) * gt + 0.5);
        uint32_t ib = (uint32_t)(44 + (33 - 44) * gt + 0.5);
        uint32_t ink = 0xFF000000u | (ir << 16) | (ig << 8) | ib;
        for (int x = 0; x < EXPR_W; x++) {
            const uint8_t *c = GF.cov + (size_t)(y * SS) * COV_W + x * SS;
            int cov = c[0] + c[1] + c[COV_W] + c[COV_W + 1];
            if (cov <= 0) {
                *dst++ = 0xFF101418u;
            } else if (cov >= 1020) {
                *dst++ = ink;
            } else {
                double al = (double)cov / 1020.0;
                uint32_t r = (uint32_t)(0x10 + (ir - 0x10) * al);
                uint32_t gr = (uint32_t)(0x14 + (ig - 0x14) * al);
                uint32_t b = (uint32_t)(0x18 + (ib - 0x18) * al);
                *dst++ = 0xFF000000u | (r << 16) | (gr << 8) | b;
            }
        }
    }
}

/* ================= FX 彩效层 v3（demo 按钮级还原：转一圈/粒子） =================
 * 对应 replica demo 按钮：act-spin→spinOnce()（spinTurn 弹簧 freq6.2/damp1
 * 目标±2π，身体整圈旋转）；act-burst→burst(22,1.1,0.3)（金星星芒/圆点/
 * 速度拉伸条，6 色调色板，170-360 速、0.45-0.85s 短寿）。
 * 转一圈=纯整圈旋转（彩带两度加入都被评"不好看"，永久删除，
 * 勿再恢复）；唤醒=转一圈、回复起播=粒子爆发。
 * overlay：think 彩虹弧已按用户要求删除；listen 呼吸环保留。 */
#define FX_N 96

static const uint32_t FX_PALETTE[6] = {   /* fx.js PALETTE */
    0xf9705c, 0x5b95f0, 0x3fbe86, 0xf5b13f, 0x9a72ee, 0x35c3bd
};
#define FX_STAR_R 0xf4
#define FX_STAR_G 0xc3
#define FX_STAR_B 0x4e

static struct {
    uint8_t kind, used;
    float x, y, vx, vy;
    float life, max_life;
    float sz;
    uint8_t cr, cg, cb;
    uint8_t star, ring;
} FXP[FX_N];

static double fx_lit = 0.9;
static uint64_t fx_ring_last;
static uint32_t fx_rnd = 0x9e3779b9u;
static double frand(double a, double b)
{
    fx_rnd = fx_rnd * 1664525u + 1013904223u;
    return a + (double)(fx_rnd >> 8) / 16777216.0 * (b - a);
}

static void fx_trick_spin(void)
{
    if (FX_ST_on) return;
    FX_ST.x = 0;
    FX_ST.v = 0;
    FX_ST.t = M_PI * 2 * (frand(0, 1) < 0.5 ? -1 : 1);
    FX_ST_on = 1;
}

static void fx_v2c(double vx, double vy, double *ox, double *oy)
{
    *ox = (vx - GROK_RE) * VIEW_K + EXPR_W * 0.5;
    *oy = (vy - GROK_RE) * VIEW_K + EXPR_H * 0.5;
}

static void hsl_rgb(double h, double s, double l,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    double c = (1 - fabs(2 * l - 1)) * s;
    double hp = fmod(h < 0 ? h + 360 : h, 360.0) / 60.0;
    double x = c * (1 - fabs(fmod(hp, 2.0) - 1));
    double r1 = 0, g1 = 0, b1 = 0;
    if (hp < 1)      { r1 = c; g1 = x; }
    else if (hp < 2) { r1 = x; g1 = c; }
    else if (hp < 3) { g1 = c; b1 = x; }
    else if (hp < 4) { g1 = x; b1 = c; }
    else if (hp < 5) { r1 = x; b1 = c; }
    else             { r1 = c; b1 = x; }
    double m = l - c / 2;
    *r = (uint8_t)dclamp((r1 + m) * 255, 0, 255);
    *g = (uint8_t)dclamp((g1 + m) * 255, 0, 255);
    *b = (uint8_t)dclamp((b1 + m) * 255, 0, 255);
}

/* 软圆点：平方衰减 + 半加色。occlude=1 时脸覆盖区跳过（转到脸后）。 */
static void fx_dot(double x, double y, int r, uint8_t cr, uint8_t cg,
                   uint8_t cb, double a, int occlude)
{
    if (a <= 0.004 || r < 1) return;
    int x0 = (int)x - r, x1 = (int)x + r + 1;
    int y0 = (int)y - r, y1 = (int)y + r + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EXPR_W - 1) x1 = EXPR_W - 1;
    if (y1 > EXPR_H - 1) y1 = EXPR_H - 1;
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            double dx = px - x, dy = py - y;
            double d2 = (dx * dx + dy * dy) / (r * r);
            if (d2 > 1) continue;
            if (occlude) {
                const uint8_t *c = GF.cov + (size_t)(py * SS) * COV_W
                                 + (size_t)px * SS;
                if (c[0] + c[1] + c[COV_W] + c[COV_W + 1] > 200) continue;
            }
            double f = a * (1 - d2) * (1 - d2);
            uint32_t *p = expr_buf + (size_t)py * EXPR_W + px;
            uint32_t dst = *p;
            uint32_t br = (dst >> 16) & 0xFF, bg = (dst >> 8) & 0xFF,
                     bb = dst & 0xFF;
            double nr = br + (cr - br) * f + (double)cr * f * 0.35;
            double ng = bg + (cg - bg) * f + (double)cg * f * 0.35;
            double nb = bb + (cb - bb) * f + (double)cb * f * 0.35;
            *p = 0xFF000000u
               | ((uint32_t)dclamp(nr, 0, 255) << 16)
               | ((uint32_t)dclamp(ng, 0, 255) << 8)
               | (uint32_t)dclamp(nb, 0, 255);
        }
    }
}

/* 速度条：沿速度反向的胶囊（fx.js rect 拉伸，长 ∝ 速度） */
static void fx_streak(double x, double y, double vx, double vy, double w,
                      uint8_t cr, uint8_t cg, uint8_t cb, double a)
{
    double sp = sqrt(vx * vx + vy * vy);
    if (sp < 1) { fx_dot(x, y, (int)(w + 0.5), cr, cg, cb, a, 0); return; }
    double len = dclamp(sp * 0.06, 3, 26);
    double ux = vx / sp, uy = vy / sp;
    int steps = (int)len + 1;
    for (int s = 0; s <= steps; s++) {
        double t = (double)s / steps;
        fx_dot(x - ux * len * t, y - uy * len * t,
               (int)(w * (1 - 0.35 * t) + 0.5), cr, cg, cb, a, 0);
    }
}

/* 金色星芒（fx.js STAR：5 角星，小尺寸下用交叉光条+亮心等价） */
static void fx_star(double x, double y, double rot, double sz, double a)
{
    double c = cos(rot), s = sin(rot);
    for (int k = 0; k < 2; k++) {
        double rr = k ? M_PI / 4 : 0;
        double dx = cos(rr), dy = sin(rr);
        double ex = dx * c - dy * s, ey = dx * s + dy * c;
        int steps = (int)sz + 1;
        for (int i = -steps; i <= steps; i++) {
            double t = (double)i / steps;
            fx_dot(x + ex * sz * t, y + ey * sz * t,
                   (int)(sz * (0.55 + 0.45 * (1 - fabs(t))) + 0.5),
                   FX_STAR_R, FX_STAR_G, FX_STAR_B, a, 0);
        }
    }
    fx_dot(x, y, (int)(sz * 0.8 + 0.5), 0xFF, 0xE7, 0x9E, a, 0);
}

static int fx_spawn(void)
{
    for (int i = 0; i < FX_N; i++)
        if (!FXP[i].used) {
            memset(&FXP[i], 0, sizeof(FXP[i]));
            FXP[i].used = 1;
            return i;
        }
    return -1;
}

/* 彩纸爆发（fx.js burst(W,H,G)，demo 粒子按钮 = burst(22,1.1,0.3)） */
static void fx_burst(int w, double h, double g)
{
    for (int i = 0; i < w; i++) {
        int k = fx_spawn();
        if (k < 0) return;
        double a = frand(0, M_PI * 2) + frand(-0.35, 0.35);
        double rad = frand(96, 116);
        double px, py;
        fx_v2c(GROK_RE + cos(a) * rad, GROK_RE + sin(a) * rad, &px, &py);
        FXP[k].x = (float)px;
        FXP[k].y = (float)py;
        double te = frand(170, 360) * h;
        double z = g * te * 0.2;
        double vx = cos(a) * te - sin(a) * z;
        double vy = sin(a) * te + cos(a) * z - frand(20, 75);
        FXP[k].vx = (float)(vx * VIEW_K);
        FXP[k].vy = (float)(vy * VIEW_K);
        FXP[k].max_life = (float)frand(0.45, 0.85);
        FXP[k].life = FXP[k].max_life;
        FXP[k].star = frand(0, 1) < 0.18;
        if (FXP[k].star) {
            FXP[k].sz = (float)frand(4, 6.5);
        } else {
            FXP[k].sz = (float)frand(3.5, 8);
            uint32_t col = FX_PALETTE[(int)frand(0, 6) % 6];
            FXP[k].cr = (col >> 16) & 0xFF;
            FXP[k].cg = (col >> 8) & 0xFF;
            FXP[k].cb = col & 0xFF;
        }
    }
}

/* 待机卡通音效（Kenney CC0，/usr/share/robot-ui/sfx/*.wav）：
 * fork+execlp aplay 非阻塞播放，不卡 LVGL 定时器；文件缺失静默跳过。
 * 音量已压制（喇叭功放偏响），且只在 idle 动作节拍/唤醒/起播触发。
 * 相邻两次不播同一个（用户要求）：同名命中时随机换一个。 */
static const char *sfx_pool[] = { "blip", "swoosh", "sparkle", "boing",
                                  "meow", "bark", "spring" };
static char sfx_last[16];
static void sfx_play(const char *name)
{
    if (strcmp(name, sfx_last) == 0) {
        const char *alt;
        do { alt = sfx_pool[rand() % 7]; } while (strcmp(alt, name) == 0);
        name = alt;
    }
    snprintf(sfx_last, sizeof sfx_last, "%s", name);
    char path[128];
    snprintf(path, sizeof path, "/usr/share/robot-ui/sfx/%s.wav", name);
    if (access(path, R_OK) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        execlp("aplay", "aplay", "-q", path, (char *)NULL);
        _exit(127);
    }
}

static void fx_on_mood(int mood)
{
    /* 唤醒 = 转一圈打招呼（彩带由快转自动触发）；播报 = 粒子爆发 */
    if (mood == GM_WAKE) { fx_trick_spin(); sfx_play("boing"); }
    else if (mood == GM_HAPPY) { fx_burst(22, 1.1, 0.3); sfx_play("sparkle"); }
}

/* 每帧：模拟 + 叠加绘制（在 gf_render 之后调用，cov 仍在） */
static void fx_update(double mt, double dt, int mood)
{
    /* listen：呼吸扩散环（radar 式，每 1.4s） */
    if (mood == GM_LISTEN && mt * 1000.0 > fx_ring_last + 1400) {
        fx_ring_last = (uint64_t)(mt * 1000.0);
        int k = fx_spawn();
        if (k >= 0) {
            FXP[k].ring = 1;
            FXP[k].max_life = 1.2f;
            FXP[k].life = 1.2f;
        }
    }

    /* 彩纸/星芒/扩散环（fx.js step：重力 40、阻尼 0.94^60dt、1.7 次幂淡出） */
    double damp = pow(0.94, dt * 60.0);
    for (int i = 0; i < FX_N; i++) {
        if (!FXP[i].used) continue;
        FXP[i].life -= (float)dt;
        if (FXP[i].life <= 0) { FXP[i].used = 0; continue; }
        double lt = 1.0 - FXP[i].life / FXP[i].max_life;
        double alpha = lt < 0.1 ? lt / 0.1 : pow(1 - (lt - 0.1) / 0.9, 1.7);

        if (FXP[i].ring) {
            double rr = 26 + lt * (fmin(GROK_RE * VIEW_K + 16.0,
                                        EXPR_W * 0.5 - 6.0) - 26);
            uint8_t r, g, b;
            hsl_rgb(185, 0.85, 0.6, &r, &g, &b);
            double cx = EXPR_W * 0.5, cy = EXPR_H * 0.5;
            int seg = (int)(rr * 0.35);
            if (seg < 20) seg = 20;
            if (seg > 60) seg = 60;
            for (int s = 0; s < seg; s++) {
                double a = M_PI * 2 * s / seg;
                fx_dot(cx + cos(a) * rr, cy + sin(a) * rr * 0.94,
                       1, r, g, b, 0.3 * (1 - lt), 0);
            }
            continue;
        }

        FXP[i].vy = FXP[i].vy * (float)damp + 40.0f * (float)dt * VIEW_K;
        FXP[i].vx *= (float)damp;
        FXP[i].x += FXP[i].vx * (float)dt;
        FXP[i].y += FXP[i].vy * (float)dt;
        double sz = FXP[i].sz * (1 - lt * 0.4);
        if (FXP[i].star)
            fx_star(FXP[i].x, FXP[i].y, mt * 4.0 + i, sz * 0.9, alpha);
        else
            fx_streak(FXP[i].x, FXP[i].y, FXP[i].vx, FXP[i].vy, sz,
                      FXP[i].cr, FXP[i].cg, FXP[i].cb, alpha);
    }
}

static void expr_timer_cb(lv_timer_t *tm)
{
    (void)tm;
    if (lv_screen_active() != scr_voice || !GF.t0) return;
    uint64_t now = now_ms();
    double dt = dclamp((double)(now - GF.last_tick) / 1000.0, 0, 0.1);
    GF.last_tick = now;
    double mt = (double)(now - GF.t0) / 1000.0;
    double dts = (double)(now - GF.state_at) / 1000.0;

    double spin, txv, tyv, squash, lid, boost;
    gf_pose(mt, dts, now, &spin, &txv, &tyv, &squash, &lid, &boost);
    GF.s[G_SPIN].t = spin;
    GF.s[G_TX].t = txv;
    GF.s[G_TY].t = tyv;
    GF.s[G_SQUASH].t = squash;
    GF.s[G_ESCL].t = boost;
    GF.pose_lid = lid;

    if (GF.wake_blink && !GF.waking_blinked && GF.bq_n == 0) {
        gf_blink_queue(now);
        GF.waking_blinked = 1;
    }
    GF.wake_blink = 0;

    /* 眼形播放列表推进（waking 由 pose 时间线驱动） */
    if (GF.mood != GM_WAKE && now >= GF.eye_until) {
        int n = GM_PLAY_N[GF.mood];
        GF.eye_idx = (GF.eye_idx + 1 + (int)drand(0, n - 1)) % n;
        gf_morph(GM_PLAY[GF.mood][GF.eye_idx],
                 GF.mood == GM_IDLE ? 9 : 6);
        if (GF.mood == GM_IDLE && !FX_ST_on) {
            /* 待机动作轮播：偶尔转一圈（带彩带爆发）或撒小把彩纸 */
            double roll = drand(0, 1);
            if (roll < 0.16) { fx_trick_spin(); sfx_play("swoosh"); }
            else if (roll < 0.34) { fx_burst(14, 1.0, 0.3); sfx_play("sparkle"); }
            else if (roll < 0.58) sfx_play("blip");   /* 换形轻声点缀 */
            /* 卡通萌声（Mixkit 免费许可）：低频彩蛋，别太吵 */
            else if (roll < 0.66) sfx_play("meow");
            else if (roll < 0.73) sfx_play("bark");
            else if (roll < 0.80) sfx_play("spring");
        }
        GF.eye_until = now + (uint64_t)drand(GM_HOLD[GF.mood][0],
                                             GM_HOLD[GF.mood][1]);
    }

    /* 眨眼节奏 + 队列消费 */
    if (GM_BLINK[GF.mood][0] && now >= GF.blink_until) {
        gf_blink_queue(now);
        GF.blink_until = now + (uint64_t)drand(GM_BLINK[GF.mood][0],
                                               GM_BLINK[GF.mood][1]);
    }
    int popped = 0;
    double bkey = 1;
    while (GF.bq_n && now >= GF.bq[0].at) {
        bkey = GF.bq[0].v;
        memmove(GF.bq, GF.bq + 1, sizeof(GF.bq[0]) * (size_t)(GF.bq_n - 1));
        GF.bq_n--;
        popped = 1;
    }
    if (popped) GF.s[G_BLINK].t = bkey;
    else if (GF.bq_n == 0) GF.s[G_BLINK].t = GF.pose_lid;

    /* 视线游走 */
    if (now >= GF.gaze_until) {
        double gx, gy, hold;
        gf_next_gaze(&gx, &gy, &hold);
        GF.s[G_GZX].t = gx;
        GF.s[G_GZY].t = gy;
        GF.gaze_until = now + (uint64_t)hold;
    }

    /* 弹簧子步进（1/120 固定步长） */
    int nsteps = dt > 0 ? (int)(dt * 120.0) + 1 : 1;
    if (nsteps > 12) nsteps = 12;
    double step = dt / nsteps;
    for (int k = 0; k < nsteps; k++) {
        gs_step(&GF.morph, GF.stiff, 1, step);
        for (int e = 0; e < 8; e++)
            gs_step(&GF.s[e], GM_SPR[e][0], GM_SPR[e][1], step);
        if (FX_ST_on)
            gs_step(&FX_ST, 6.2, 1, step);   /* tables.js spinTurn [6.2,1] */
    }
    if (FX_ST_on && fabs(FX_ST.t - FX_ST.x) < 0.004
        && fabs(FX_ST.v) < 0.015)
        FX_ST_on = 0;                         /* spinTurnSettled */

    gf_render(now);
    fx_update(mt, dt, GF.mood);
    lv_obj_invalidate(expr_canvas);
}

/* 语音链由语音页进出控制：进页 start、退回首页 stop（用户要求 UI 门控） */
static void voice_engine(int on)
{
    /* restart 而非 start：服务已在跑（如 UI 重启没走 home stop）时，
     * start 是空操作、va 不会再发 state idle，徽章会卡"启动中" */
    system(on ? "systemctl restart robot-voice --no-block"
              : "systemctl stop robot-voice --no-block");
}

static const int ST_TO_GM[5] = {
    GM_IDLE, GM_WAKE, GM_LISTEN, GM_THINK, GM_HAPPY
};

static void voice_set_state(int s)
{
    gf_set_mood(ST_TO_GM[s], 0);
    lv_label_set_text(st_badge_lb, st_names[s]);
    lv_obj_set_style_bg_color(st_badge, COL_ACCENT, 0);
    lv_obj_set_style_text_color(st_badge_lb, lv_color_hex(0xffffff), 0);
    if (s == ST_THINK || s == ST_SPEAK)
        lv_obj_clear_flag(st_spinner, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(st_spinner, LV_OBJ_FLAG_HIDDEN);
}

static void voice_add_msg(const char *text, int from_user)
{
    lv_obj_t *bub = lv_obj_create(dlg_cont);
    lv_obj_set_width(bub, LV_PCT(86));
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bub, from_user ? COL_USER : COL_ROBOT, 0);
    lv_obj_set_style_radius(bub, 14, 0);
    lv_obj_set_style_pad_all(bub, 10, 0);
    lv_obj_set_style_border_width(bub, 0, 0);
    lv_obj_set_style_bg_opa(bub, from_user ? LV_OPA_40 : LV_OPA_100, 0);
    lv_obj_t *lb = label_cn(bub, text);
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lb, LV_PCT(100));
    lv_obj_set_style_text_color(lb, from_user ? lv_color_hex(0xbfe0ff) : COL_TEXT, 0);
    if (from_user) lv_obj_align(bub, LV_ALIGN_RIGHT_MID, 0, 0);
    else lv_obj_align(bub, LV_ALIGN_LEFT_MID, 0, 0);

    /* cap the log: drop the two oldest bubbles */
    uint32_t n = lv_obj_get_child_count(dlg_cont);
    if (n > 30) {
        lv_obj_delete(lv_obj_get_child(dlg_cont, 0));
        lv_obj_delete(lv_obj_get_child(dlg_cont, 0));
    }
    lv_obj_scroll_to_y(dlg_cont, LV_COORD_MAX, LV_ANIM_OFF);
    voice_set_state(from_user ? ST_THINK : ST_SPEAK);
}

static void voice_reset(void);

static void voice_build(void)
{
    scr_voice = lv_obj_create(NULL);
    style_base(scr_voice, NULL);
    /* 无标题栏：返回按钮移最右，状态徽章放文字栏上方偏左 */
    lv_obj_t *back = lv_button_create(scr_voice);
    lv_obj_set_size(back, 96, 56);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_set_style_bg_color(back, COL_CARD, 0);
    lv_obj_set_style_radius(back, 14, 0);
    lv_obj_set_style_border_color(back, COL_DIM, 0);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_to_home, LV_EVENT_CLICKED, NULL);

    /* 状态徽章：文字栏上方偏左，只显示当前状态 */
    st_badge = lv_obj_create(scr_voice);
    lv_obj_set_size(st_badge, 150, 48);
    lv_obj_set_pos(st_badge, 424, 16);
    lv_obj_set_style_radius(st_badge, 14, 0);
    lv_obj_set_style_bg_color(st_badge, COL_CARD, 0);
    lv_obj_set_style_border_width(st_badge, 0, 0);
    lv_obj_clear_flag(st_badge, LV_OBJ_FLAG_SCROLLABLE);
    st_badge_lb = label_cn(st_badge, "待机");
    lv_obj_align(st_badge_lb, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(st_badge_lb, COL_DIM, 0);
    /* spinner 在徽章右侧独立显示：think/speak 才出现，徽章无空白 */
    st_spinner = lv_spinner_create(scr_voice);
    lv_obj_set_size(st_spinner, 36, 36);
    lv_obj_set_pos(st_spinner, 584, 22);
    lv_obj_set_style_arc_color(st_spinner, lv_color_hex(0xffffff),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(st_spinner, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_arc_width(st_spinner, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(st_spinner, 7, LV_PART_MAIN);
    /* 270° 长弧 800ms 快转（默认 360° 整圈旋转视觉上不明显） */
    lv_spinner_set_anim_params(st_spinner, 800, 270);
    lv_obj_add_flag(st_spinner, LV_OBJ_FLAG_HIDDEN);

    /* 当前眼形名（画布上方居中，gf_morph 刷新） */
    expr_name_lb = label_cn(scr_voice, "平静");
    lv_obj_set_width(expr_name_lb, 400);
    lv_obj_set_pos(expr_name_lb, 10, 34);
    lv_obj_set_style_text_align(expr_name_lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(expr_name_lb, COL_DIM, 0);

    /* expression engine（左列：弹簧 blob 表情画布） */
    expr_canvas = lv_canvas_create(scr_voice);
    lv_canvas_set_buffer(expr_canvas, expr_buf, EXPR_W, EXPR_H,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(expr_canvas, 10, 64);   /* 无边框：与页面底色一致 */
    GF.t0 = now_ms();
    GF.last_tick = GF.t0;
    GF.mood = -1;
    GF.s[G_SQUASH].x = 1;
    GF.s[G_BLINK].x = 1;
    GF.s[G_ESCL].x = 1;
    gf_span_init();
    gf_set_mood(GM_IDLE, 1);
    lv_timer_create(expr_timer_cb, 33, NULL);

    /* dialogue log (right column) */
    dlg_cont = lv_obj_create(scr_voice);
    lv_obj_set_size(dlg_cont, 364, 388);
    lv_obj_set_pos(dlg_cont, 424, 78);
    lv_obj_set_style_bg_color(dlg_cont, lv_color_hex(0x151a20), 0);
    lv_obj_set_style_border_width(dlg_cont, 0, 0);
    lv_obj_set_style_radius(dlg_cont, 16, 0);
    lv_obj_set_flex_flow(dlg_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(dlg_cont, 10, 0);
    lv_obj_set_style_pad_row(dlg_cont, 10, 0);

    voice_reset();   /* 徽章先显"启动中"，首个 fifo 事件即切实际状态 */
}

static void voice_reset(void)
{
    /* 徽章回"启动中"暗态 + spinner 隐藏（等待 va 首个事件） */
    gf_set_mood(GM_IDLE, 0);
    lv_label_set_text(st_badge_lb, "启动中");
    lv_obj_set_style_bg_color(st_badge, COL_CARD, 0);
    lv_obj_set_style_text_color(st_badge_lb, COL_DIM, 0);
    lv_obj_add_flag(st_spinner, LV_OBJ_FLAG_HIDDEN);
}

static void goto_voice(lv_event_t *e)
{
    (void)e;
    voice_reset();
    voice_engine(1);
    lv_screen_load_anim(scr_voice, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

/* ============ camera page ============ */
static lv_obj_t *cam_label;
static int cam_fd = -1;

static void cam_probe(void)
{
    const char *msg = "摄像头未就绪";
    do {
        cam_fd = open("/dev/video0", O_RDWR);
        if (cam_fd < 0) { msg = "无法打开 /dev/video0"; break; }
        struct v4l2_capability cap;
        if (ioctl(cam_fd, VIDIOC_QUERYCAP, &cap) < 0) { msg = "QUERYCAP 失败"; break; }
        static char info[128];
        struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        ioctl(cam_fd, VIDIOC_G_FMT, &fmt);
        snprintf(info, sizeof(info), "%s\n当前格式: %ux%u %s\n(预览功能开发中)",
                 (const char *)cap.card, fmt.fmt.pix.width, fmt.fmt.pix.height,
                 fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12 ? "NV12" : "RAW");
        msg = info;
        close(cam_fd); cam_fd = -1;
    } while (0);
    lv_label_set_text(cam_label, msg);
}

static void cam_build(void)
{
    scr_cam = lv_obj_create(NULL);
    style_base(scr_cam, NULL);
    topbar_build(scr_cam, "摄像头");
    lv_obj_t *box = lv_obj_create(scr_cam);
    lv_obj_set_size(box, 740, 330);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0b0e12), 0);
    lv_obj_set_style_border_color(box, COL_ACCENT2, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    cam_label = label_cn(box, "检测中…");
    lv_obj_center(cam_label);
}

static void goto_cam(lv_event_t *e)
{
    (void)e;
    lv_screen_load_anim(scr_cam, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
    cam_probe();
}

/* ---------------- UI fifo (va -> robot-ui) ---------------- */
static int fifo_fd = -1;

static void fifo_init(void)
{
    unlink(UI_FIFO);
    mkfifo(UI_FIFO, 0666);
    fifo_fd = open(UI_FIFO, O_RDONLY | O_NONBLOCK);
    if (fifo_fd < 0) perror("robot-ui: fifo");
    /* keep a dummy writer so read() doesn't hit EOF repeatedly */
    int w = open(UI_FIFO, O_WRONLY | O_NONBLOCK);
    (void)w;
}

static void fifo_poll(void)
{
    static char buf[1024];
    static size_t len = 0;
    if (pending_release && now_ms() - press_ms >= 40) {
        touch.pressed = 0;
        pending_release = 0;
    }
    for (;;) {
        ssize_t n = read(fifo_fd, buf + len, sizeof(buf) - len - 1);
        if (n <= 0) break;
        len += n;
        buf[len] = 0;
        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            if (!strncmp(line, "state ", 6)) {
                const char *s = line + 6;
                int st = ST_IDLE;
                if (!strcmp(s, "wake")) st = ST_WAKE;
                else if (!strcmp(s, "listen")) st = ST_LISTEN;
                else if (!strcmp(s, "think")) st = ST_THINK;
                else if (!strcmp(s, "speak")) st = ST_SPEAK;
                else st = ST_IDLE;
                voice_set_state(st);
            } else if (!strncmp(line, "fx ", 3)) {
                const char *s2 = line + 3;
                if (!strcmp(s2, "spin")) fx_trick_spin();
                else if (!strcmp(s2, "burst")) fx_burst(22, 1.1, 0.3);
            } else if (!strncmp(line, "page ", 5)) {
                /* 页面导航（自动化测试用；与磁贴点击等价） */
                const char *s = line + 5;
                if (!strcmp(s, "voice")) goto_voice(NULL);
                else if (!strcmp(s, "cam")) goto_cam(NULL);
                else if (!strcmp(s, "home")) back_to_home(NULL);
            } else if (!strncmp(line, "user ", 5)) {
                voice_add_msg(line + 5, 1);
            } else if (!strncmp(line, "robot ", 6)) {
                voice_add_msg(line + 6, 0);
            }
            line = nl + 1;
        }
        len = strlen(line);
        memmove(buf, line, len);
    }
}

/* ---------------- main ---------------- */
static void on_sigint(int s) { (void)s; exit(0); }

int main(int argc, char **argv)
{
    const char *fbdev = "/dev/fb0";
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--fb") && i + 1 < argc) fbdev = argv[++i];
    {
        const char *r = getenv("ROBOT_UI_ROT");
        if (r) {
            int v = atoi(r);
            if (v == 0 || v == 90 || v == 270) ROT = v;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGPIPE, SIG_IGN);

    fb_open(fbdev);
    if ((ROT == 90 || ROT == 270) && (FB.w != VRES || FB.h != HRES)) {
        fprintf(stderr, "robot-ui: rot %d needs %ux%u fb (got %ux%u)\n",
                ROT, VRES, HRES, FB.w, FB.h);
        exit(1);
    } else if (ROT == 0 && (FB.w != HRES || FB.h != VRES)) {
        fprintf(stderr, "robot-ui: rot 0 needs %ux%u fb (got %ux%u)\n",
                HRES, VRES, FB.w, FB.h);
        exit(1);
    }
    ts_init();
    fifo_init();
    signal(SIGCHLD, SIG_IGN);   /* sfx aplay 子进程免僵尸 */

    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(HRES, VRES);
    lv_display_set_flush_cb(disp, flush_cb);
    static uint8_t buf1[HRES * 60 * 4], buf2[HRES * 60 * 4];
    lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_indev_t *tp = lv_indev_create();
    lv_indev_set_type(tp, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(tp, touch_read_cb);

    home_build();
    voice_build();
    cam_build();
    lv_screen_load(scr_home);

    uint64_t last = 0;
    for (;;) {
        ts_poll();
        fifo_poll();
        lv_timer_handler();
        uint64_t now = now_ms();
        if (now - last >= 5) { last = now; usleep(5000); }
        else usleep(1000);
    }
    return 0;
}
