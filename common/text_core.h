/* ══════════════════════════════════════════════════════════════════════════════
 * text_core.h —— 字库读取 + 文字绘制原语（C1 第三步；2026-11-11 从 main.c 搬出）
 * ------------------------------------------------------------------------------
 * 本模块只管「字」：UTF-8 解码、zfont.bin 的装载与查字、按宽度截断、把一行字
 * 落进帧缓冲（fb）。**不含**页面排版/业务文案（那些在 main.c）。
 *
 * 原样搬运，行为逐字等价：
 *   · utf8_next（非法字节按 '?' 吃 1 字节，永不越界/死循环）
 *   · zfont_idx_load（开机一次，把码点索引读进 PSRAM）
 *   · zf_glyph / zf_width / zf_ink_cols / zf_has_title_glyph
 *   · fb_text_c / fb_text_c_fit / fb_text_l / fb_text_l_fit / fb_text_r /
 *     fb_text_r_fit / fb_btn_text_fit
 *
 * 依赖：assets_rw（按需读字形）、fb_core（fb[]）、t_()（i18n，main_api.h）。
 * ⚠ fb_text_* 是 i18n 的**唯一入口层**（t_() 在这里接），调用点不要自己再包 T()。
 * ════════════════════════════════════════════════════════════════════════════════ */
#ifndef TEXT_CORE_H
#define TEXT_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "main_api.h"       /* EPD_W/H · ZF_FONT_* · ZF_BIT · t_() 等公共声明 */

/* UTF-8 → 码点。返回消耗字节数（0 = 串尾 / 空串；非法字节按 '?' 吃 1 字节）*/
int utf8_next(const char *s, uint32_t *cp);

/* 开机装载 zfont.bin 的布局与码点索引（失败 → 后续所有查字返回 NULL = 画豆腐）*/
bool zfont_idx_load(void);

/* 查一个字的「格」：返回位图（NULL = 缺字），并回填 行数 / 行字节数 / 前进宽度。
   ⚠ 返回指针指向**模块内静态缓冲**，下次调用即失效（调用方都是立即消费）。 */
const uint8_t *zf_glyph(int kind, uint32_t cp, int *h, int *rb, int *adv);

/* 本串墨迹的列范围 [c0,c1]（相对串首 cell 左缘；无墨返回 false）*/
bool zf_ink_cols(int kind, const char *utf8, int *c0, int *c1);

/* 本串「第一个非空表行」（fb_text_c / 邮编框对齐用；全空格返回 0）*/
int zf_first_ink_row(int kind, const char *utf8);

/* 右对齐 + 按宽度截断（列表行尾寄信人用；口径同 fb_text_l_fit）*/
int fb_text_r_fit(int kind, int xr, int y, const char *utf8, int max_w);

/* 按墨迹框**对称居中**在按钮框 [x0,x1]（与出图侧 _btn_round 同式）*/
int fb_btn_text_fit(int kind, int x0, int x1, int y, const char *utf8, int max_w);

#endif /* TEXT_CORE_H */
