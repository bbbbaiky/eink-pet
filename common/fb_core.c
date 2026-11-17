/* fb_core.c —— 帧缓冲与绘制原语实现
 * 2026-11-11（C1 第二步）：从 main.c **逐字搬出**，行为与调用约定不变。
 * 只搬「不依赖业务状态」的部分；文字/assets 装载仍在 main.c。 */
#include <string.h>

#include "fb_core.h"

/* 字节每像素的灰度帧缓冲（0=黑 255=白，40KB）。
   ★★2026-10-23 RAM 优化：40KB 帧缓冲**搬 PSRAM**（内部 DRAM 余量 +40KB），
     分配见 main.c::fb_alloc()；`fb[y][x]` 的用法一行不用改（二维数组指针）。 */
uint8_t (*fb)[EPD_W] = NULL;               /* PSRAM，开机 fb_alloc() */

/* 打包输出（5KB）。与动画帧 / assets 位图逐字节同构：bit=1 → 白。 */
uint8_t packed[EPD_BUF_BYTES];

void fb_clear_white(void)
{
    for (int y = 0; y < EPD_H; ++y) memset(fb[y], 255, EPD_W);
}

/* 白色实心矩形（抹掉 ui 位图上的局部内容用） */
void fb_fill_white(int l, int t, int r, int b)
{
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r >= EPD_W) r = EPD_W - 1;
    if (b >= EPD_H) b = EPD_H - 1;
    for (int y = t; y <= b; ++y) memset(fb[y] + l, 255, (size_t)(r - l + 1));
}

/* 画一个墨点（越界自动丢）。 */
void fb_put(int y, int x)
{
    if (y >= 0 && y < EPD_H && x >= 0 && x < EPD_W) fb[y][x] = 0;
}

/* 画一个白点（越界自动丢）—— fb_put 的对偶（fb 极性：0 = 墨 / 1 = 白）。
   ★★ 2026-10-17：成就「已达成」圆圈的勾原来用 fb_put 画 —— 黑勾画在黑圆上，
     用户实测「已完成状态的圆圈里面没有勾」。勾必须走本函数（白）。 */
void fb_wput(int y, int x)
{
    if (y >= 0 && y < EPD_H && x >= 0 && x < EPD_W) fb[y][x] = 1;
}

/* ★★ 1px **圆角**矩形（选中框）。
   圆角 = **2px 45° 斜角** —— 与设计稿逐像素一致：功能页框 (8,30)-(65,96) 的角上是
   (10,30) → (9,31) → (8,32)，WIFI 管理页框同样 2px（设置页稿上是 4px，按用户要求
   「跟功能页的选中一样」统一取 **2px**）。 */
#define FB_RRECT_R 2
void fb_round_rect(int l, int t, int r, int b)
{
    const int rr = FB_RRECT_R;
    for (int x = l + rr; x <= r - rr; ++x) { fb_put(t, x); fb_put(b, x); }
    for (int y = t + rr; y <= b - rr; ++y) { fb_put(y, l); fb_put(y, r); }
    for (int i = 0; i <= rr; ++i) {
        fb_put(t + rr - i, l + i);      /* 左上角斜线 */
        fb_put(t + rr - i, r - i);      /* 右上角 */
        fb_put(b - rr + i, l + i);      /* 左下角 */
        fb_put(b - rr + i, r - i);      /* 右下角 */
    }
}

/* ★★ 2026-10-08 新增：矩形**反白**（`l..r / t..b` 闭区间逐点取反）。
   用途 = 关于页底部两颗按钮的「选中」态 —— 与「添加网络确认」页同款视觉效果
   （那一页是**两张底图**各烤了一个反白按钮，固件这边没有那种能力，所以补这个原语）。
   用法（顺序很重要）：**先把文字画成墨点，再对整块反白** ⇒ 得到「墨底白字」
   （先反白再画字 = 字与底同色，看不见）。 */
void fb_invert_rect(int l, int t, int r, int b)
{
    for (int y = t; y <= b; ++y)
        for (int x = l; x <= r; ++x)
            if (y >= 0 && y < EPD_H && x >= 0 && x < EPD_W)
                fb[y][x] = fb[y][x] ? 0u : 255u;   /* 帧缓冲只有 0/255 两值，别用 ^=1 */
}

/* ★ 通用「贴一整块 1-bit 位图」（bit=1 → 墨，源行宽 row_bytes）。
   与 fb_apply_win 同一套约定，但位置/尺寸是参数 —— post.bin / icon.bin 都用它。
   opaque = true：白像素也盖住底图（照片是整块不透明的）；
   opaque = false：只落墨、背景透明（角标图标用，别把房间墙擦白一块）。 */
void fb_blit1(int l, int t, int w, int h, int row_bytes,
              const uint8_t *src, bool opaque)
{
    int x0 = (l < 0) ? 0 : l;
    int y0 = (t < 0) ? 0 : t;
    int x1 = (l + w > EPD_W) ? EPD_W : l + w;
    int y1 = (t + h > EPD_H) ? EPD_H : t + h;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            int ink = bit_get(src, row_bytes, y - t, x - l);
            if (ink)            fb[y][x] = 0;
            else if (opaque)    fb[y][x] = 255;
        }
    }
}

/* 打包核心（**不盖底部提示带**）：关机页要走它 —— 关机页上不该出现 PAGE_HINT。 */
const uint8_t *fb_pack_core(void)
{
    memset(packed, 0, sizeof(packed));
    for (int y = 0; y < EPD_H; ++y) {
        for (int x = 0; x < EPD_W; ++x) {
            if (fb[y][x]) {
                packed[y * EPD_LINE_BYTES + (x >> 3)] |= (0x80 >> (x & 7));
            }
        }
    }
    return packed;
}

/* ★R179：打包位图（25B/行，1=白）→ 灰度帧缓冲（255=白）。
   关机页从 assets 读出来的是**打包位图**，要先摊回 fb，才能用 fb_capsule_c 往上叠气泡。 */
void fb_from_packed(const uint8_t *src)
{
    for (int y = 0; y < EPD_H; ++y) {
        for (int x = 0; x < EPD_W; ++x) {
            const bool white = (src[y * EPD_LINE_BYTES + (x >> 3)] & (0x80 >> (x & 7))) != 0;
            fb[y][x] = white ? 255 : 0;
        }
    }
}
