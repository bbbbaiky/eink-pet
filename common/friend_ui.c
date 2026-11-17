/* ══════════════════════════════════════════════════════════════════════
 * friend_ui.c —— 朋友/串门三页的绘制（自 common/main.c 整段搬出）
 *
 * ★2026-10-26 重构（承接《重构方案与内存体检_20261025》第 2 步）：
 *   朋友域此前已经拆了两块：friend.c（好友码/好友表）、friend_net.c（跨设备链路）；
 *   portal 状态机 10-26 也搬进 friend.c；**剩下这一坨纯画笔**（好友列表页 / 二维码页 /
 *   串门场景页 / 串门图测试页）一直压在 main.c 里，现在一并搬出来。
 *   搬动口径：**代码零改动**，只把 4 个入口从 static 改成外部可见、串门状态改为本文件持有。
 * ══════════════════════════════════════════════════════════════════════ */
#include "variant.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

#include "main_api.h"
#include "friend.h"
#include "friend_net.h"
#include "friend_ui.h"
#include "i18n.h"

/* TAG 由 main.c 定义、main_api.h extern（全工程同一 TAG） */

/* ★★ R92（2026-10-27 用户令）：**4 行/页 → 3 行/页**（宏本体在 main_api.h，唯一来源）。
   原因：列表底部要加「添加朋友」整宽按钮（y164..191，与 WiFi 列表页的「+ 添加网络」
   同位同款），而原第 4 行 AROW_Y[3]=160 → 行选中框 150..181 会**正面压住按钮**。
   3 行（AROW_Y[0..2] = 52/88/124）+ 底部整宽按钮，正是 WiFi 列表页的口径。 */

/* 底部整宽「添加朋友」按钮（自绘本页没有 ui.bin 底图）：几何照抄 BOT_BTN_*
   （全站底部按钮 = x8..191 / y164..191），与 build_ui_touch.py 的 _btn_round 同源。 */
#define FR_ADD_BTN_X0    8
#define FR_ADD_BTN_Y0  164
#define FR_ADD_BTN_X1  191
#define FR_ADD_BTN_Y1  191

/* 行选中框：与 main.c 的 draw_row_sel() 逐像素同款（friend_ui.c 不能调 static，
   这里复制一份并注明同源 —— 那边改几何这里必须一起改）。 */
static void fr_row_sel(int y_top)
{
    fb_round_rect(4, y_top - 10, 195, y_top + 12 - 1 + 10);
}

/* 右上角页码「n/m」：与 main.c 的 page_draw_pageno 逐像素同款。
   friend_ui.c 调不到 main.c 的 static，这里复制一份（同 fr_row_sel 的先例）——
   **那边改几何这里必须一起改**。
   几何：右对齐 x191、墨迹顶 y13；**先擦白条 [152,8..199,27] 再写**（理由同那边：
   ① 骨架烤过静态页码占位，不擦=真值压占位叠字；② 换页后新串变短会留残影）；
   擦除带夹在 [x152,199]：右界给「10/12」5 字符 30px 留足，左界避开居中标题（≤8 字 x≈52..148）。
   ★★ R100（2026-10-27 用户拍板 C）：**删掉右上角「＋」，页码回到右上角** ——
   与 WiFi / 明信片 / 收藏品列表完全一致。沿革：2026-10-22 ＋ 占了右上 ⇒ 页码挪右下 y181；
   R92 底部加「添加朋友」整宽按钮 ⇒ 再被挤到 y146。用户原话「不应该挤到 Y146 才对」。
   代价（用户已知并接受）：「我的二维码」从 1 次点击变 2 次（底部按钮 → 左三角）。 */
static void fr_pageno(int off, int cnt)
{
    if (cnt <= FR_PER_PAGE) return;         /* 装得下就不画角标（全站口径） */
    fb_fill_white(152, 8, 199, 27);
    char pb[24];
    snprintf(pb, sizeof(pb), "%d/%d", off / FR_PER_PAGE + 1,
             (cnt + FR_PER_PAGE - 1) / FR_PER_PAGE);
    fb_text_r(ZF_FONT_BODY, 191, 13, pb);
}

/* 左右箭头：**照抄收藏品详情页（UI_LOOT_ITEM 槽30 底图烤死的）那两枚三角**
   —— 9 宽 × 17 高实心（逐行宽 2→9→2）。
   ★2026-10-23 用户令：整体下移 12px（y85..101，中心 y93），与二维码黑区
   （y35..151，中心 y93）视觉居中；形状/左右位置仍与收藏品页一致。
   ★两版都画（按键版 POWER 切屏），但每屏只画该方向那一枚。 */
static void draw_fr_arrow(int dir)
{
    static const uint8_t W[17] = { 2, 3, 4, 5, 6, 7, 8, 9, 9, 9, 8, 7, 6, 5, 4, 3, 2 };
    for (int i = 0; i < 17; ++i) {
        const int y = 85 + i;
        for (int j = 0; j < W[i]; ++j)
            fb_put(y, (dir > 0) ? (177 + j) : (22 - j));
    }
}

void draw_friend_page(void)
{
    fb_clear_white();
#if TOUCH_DEMO
    fb_back_arrow();
#endif
    fb_text_c(ZF_FONT_TITLE, 100, 12, T("朋友"));

    const int cnt = friend_count();
    if (g_fr_off >= cnt) g_fr_off = 0;
    if (g_fr_sel > cnt) g_fr_sel = cnt;   /* ★R92：删好友后光标可能越界 → 夹回底部「添加朋友」 */

    if (cnt <= 0) {
        fb_text_c(ZF_FONT_TITLE, 100, 96,  T("还没有朋友"));
        /* ★R193 用户令：二维码功能已下掉 ⇒ 删掉「让对方扫我的二维码」这句误导文案。 */
    } else {
        for (int i = 0; i < FR_PER_PAGE && g_fr_off + i < cnt; ++i) {
            const friend_info_t *f = friend_at(g_fr_off + i);
            const int y = AROW_Y[i];
            /* ★2026-10-22 用户令：朋友列表**不要行首圆点**（那是旅程页的站点标记）*/
            /* ★2026-10-25 基座接线④：右侧显示对方状态 + 留言未读。
               ★R193 用户令（「不能两种不一样」）：**这一列只显示状态**，没收到过状态时
               统一显示「未知」（原来兜底成好友码 ⇒ 同一列表里一个是「在家」、一个是
               「ABC123」，两套口径）。好友码只在该好友的详情页显示。 */
            fb_text_l_fit(ZF_FONT_TITLE, 12, y, f->name, 96);
            {
                int v = -1;
                const bool got = friend_net_vis_of(g_fr_off + i, &v);
                const bool unread = friend_net_msg_unread(g_fr_off + i);
                char right[24];
                if (got && v >= 2)      snprintf(right, sizeof(right), "%s", T("远行"));
                else if (got && v == 1) snprintf(right, sizeof(right), "%s", T("出门"));
                else if (got)           snprintf(right, sizeof(right), "%s", T("在家"));
                else                    snprintf(right, sizeof(right), "%s", T("未知"));
                if (unread) {
                    char r2[64];
                    snprintf(r2, sizeof(r2), "%s·%s", right, T("新留言"));
                    fb_text_r(ZF_FONT_BODY, 184, y + 1, r2);
                } else {
                    fb_text_r(ZF_FONT_BODY, 184, y + 1, right);
                }
            }
        }
    }

    /* ★R100：页码回右上角（与 WiFi / 明信片 / 收藏品列表一致）；好友 ≤ 3 个时不画。 */
    fr_pageno(g_fr_off, cnt);

    /* ★★ R92：底部整宽「添加朋友」（两版都有，交互各自不同）——
       触屏 = 点这一条；按键版 = BOOT 把光标移到它上面（g_fr_sel == cnt）再 POWER。
       样式 = 1px 圆角框 + 居中 12px 文字；选中态 = **内区整块反白**（框线保留），
       与 WiFi 列表页「+ 添加网络」的选中态同款。 */
    fb_round_rect(FR_ADD_BTN_X0, FR_ADD_BTN_Y0, FR_ADD_BTN_X1, FR_ADD_BTN_Y1);
    fb_text_c(ZF_FONT_TITLE, 100, 172, T("添加朋友"));

    /* 光标（触屏进页 = -1 不画，按键版进页 = 0 画） */
    if (g_fr_sel >= 0) {
        if (g_fr_sel >= cnt) {                         /* 选中的是底部按钮 */
            fb_invert_rect(FR_ADD_BTN_X0 + 1, FR_ADD_BTN_Y0 + 1,
                           FR_ADD_BTN_X1 - 1, FR_ADD_BTN_Y1 - 1);
        } else {
            const int r = g_fr_sel - g_fr_off;
            if (r >= 0 && r < FR_PER_PAGE) fr_row_sel(AROW_Y[r]);
        }
    }
}

/* ══ ★★ R92（2026-10-27 用户令）：好友详情页 ══════════════════════════════════
 * 用户原话：「朋友可以点进去，二级页面类似 WiFi 点进去的页面，显示朋友的名字和一个
 *   朋友码，下面两个按钮：修改 / 删除」。
 * 版面：**整套照抄 PG_WIFI_ITEM**（draw_wifi_item_page）—— 标题「朋友」y12 ·
 *   第 1 行 名字 y68（居中，超长截断）· 第 2 行 6 位码 y92 · 双按钮 y117..142
 *   （TP_BTN2_* 全站几何）：「修改」左 /「删除」右。只换了字段，坐标一字未改。
 * ★★ R93（2026-10-27 用户令）：「好友码不用像『我的好友码』页那么大，变成正常的 12 像素
 *   的一个字；跟上面的名字、下面的两个按钮的距离都近一点」+
 *   「整体跟点进 WiFi 后的详情页一模一样，那页也是两行文本 + 两个按钮，把字段换一下就行」。
 *   ⇒ ① 原 36px 大字（fb_text_code3x，18×36 占满 y60..96）撤掉，改常规 12px 标题档；
 *     ② 坐标改成 WiFi 详情页的 y68 / y92（原来 y42 / y60，两行散在页面上半）。
 *   ★「我的好友码」页（draw_friend_qr_page）仍保持 36px 大字不变 —— 那是给对方扫的。
 * 交互（全套照抄 PG_WIFI_ITEM）：
 *   触屏 = 点哪颗哪颗反白并**立即执行**（单段式，不是"先选再确认"）；
 *   按键版 = BOOT 在「修改/删除」间切选中，POWER 执行。
 * ★删除**没有二次确认**（用户 2026-10-27 拍板：「一切都朝 WiFi 列表页对齐」，
 *   而 WiFi 的删除本来就是点一下即删，点进详情页本身就算一次确认）。
 * 几何必须与 ui.bin 无关：本页**纯自绘**，不占 ui.bin 槽位（不必重新出资源）。 */
void draw_friend_item_page(void)
{
    fb_clear_white();
#if TOUCH_DEMO
    fb_back_arrow();
#endif
    fb_text_c(ZF_FONT_TITLE, 100, 12, T("朋友"));

    const friend_info_t *f = friend_at(g_fr_sel);
    if (f) {
        /* ★R93：**版面逐字照抄 PG_WIFI_ITEM**（draw_wifi_item_page，main.c:8224）——
           第 1 行 y68 / 第 2 行 y92，双按钮 y117..142；只把字段换成 好友名 / 好友码。
           WiFi 页第 2 行是 BODY(10px) 的「当前/未使用」注释，这里好友码是主信息 ⇒
           仍用 12px 标题档（用户令「正常的 12 像素的一个字」）。 */
        fb_text_c_fit(ZF_FONT_TITLE, 100, 68, f->name, 184);
        fb_text_c(ZF_FONT_TITLE, 100, 92, f->code);
    } else {
        fb_text_c(ZF_FONT_TITLE, 100, 68, T("好友已不在"));
    }

    /* 双按钮（空心框由固件画，选中那颗由 btn2_invert 只翻内区） */
    fb_round_rect(TP_BTN2_L0, TP_BTN2_Y0, TP_BTN2_L1, TP_BTN2_Y1);
    fb_round_rect(TP_BTN2_R0, TP_BTN2_Y0, TP_BTN2_R1, TP_BTN2_Y1);
    fb_text_c(ZF_FONT_TITLE, 52,  124, T("修改"));     /* 左钮中心 (8+97)/2 ≈ 52 */
    fb_text_c(ZF_FONT_TITLE, 146, 124, T("删除"));     /* 右钮中心 (102+191)/2 ≈ 146 */
    btn2_invert(g_fr_isel);                            /* 0=修改 1=删除 -1=无选中 */
}

/* ★2026-10-25 A3：好友码 36px 显示 —— 12px 标题档 ASCII 字形 **3 倍放大**（每墨点 3×3）。
   只服务好友码（A-Z0-9）：源字形 6×12 → 18×36；字间 gap 6px；水平居中于 cx。 */
static void fb_text_code3x(const char *s, int cx, int y_top)
{
    /* ★R193 用户令：「好友码字间距稍微收一点」→ GAP 6 → **3**（原来码之间空 1/3 个字宽，太散）。*/
    enum { GW = 6, S = 3, GAP = 3 };
    const int n = (int)strlen(s);
    const int cw = GW * S;
    const int total = n * cw + (n > 0 ? (n - 1) * GAP : 0);
    int pen = cx - total / 2;
    for (int i = 0; i < n; ++i, pen += cw + GAP) {
        int h = 0, rb = 0, adv = 0;
        const uint8_t *b = zf_glyph(ZF_FONT_TITLE, (uint32_t)(uint8_t)s[i], &h, &rb, &adv);
        if (!b) continue;
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < GW; ++c)
                if (ZF_BIT(b, r, rb, c)) {
                    const int py = y_top + r * S, px = pen + c * S;
                    for (int dy = 0; dy < S; ++dy)
                        for (int dx = 0; dx < S; ++dx) {
                            const int yy = py + dy, xx = px + dx;
                            if (yy >= 0 && yy < EPD_H && xx >= 0 && xx < EPD_W) fb_put(yy, xx);
                        }
                }
    }
}

void draw_friend_qr_page(void)
{
    if (!friend_pane_is_add()) {            /* 左屏：我的二维码（被动被加）*/
        fb_clear_white();
#if TOUCH_DEMO
        fb_back_arrow();
#endif
        fb_text_c(ZF_FONT_TITLE, 100, 12, T("我的好友码"));

        /* ★2026-10-25 用户令（改版）：去掉码底圆角框；好友码 12px 字形 3 倍放大 = 36px。
           ★R193 用户令：整块**上移**并把下方文案**靠近**好友码 —— 码 y74（占 74..109）、
           文案 y120 / y136（码底到首行 11px）；整块 74..146，在标题(12..24)与屏底之间居中。 */
        fb_text_code3x(friend_my_code(), 100, 74);
        fb_text_c(ZF_FONT_BODY, 100, 120, T("滑动打开添加朋友"));
        fb_text_c(ZF_FONT_BODY, 100, 136, T("输入好友码就能加上"));
        draw_fr_arrow(+1);                   /* 右滑 / 点右箭头 → 添加朋友 */
    } else {                                /* 右屏：主动添加（热点已开）*/
        /* ★★2026-10-23 用户令：**整页照抄配网引导页（UI_PROV_GUIDE 槽 11）的样式**——
           WiFi 图标 /「手机连WIFI」/ 热点名留空带 /「浏览器打开 192.168.4.1」/ 底部横框
           全部沿用底图，只换三处文字（坐标由 assets 页 11 逐像素量出）：
             ① 标题墨带 y12..24 →「添加朋友」（不碰左上 ‹）；
             ② 热点名留空带 y112..143 → SSID（PROV_SSID_TOP 同款）；
             ③ 底部按钮带 y164..191（旧「按住3秒跳过配网」+ 框线）整块擦白 →
                「输入对方好友码」「或让对方发码给你」（不是按钮，所以框线一并去掉）。 */
        fb_load_ui(UI_PROV_GUIDE);
        fb_fill_white(24, 10, 199, 26);
        fb_text_c(ZF_FONT_TITLE, 100, 12, T("添加朋友"));
        char ssid[40] = {0};
        ws_friend_ap_ssid(ssid, sizeof(ssid));
        fb_text_c_fit(ZF_FONT_TITLE, 100, PROV_SSID_TOP,
                      ssid[0] ? ssid : T("热点开启中"), 184);
        fb_fill_white(6, 160, 193, 196);
        fb_text_c(ZF_FONT_BODY, 100, 170, T("输入对方好友码"));
        draw_fr_arrow(-1);                   /* 左滑 / 点左箭头 → 我的二维码 */
    }
}

/* ══ ★★2026-10-25 基座接线①：串门场景（好友的猫来家里玩）═══
 * 素材 = 用户手绘（两只猫 / 阴影范围，200×200 同画布：猫 y113..169 · 影 y161..172，
 * 直接按原坐标落屏；第二组 y91 起）。
 * ★R121（用户令）：触发 = 好友 `fn<i>_vis == 1`（串门信号写下的有效期内也算）；
 *   主页**持续**画串门猫 + 告示牌直到到期 —— 不再弹 3 秒全屏场景页。 */
#include "visit_cats.inc"
#include "visit_shad.inc"
#include "visit_cats2.inc"
#include "visit_shad2.inc"

static void visit_blit_v(int x0, int y0, int w, int h, int row, const uint8_t *bits, uint8_t val)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if ((bits[y * row + (x >> 3)] >> (7 - (x & 7))) & 1) {
                const int yy = y0 + y, xx = x0 + x;
                if (yy >= 0 && yy < EPD_H && xx >= 0 && xx < EPD_W) fb[yy][xx] = val;
            }
}

typedef struct {
    int cx, cy, cw, ch, crow; const uint8_t *cbits; const uint8_t *cwbits;
    int sx, sy, sw, sh, srow; const uint8_t *sbits;
} visit_set_t;
static const visit_set_t VISIT_SETS[2] = {
    { 20, 113, VISIT_CAT_W,  VISIT_CAT_H,  VISIT_CAT_ROW,  VISIT_CAT_BITS,  VISIT_CAT_W_BITS,
      24, 161, VISIT_SHAD_W,  VISIT_SHAD_H,  VISIT_SHAD_ROW,  VISIT_SHAD_BITS },
    { 14,  91, VISIT_CAT2_W, VISIT_CAT2_H, VISIT_CAT2_ROW, VISIT_CAT2_BITS, VISIT_CAT2_W_BITS,
      27, 161, VISIT_SHAD2_W, VISIT_SHAD2_H, VISIT_SHAD2_ROW, VISIT_SHAD2_BITS },
};

/* ★2026-10-27 用户令：串门猫必须有**白平面**（与正常猫精灵同构）——
   否则猫肚子/眼白这些「白」像素在房间里是透明洞，会透出房间纹理。 */
static void visit_draw_set(const visit_set_t *vs)
{
    visit_blit_v(vs->sx, vs->sy, vs->sw, vs->sh, vs->srow, vs->sbits, 0);          /* 影（网点）*/
    visit_blit_v(vs->cx, vs->cy, vs->cw, vs->ch, vs->crow, vs->cwbits, 255);       /* 白 */
    visit_blit_v(vs->cx, vs->cy, vs->cw, vs->ch, vs->crow, vs->cbits, 0);          /* 黑 */
}

/* ★2026-10-27 用户令：告示牌「好友名的猫名」放不下时的取舍 ——
   · 优先保住**猫名**（好友名按像素宽度截断加「…」）；
   · 好友名最少留 2 个字，不够就干脆只显示猫名；
   · 名字一律用对方的原始拼写，**不做翻译**（日语猫名在中文明设备上也显示日语）。
   下面三个小工具只在本文件用：UTF-8 步进 / 串宽 / 取「能放进 maxw 的最长前缀」。 */
static int fu_next(const char *p, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)p;
    if (!s[0]) return 0;
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    /* ★R212 修（全量审计）：原实现直接读 s[1]/s[2]/s[3] 拼码点，既不校验续字节也不判
       NUL —— 好友名被按字节截断成「半个汉字」时会越界读 1~2 字节。
       现在逐段校验：续字节必须存在且形如 10xxxxxx，否则当 1 字节坏字符跳过。 */
    if ((s[0] & 0xE0) == 0xC0) {
        if (s[1] && (s[1] & 0xC0) == 0x80) {
            *cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            return 2;
        }
    } else if ((s[0] & 0xF0) == 0xE0) {
        if (s[1] && (s[1] & 0xC0) == 0x80 && s[2] && (s[2] & 0xC0) == 0x80) {
            *cp = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            return 3;
        }
    } else if ((s[0] & 0xF8) == 0xF0) {
        if (s[1] && (s[1] & 0xC0) == 0x80 && s[2] && (s[2] & 0xC0) == 0x80 &&
            s[3] && (s[3] & 0xC0) == 0x80) {
            *cp = ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                  ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            return 4;
        }
    }
    *cp = 0xFFFD;      /* 非法/截断：按 1 字节步进，绝不越界 */
    return 1;
}

/* 汉字前进宽度：直接问字库（zf_glyph 的 adv），不写死常量 —— 换字库也不会错位。 */
static int fu_cjk_adv(int kind)
{
    int h = 0, rb = 0, adv = 0;
    const uint8_t *g = zf_glyph(kind, 0x4E2Du, &h, &rb, &adv);
    return g ? adv : 12;
}

static int fu_width(int kind, const char *s)
{
    int h = 0, rb = 0, adv = 0, w = 0, n = 0;
    uint32_t cp = 0;
    const int cjk = fu_cjk_adv(kind);
    while ((n = fu_next(s, &cp)) != 0) {
        const uint8_t *g = zf_glyph(kind, cp, &h, &rb, &adv);
        w += g ? adv : cjk;
        s += n;
    }
    return w;
}

static int fu_prefix(int kind, const char *s, int maxw, char *out, int cap)
{
    int h = 0, rb = 0, adv = 0, w = 0, n = 0, off = 0;
    uint32_t cp = 0;
    const int cjk = fu_cjk_adv(kind);
    while ((n = fu_next(s, &cp)) != 0) {
        const uint8_t *g = zf_glyph(kind, cp, &h, &rb, &adv);
        const int aw = g ? adv : cjk;
        if (w + aw > maxw || off + n >= cap) break;   /* 不切半个 UTF-8 字符 */
        memcpy(out + off, s, (size_t)n);
        off += n; w += aw; s += n;
    }
    out[off] = 0;
    return w;
}

/* ★2026-10-27 用户令：连接词按界面语言走（不进 i18n 表 —— 那要重打包 assets，
   会冲掉设备里已存的明信片；字库已含「的/の」）。名字本身仍用对方原始拼写。 */
static const char *visit_conn(void)
{
    if (g_lang == LANG_EN) return "'s ";
    if (g_lang == LANG_JA) return "の";
    return "的";
}

/* 拼「好友名的猫名」，超宽按上面的取舍规则压缩。maxw = 告示牌标题行像素宽。 */
static void visit_sign_compose(char *out, int cap, const char *friend_name, const char *cat_name, int maxw)
{
    const int kind = ZF_FONT_TITLE;
    const char *conn = visit_conn();
    char full[72];
    if (!cat_name || !cat_name[0]) { snprintf(out, (size_t)cap, "%s", friend_name); return; }
    snprintf(full, sizeof(full), "%s%s%s", friend_name, conn, cat_name);
    if (fu_width(kind, full) <= maxw) { snprintf(out, (size_t)cap, "%s", full); return; }

    const int sepw = fu_width(kind, conn);
    const int ellw = fu_width(kind, "…");
    const int catw = fu_width(kind, cat_name);
    const int cjk  = fu_cjk_adv(kind);
    const int avail = maxw - catw - sepw - ellw;     /* 好友名可用的宽度 */
    if (avail >= 2 * cjk) {                           /* 至少还剩 2 个字位 → 截好友名 */
        char pre[40];
        fu_prefix(kind, friend_name, avail, pre, sizeof(pre));
        if (pre[0]) { snprintf(out, (size_t)cap, "%s…%s%s", pre, conn, cat_name); return; }
    }
    /* 极端：好友名连 2 个字都放不下 → 只显示猫名（本身超宽再截） */
    if (catw <= maxw) { snprintf(out, (size_t)cap, "%s", cat_name); return; }
    char pre[40];
    fu_prefix(kind, cat_name, maxw - ellw, pre, sizeof(pre));
    snprintf(out, (size_t)cap, "%s…", pre);
}

/* ★2026-10-27 用户令：好友的猫在串门时，主页房间里的猫换成串门那对精灵。
   which = 正在串门的好友下标（vis>0 的第一个；无 = -1）；组号按好友下标固定取（idx & 1），
   这样每次重绘都是同一对，不会一刷新就换猫。 */
int friend_visit_which(void)
{
    const int n = friend_count();
    for (int i = 0; i < n; ++i) {
        int v = 0;
        if (friend_net_vis_of(i, &v) && v == 1) return i;
    }
    return -1;
}

void friend_visit_draw_cats(int idx)
{
    if (idx < 0) return;
    visit_draw_set(&VISIT_SETS[idx & 1]);
}

/* 主页告示牌判据：任一好友的猫在外面（vis>0）→ 立牌浮出。
   ★2026-10-27 用户令（实测纠正）：牌上要写「好友名 + 的 + 对方猫名」
     （例如好友叫「触屏版」、对方猫叫「咪咪」→ 显示「触屏版的咪咪」）。
     猫名来自 state v3；老固件没带名字时只显示好友名，绝不空着。 */
bool friend_visit_sign(char *out, int cap)
{
    const int n = friend_count();
    for (int i = 0; i < n; ++i) {
        int v = 0;
        if (friend_net_vis_of(i, &v) && v == 1) {
            const friend_info_t *f = friend_at(i);
            if (!f) continue;
            char cat[24] = {0};
            if (!(friend_net_name_of(i, cat, sizeof(cat)) && cat[0]))
                snprintf(cat, sizeof(cat), "-");   /* ★用户令：对方还没给猫取名 → 用「-」占位（好友名的-） */
            visit_sign_compose(out, cap, f->name, cat, 120);
            return true;
        }
    }
    return false;
}

/* ══ ★2026-10-25 基座接线①附：串门图测试页（两组手绘素材切换预览）═══ */
int g_tvisit_grp = 0;         /* 预览组号 0/1 */

void draw_test_visit_page(void)
{
    /* ★2026-11-12 用户令：背景用**当前房间**（与合成预览同源）；画面按真实串门页来 ——
       只有那对猫 + 底部告示牌，顶上不出标题、底下不出调试文字（「第N组」已删）。 */
    test_draw_visit_bg();
#if TOUCH_DEMO
    fb_back_arrow();
#endif
    visit_draw_set(&VISIT_SETS[g_tvisit_grp & 1]);
    test_draw_visit_sign("一");     /* 测试态没有真实访客 → 名字位一根横杠占位 */
    ESP_LOGI(TAG, "页：串门（第 %d 组）", (g_tvisit_grp & 1) + 1);
}
