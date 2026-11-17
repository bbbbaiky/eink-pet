/* ══════════════════════════════════════════════════════════════════════════════
 * fb_core.h —— 帧缓冲 + 绘制原语（C1 第二步；2026-11-11 从 main.c 搬出）
 * ------------------------------------------------------------------------------
 * 灰度帧缓冲 `fb`（200×200 字节/像素，0=墨 / 255=白）与打包输出 `packed`
 * （200×200 1bpp、25B/行、bit=1→白）原来都定义在 main.c，绘制原语散在文件各处。
 * 拆成独立 TU 后，`fb[y][x]` 的写法、调用点**一行都没改**（只是定义换了文件）。
 *
 * 本模块只放**不依赖任何业务状态**的原语：
 *   · fb_clear_white / fb_fill_white / fb_put / fb_wput / fb_round_rect / fb_invert_rect
 *   · fb_blit1（整块 1-bit 位图）
 *   · fb_pack_core（打包，**不盖底部提示带**）/ fb_from_packed（打包 → 灰度反解）
 * 文字绘制与 assets 装载仍在 main.c（它们依赖字库/区域状态），后续再拆。
 *
 * ⚠ fb_alloc() 仍留在 main.c（用 psram_u8 分 PSRAM，且只有开机一处调用）。
 * ════════════════════════════════════════════════════════════════════════════════ */
#ifndef FB_CORE_H
#define FB_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "main_api.h"        /* EPD_W / EPD_H */

#define EPD_LINE_BYTES  (EPD_W / 8)              /* 25 */
#define EPD_BUF_BYTES   (EPD_LINE_BYTES * EPD_H) /* 5000 */

/* PSRAM 灰度帧缓冲（开机 fb_alloc() 分配；失败为 NULL，调用方必须已判过）*/
extern uint8_t (*fb)[EPD_W];

/* 推屏用的打包缓冲（与动画帧、assets 位图**逐字节同构**）*/
extern uint8_t packed[EPD_BUF_BYTES];

/* 取 1-bit 位图的某一位（行宽 row_bytes，MSB 在前）*/
static inline int bit_get(const uint8_t *bits, int row_bytes, int y, int x)
{
    return (bits[y * row_bytes + (x >> 3)] >> (7 - (x & 7))) & 1;
}

void fb_clear_white(void);
void fb_fill_white(int l, int t, int r, int b);
void fb_put(int y, int x);                 /* 墨点 */
void fb_wput(int y, int x);                /* 白点（fb_put 的对偶）*/
void fb_round_rect(int l, int t, int r, int b);
void fb_invert_rect(int l, int t, int r, int b);
void fb_blit1(int l, int t, int w, int h, int row_bytes,
              const uint8_t *src, bool opaque);
const uint8_t *fb_pack_core(void);
void fb_from_packed(const uint8_t *src);

#endif /* FB_CORE_H */
