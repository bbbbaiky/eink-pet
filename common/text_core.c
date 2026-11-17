/* text_core.c —— 字库读取 + 文字绘制原语
 * 2026-11-11（C1 第三步）：从 main.c **逐字搬出**，行为与调用约定不变。
 * 只搬「查字 / 落墨」这一层；页面排版与业务文案仍留在 main.c。 */
#include <stdio.h>
#include <string.h>

#include <esp_log.h>

#include "assets/zfont.h"   /* ZF_* 字形几何常量（两版各自的 main/assets/zfont.h）*/
#include "fb_core.h"        /* fb[] / EPD_W / EPD_H */
#include "text_core.h"

static const char *TAG_T = "pet_text";

/* ══ UTF-8 ══════════════════════════════════════════════════════════════════ */

/* UTF-8 → 码点。返回消耗的字节数（0 = 串尾；非法字节按 '?' 吃掉 1 字节，保证不死循环）。 */
int utf8_next(const char *s, uint32_t *cp)
{
    /* ★R149：必须兜 NULL。t_() 在 zh==NULL 时**返回 NULL**（见 t_ 的早退），
       而 fb_text_c / zf_width / fb_fit_utf8 全链不查空就把结果喂进来 —— analyzer 在
       power_click → PG_FRIEND_QR 占位 → fb_pack → fb_hint_unify → fb_text_c 这条真实链上
       报 CWE-476「dereference of NULL」，此处 p[0] 即读地址 0（ESP32-S3 上直接 panic）。
       现状靠调用点实参都非 NULL 才没炸，属可触发地雷，一行兜住。 */
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) return 0;
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6)
            | (p[2] & 0x3F); return 3;
    }
    /* ★2026-10-13 任务二 P2-1 修：4 字节分支原来不校验延续字节 —— 输入被截断的
       "\xF0\x9F"+NUL 时也 return 4，调用方 p+=4 **跳过终止符**，下一轮从界外读。
       与 2/3 字节分支同构补延续校验；不满足 → 落到下面按 1 字节吃（'?'），不死循环也不越界。
       （短路 && 保证 p[3] 只在 p[1]/p[2] 均为延续字节时才读，不会越过 NUL。） */
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = '?'; return 4;   /* 4 字节（emoji 等）→ 替换符 */
    }
    *cp = '?'; return 1;
}

/* ══ 字库装载（zfont.bin，'ZFNT' 容器）══════════════════════════════════════
 * 布局与 scripts/_tools/_zfont_to_bin.py 严格对应（全 LE）：
 *   0x00 magic 'ZFNT' · 0x04 ver · 0x08 n_body_cjk · 0x0C n_title_cjk
 *   0x10 起：body_ascii 95×11 · title_ascii 95×12 · body_cjk n×24 ·
 *            title_cjk n×28 · body_idx n×4 · title_idx n×4
 * 开机把**码点索引**读进 RAM 做二分；命中后按需读单条字形。
 * 索引数组大小 = zfont.h 的 ZF_*_CJK_N 常量（抽 bin 时写入，两边必须一致）。
 * ★★2026-10-22 空间优化（用户批准的三档之一）：body_idx 与 title_idx 在 zfont.bin 里
 *   **逐字节完全相同**（实测 7455 字、两段 memcmp == 0，因为正文/标题共用同一字符集）
 *   ⇒ 只保留**一张**索引表供两者共用，省 29,820B 内部 RAM。
 *   开机仍会逐块比对 title_idx 与已加载的这份；不一致只报错（说明资产被换过），
 *   不会悄悄错字 —— 这是"零风险去重"的安全网。 */
static uint32_t s_zf_nbody, s_zf_ntitle;
static uint32_t s_zf_body_ascii_off, s_zf_title_ascii_off;
static uint32_t s_zf_body_cjk_off, s_zf_title_cjk_off;
static uint32_t s_zf_body_idx_off, s_zf_title_idx_off;
/* ★★2026-10-23 RAM 优化（空间审查批次）：索引表 7455×4 = 29,820B **搬进 PSRAM**
   （原来是内部 DRAM 常驻）。查字仍是二分，命中后按需读 assets 里的单条字形；
   PSRAM 8MB 只占 0.36%，换来内部 DRAM 余量 +29.8KB（本工程 DRAM 长期贴顶）。 */
static uint8_t *s_zf_idx = NULL;                   /* body/title 共用（原 2×29,820B）*/
static bool     s_zf_idx_shared = false;          /* true = 已校验两段一致 */
static uint8_t  s_zf_glyph[28];                   /* 单字形缓冲（最大 28B）*/

bool zfont_idx_load(void)                         /* 开机一次 */
{
    uint32_t off, len;
    if (!assets_region(AID_ZFONT, &off, &len) || len < 16) return false;
    uint8_t hdr[16];
    if (!assets_region_read(AID_ZFONT, 0, hdr, 16)) return false;
    if (a_rd32(hdr) != 0x544E465Au) return false;             /* 'ZFNT' */
    uint32_t zver = a_rd32(hdr + 4);
    s_zf_nbody  = a_rd32(hdr + 8);
    s_zf_ntitle = a_rd32(hdr + 12);
    if (s_zf_nbody != ZF_BODY_CJK_N || s_zf_ntitle != ZF_TITLE_CJK_N) return false;
    uint32_t o = 16;
    s_zf_body_ascii_off  = o;  o += ZF_ASCII_N * 11;
    s_zf_title_ascii_off = o;  o += ZF_ASCII_N * 12;
    s_zf_body_cjk_off    = o;  o += s_zf_nbody * 24;
    s_zf_title_cjk_off   = o;  o += s_zf_ntitle * 28;
    s_zf_body_idx_off    = o;  o += s_zf_nbody * 4;
    /* ★R183：ver2 起**只存一条码点索引**（title_idx 原本是 body_idx 的逐字节副本，
       只在开机做一致性校验，校验完就没人用 ⇒ 白占 12306×4 = 49,224 B）。
       ver1 老 bin 仍按原布局读 + 校验，两条索引不一致时照旧报错。 */
    if (zver >= 2) {
        s_zf_title_idx_off = s_zf_body_idx_off;
    } else {
        s_zf_title_idx_off = o;  o += s_zf_ntitle * 4;
    }
    if (o != len) return false;      /* bin 长度与布局不符 → 版本不匹配 */
    if (!s_zf_idx) s_zf_idx = psram_u8((size_t)s_zf_nbody * 4u);   /* ★2026-10-23：索引落 PSRAM */
    if (!s_zf_idx) return false;
    if (!assets_region_read(AID_ZFONT, s_zf_body_idx_off, s_zf_idx, s_zf_nbody * 4)) return false;

    /* ver1：逐块比对 title_idx 与这份索引（栈上 256B，不额外占静态 RAM）*/
    s_zf_idx_shared = (zver >= 2) || (s_zf_ntitle == s_zf_nbody);
    if (zver < 2 && s_zf_idx_shared) {
        uint8_t tmp[256];
        uint32_t done = 0, total = s_zf_nbody * 4u;
        while (done < total) {
            const uint32_t n = (total - done > sizeof(tmp)) ? (uint32_t)sizeof(tmp) : (total - done);
            if (!assets_region_read(AID_ZFONT, s_zf_title_idx_off + done, tmp, n)) { s_zf_idx_shared = false; break; }
            if (memcmp(tmp, s_zf_idx + done, n) != 0) { s_zf_idx_shared = false; break; }
            done += n;
        }
    }
    if (!s_zf_idx_shared) {
        ESP_LOGE(TAG_T, "★ 字模 title_idx 与 body_idx 不一致 → 标题字形会错（索引已共用，请重生成 zfont.bin）");
    }
    return true;
}

/* ══ 查字 / 量宽 ═════════════════════════════════════════════════════════════ */

/* 查一个字的「格」：返回位图（NULL = 缺字），并回填 行数/行字节数/前进宽度。
      —— 中文 WiFi 名要画在标题档上，之前标题档只有 ASCII，中文会整串变空心方框。
   ★ 两张汉字表都按**码点升序**排（gen_zfont.py 里 sorted()）→ 二分查找。
   ★ 前进宽度必须取 ZF_*_CJK_ADV，**不能拿格宽 ZF_*_CJK_W 冒充** ——
     正文档两者恰好都是 10，标题档是 12(格宽)/12(前进) 但含义不同，混用迟早踩。 */
const uint8_t *zf_glyph(int kind, uint32_t cp, int *h, int *rb, int *adv)
{
    /* ★★★ 2026-10-16 拆分重构④：四张表**不再内嵌在 app**（原来在 assets/zfont.h 的
       .rodata 里，两版各 ~389KB，两个 OTA 槽共 ~778KB 纯重复），改为放 assets 分区的
       zfont.bin（单份）。查字 = ① 码点索引（开机已读进 RAM，zf_*_idx）二分 →
       ② 命中后按需读那一条字形（body 24B / title 28B）到 s_zf_glyph 再返回。
       ⚠ 返回的指针指向**静态缓冲**，下次调用即失效（调用方都是立即消费，原行为一致）。
       索引/区域没装载（资产缺失）→ 一律返回 NULL（缺字方框），不崩。 */
    if (kind == ZF_FONT_TITLE) {
        *h = ZF_TITLE_H;
        if (cp >= ZF_ASCII_LO && cp <= ZF_ASCII_HI) {
            *rb = ZF_TITLE_RB; *adv = ZF_TITLE_ADV;
            if (!assets_region_read(AID_ZFONT,
                                    s_zf_title_ascii_off + (cp - ZF_ASCII_LO) * 12u,
                                    s_zf_glyph, 12)) return NULL;
            return s_zf_glyph;
        }
        /* CJK：索引在 RAM（4B/条，与表同序 = 码点升序）→ 二分 */
        size_t lo = 0, hi = s_zf_ntitle;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (a_rd32(s_zf_idx + mid * 4) < cp) lo = mid + 1; else hi = mid;
        }
        if (lo < s_zf_ntitle && a_rd32(s_zf_idx + lo * 4) == cp) {
            *rb = ZF_TITLE_CJK_RB; *adv = ZF_TITLE_CJK_ADV;
            if (!assets_region_read(AID_ZFONT,
                                    s_zf_title_cjk_off + (uint32_t)lo * 28u + 4u,
                                    s_zf_glyph, 24)) return NULL;   /* +4 跳过 cp 字段 */
            return s_zf_glyph;
        }
        return NULL;
    }
    *h = ZF_BODY_H;
    if (cp >= ZF_ASCII_LO && cp <= ZF_ASCII_HI) {
        *rb = ZF_BODY_RB;
        if (!assets_region_read(AID_ZFONT,
                                s_zf_body_ascii_off + (cp - ZF_ASCII_LO) * 11u,
                                s_zf_glyph, 11)) return NULL;
        *adv = s_zf_glyph[0];          /* 条目第 0 字节 = 前进宽度 */
        return s_zf_glyph + 1;         /* 其后 10B = 位图 */
    }
    size_t lo = 0, hi = s_zf_nbody;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a_rd32(s_zf_idx + mid * 4) < cp) lo = mid + 1; else hi = mid;
    }
    if (lo < s_zf_nbody && a_rd32(s_zf_idx + lo * 4) == cp) {
        *rb = ZF_CJK_RB; *adv = ZF_CJK_ADV;
        if (!assets_region_read(AID_ZFONT,
                                s_zf_body_cjk_off + (uint32_t)lo * 24u + 4u,
                                s_zf_glyph, 20)) return NULL;       /* +4 跳过 cp 字段 */
        return s_zf_glyph;
    }
    return NULL;                       /* 表里没收录的字 → 缺字（画空心方框） */
}

/* 串宽（按前进宽度累加；缺字按一格算，保证居中还算得准） */
int zf_width(int kind, const char *utf8)
{
    const char *p = utf8;
    uint32_t cp;
    int w = 0, n;
    while ((n = utf8_next(p, &cp)) != 0) {
        p += n;
        int h, rb, adv;
        if (zf_glyph(kind, cp, &h, &rb, &adv)) w += adv;
        else w += (kind == ZF_FONT_TITLE) ? ZF_TITLE_CJK_ADV : ZF_CJK_ADV;
    }
    return w;
}

/* 本串「第一个非空表行」——落屏时把它对齐到调用方给的 y */
int zf_first_ink_row(int kind, const char *utf8)
{
    const char *p = utf8;
    uint32_t cp;
    int best = 1 << 30, n;
    while ((n = utf8_next(p, &cp)) != 0) {
        p += n;
        int h, rb, adv;
        const uint8_t *b = zf_glyph(kind, cp, &h, &rb, &adv);
        if (!b) { if (best > 0) best = 0; continue; }   /* 缺字从第 0 行画 */
        for (int r = 0; r < h; ++r) {
            int any = 0;
            for (int c = 0; c < rb * 8; ++c) {
                if (ZF_BIT(b, r, rb, c)) { any = 1; break; }
            }
            if (any) { if (r < best) best = r; break; }
        }
    }
    return (best == (1 << 30)) ? 0 : best;      /* 全是空格 → 0 */
}

/* 本串墨迹的列范围 [c0,c1]（相对串首 cell 左缘；无墨返回 false）。
   ★ zf_width 累加的是**前进宽度 adv**，不是墨迹宽 —— 要按墨迹居中的地方
   （邮编框 fb_zip_boxes）用这个量。缺字跳过（按无墨处理）。 */
bool zf_ink_cols(int kind, const char *utf8, int *c0, int *c1)
{
    const char *p = utf8;
    uint32_t cp;
    int best0 = 1 << 30, best1 = -1, pen = 0, n;
    while ((n = utf8_next(p, &cp)) != 0) {
        p += n;
        int h, rb, adv;
        const uint8_t *b = zf_glyph(kind, cp, &h, &rb, &adv);
        if (!b) { pen += (kind == ZF_FONT_TITLE) ? ZF_TITLE_CJK_ADV : ZF_CJK_ADV; continue; }
        for (int r = 0; r < h; ++r)
            for (int c = 0; c < rb * 8; ++c)
                if (ZF_BIT(b, r, rb, c)) {
                    if (pen + c < best0) best0 = pen + c;
                    if (pen + c > best1) best1 = pen + c;
                }
        pen += adv;
    }
    if (best1 < 0) return false;
    *c0 = best0;
    *c1 = best1;
    return true;
}

/* 12px 标题字库是否含某 Unicode 码点（供取名校验「缺字点名」） */
bool zf_has_title_glyph(uint32_t cp)
{
    int h, rb, adv;
    return zf_glyph(ZF_FONT_TITLE, cp, &h, &rb, &adv) != NULL;
}

/* ══ 落墨 ═══════════════════════════════════════════════════════════════════ */

/* 画一串字：水平**按前进宽度居中**在 cx；本串第一个非空行落在 y。
   墨 = 黑（fb = 0）。返回画出的墨点数（0 = 什么都没画出来，可用于自检）。 */
/* ★★ 2026-10-20 抽取（消除本工程"改一处必漏另一处"的头号隐患）：
   fb_text_c() 与 fb_text_l() 原本**各抄了一份逐字相同的字形落墨循环** ——
   缺字画豆腐块 + 逐行逐位落墨 + 屏幕边界裁剪 + ink 计数，四件事写了两遍。
   它们都在**两版共用区**（不在 #if TOUCH_DEMO 分叉里），本该天然共用，
   却仍是复制品；越界裁剪、墨点计数、兜底尺寸任一处要改都得同时改两处 ——
   本文件里已有多条"两边各一份、只改了一边"的审计注记，正是这个模式的代价。
   ⇒ 抽成这一个函数；调用方只保留各自的 pen / dy 起点算法。
   ⚠ 纯机械抽取，行为逐字节等价：dy = 表行 0 对应的 y，pen = 起始 x，返回落墨像素数。 */
static int zf_stamp_run(int kind, const char *utf8, int pen0, int dy)
{
    int ink = 0, pen = pen0;
    const char *p = utf8;
    uint32_t cp;
    int n;
    while ((n = utf8_next(p, &cp)) != 0) {
        p += n;
        int h, rb, adv;
        const uint8_t *b = zf_glyph(kind, cp, &h, &rb, &adv);
        if (!b) {
            /* 缺字：画一个空心方框（豆腐），免得「静默不画」却没人发现 */
            int side = (kind == ZF_FONT_BODY) ? ZF_BODY_H : ZF_TITLE_H;
            for (int r = 0; r < side; ++r) {
                for (int c = 0; c < side; ++c) {
                    if (r && c && r != side - 1 && c != side - 1) continue;
                    int yy = dy + r, xx = pen + c;
                    if (yy >= 0 && yy < EPD_H && xx >= 0 && xx < EPD_W) { fb[yy][xx] = 0; ++ink; }
                }
            }
            pen += side;
            continue;
        }
        for (int r = 0; r < h; ++r) {
            int yy = dy + r;
            if (yy < 0 || yy >= EPD_H) continue;
            for (int c = 0; c < rb * 8; ++c) {
                if (!ZF_BIT(b, r, rb, c)) continue;
                int xx = pen + c;
                if (xx < 0 || xx >= EPD_W) continue;
                fb[yy][xx] = 0; ++ink;
            }
        }
        pen += adv;
    }
    return ink;
}

int fb_text_c(int kind, int cx, int y, const char *utf8)
{
    /* ★★ 2026-10-13 多语言：运行时上屏文字的**唯一入口**都在这里接 i18n。
       为什么不在每个调用点包 T()：全站 ~600 处字符串字面量，逐个包既易漏又易错，
       而且静态表（g_posts[] / g_loot[]）的初值里**放不了函数调用**。
       放在这一层 ⇒ 调用点一行不改；中文态 t_() 零查表直接返回原文，行为与改动前完全等价。
       —— 这份说明原本被复制了 5 份（c / c_fit / l / l_fit / r），2026-10-20 收敛：
          完整说明只留此处，其余四处改为一行短引用「i18n 入口同 fb_text_c」。 */
    utf8 = t_(utf8);
    int w = zf_width(kind, utf8);
    int pen = cx - w / 2;
    if (pen < 2) pen = 2;                        /* 超宽串别顶出左边界 */
    int top = zf_first_ink_row(kind, utf8);
    return zf_stamp_run(kind, utf8, pen, y - top);
}

/* ══ 两版共用 · 截断内核 ══════════════════════════════════════════════════════
   ★R146：`fb_text_c_fit` / `fb_text_l_fit` / `fb_text_r_fit` 三个函数里原本各抄了一份
   **逐字相同**的断字循环（21 行 × 3 份 = 63 行）。三份只要有一份改了口径就会三处不一致
   —— 这正是「PC 端出图 vs 固件上屏宽度对不上」那类问题的温床。
   ⇒ 抽成本函数，三个 _fit 只管「怎么对齐」，断字只有这一处。
   口径（不许改，改了三版预览会一起漂）：
     取「最长的前缀 P 使 width(P + '…') ≤ max_w」；整串放得下就原样不动、不加「…」。
   返回 1 = 发生过截断。 */
static int fb_fit_utf8(int kind, const char *utf8, int max_w, char *out, size_t cap)
{
    const int cjk_adv = (kind == ZF_FONT_TITLE) ? ZF_TITLE_CJK_ADV : ZF_CJK_ADV;
    int h = 0, rb = 0, adv = 0;
    /* ★R189：字库里没有「…」（U+2026）时**不要**再塞它 —— 否则截断位置会画成空心方框
       （用户报「标题有无法显示的字符就方框」）。缺字形 → 只截断、不加省略号。 */
    const bool has_ell = zf_glyph(kind, 0x2026, &h, &rb, &adv) != NULL;
    const int ell_w = has_ell ? adv : 0;
    if (zf_width(kind, utf8) <= max_w) { snprintf(out, cap, "%s", utf8); return 0; }
    size_t off = 0;
    int w = 0, n;
    uint32_t cp;
    const char *p = utf8;
    while ((n = utf8_next(p, &cp)) != 0) {
        const uint8_t *g = zf_glyph(kind, cp, &h, &rb, &adv);
        const int aw = g ? adv : cjk_adv;
        if (w + aw + ell_w > max_w) break;              /* 再塞一个，「…」就放不下了 */
        if (off + (size_t)n >= cap - 4) break;          /* 保险：缓冲留 4 字节给「…」+NUL */
        memcpy(out + off, p, (size_t)n);
        off += (size_t)n;
        w += aw;
        p += n;
    }
    if (has_ell) memcpy(out + off, "\xE2\x80\xA6", 4);  /* UTF-8 的 '…'，连 NUL 一起写 */
    else         out[off] = 0;
    return 1;
}

/* ★★ 2026-09-22 新增：「按宽度截断」版。
   为什么必须有：WiFi 名是运行时数据，用户确认会有中文 SSID → 长名（如
   「小区公共WiFi网速太慢了我们还是别用了吧」= 216px）会直接画到屏幕外。
   用户拍板的口径：**左右各留 8px，超出用「…」收尾**（截断在字符边界，不切半个字）。
   ★ 与 PC 端出图口径必须一字不差：`_preview/preview_cjk_ssid.py::fit()`
     —— 取「最长的前缀 P 使 width(P + '…') ≤ max_w」；整串放得下就不加「…」。
   用于：连接页网名（max_w = 184 = 200 − 8×2）、WiFi 管理列表页网名（max_w = 130）。 */
int fb_text_c_fit(int kind, int cx, int y, const char *utf8, int max_w)
{
    /* i18n 入口：完整说明见 fb_text_c 上方（★2026-10-20 收敛，此处不再复制 4 行）。 */
    utf8 = t_(utf8);
    char buf[80];
    fb_fit_utf8(kind, utf8, max_w, buf, sizeof(buf));
    return fb_text_c(kind, cx, y, buf);
}

/* ★★ 2026-09-22 新增：**左对齐**版（起点就是 x）。WiFi 列表页网名要左对齐 x=12。
     找「第一个非空行」。原因：列表页左右两列必须**同基线**；标题档里 ASCII 的墨迹在表行
     2..9、汉字在 0..11，若各按「首个非空行」对齐，ASCII 会被顶高约 2px（用户一眼看出
     「wifi 名字和右边的连接状态没对齐」）。按表行 0 对齐后：ASCII 墨迹 = y+2..y+9、
     汉字 = y..y+11 —— 两者**中心都是 y+5.5**，与设计稿量的（40..51 / 42..49）逐像素一致。 */
int fb_text_l(int kind, int x, int y, const char *utf8)
{
    /* i18n 入口：完整说明见 fb_text_c 上方（★2026-10-20 收敛，此处不再复制 4 行）。 */
    utf8 = t_(utf8);
    /* ★ 落墨统一走 zf_stamp_run（与 fb_text_c 同一份实现，2026-10-20 抽取）。
       dy = y = **表行 0 对齐**（两列同基线；原因见本函数上方 2026-09-22 那条注释）。 */
    return zf_stamp_run(kind, utf8, x, y);
}

/* 左对齐 + 按宽度截断（口径与 fb_text_c_fit 一字不差：整串放得下就不加「…」）。 */
int fb_text_l_fit(int kind, int x, int y, const char *utf8, int max_w)
{
    /* i18n 入口：完整说明见 fb_text_c 上方（★2026-10-20 收敛，此处不再复制 4 行）。 */
    utf8 = t_(utf8);
    char buf[80];
    fb_fit_utf8(kind, utf8, max_w, buf, sizeof(buf));
    return fb_text_l(kind, x, y, buf);
}

/* ★2026-10-27 新增：**右对齐** + 按宽度截断（列表行尾寄信人用）。
   口径与 fb_text_l_fit 逐字相同，只是最后交给 fb_text_r。 */
int fb_text_r_fit(int kind, int xr, int y, const char *utf8, int max_w)
{
    utf8 = t_(utf8);
    char buf[80];
    fb_fit_utf8(kind, utf8, max_w, buf, sizeof(buf));
    return fb_text_r(kind, xr, y, buf);
}

/* ★★ 2026-10-13 新增（用户 bug「底部的按钮的文本好像没有水平居中」）：
   把文字**按墨迹框对称居中在按钮框 [x0,x1] 内**（不是居中在某个点上）。
   为什么要单独一个函数：原写法一律 `fb_text_c_fit(..., cx=100, ...)` —— cx=100 是"整屏
   中点"（200/2），而按钮框是 x8..191（真中 99.5）⇒ 文字恒偏右 0.5px；且烤图侧
   `_btn_round` 用 cx=(8+191)//2=99（偏左 0.5px）⇒ **同一颗按钮，烤图与固件两套版心差 1px**。
   修法 = 两边都用同一条"按框留白对称"公式：left = x0 + (框宽 - 墨迹宽 + 1) / 2
   （余数那 1px 给右侧；与 build_ui_touch.py::_btn_round 逐字同式）。
   超宽则退回 fit 截断（max_w = 框宽 - 8，留左右各 4px 余量）。 */
int fb_btn_text_fit(int kind, int x0, int x1, int y, const char *utf8, int max_w)
{
    const char *s = t_(utf8);
    char buf[80];
    if (zf_width(kind, s) > max_w) {
        /* ★R208 修（复查）：原来超宽时交给 fb_text_c_fit(cx=100) —— 那个 100 是**整屏中点**，
           不是按钮框中点 ⇒ 半宽按钮（x8..97 / x102..191）上的长译文会跨过中缝、压住两颗按钮。
           现在先按 max_w 截断，再按**本按钮框**对称居中（下面同一条公式）。 */
        fb_fit_utf8(kind, s, max_w, buf, sizeof(buf));
        s = buf;
    }
    const int w = x1 - x0 + 1;
    const int tw = zf_width(kind, s);
    int tx = x0 + (w - tw + 1) / 2;               /* ★与 _btn_round 同式：留白左=floor、右=ceil */
    if (tx < x0) tx = x0;                         /* 极端长串兜底：绝不越过按钮左沿 */
    return fb_text_l(kind, tx, y, s);
}

/* 右对齐版：右边界 = rx。用于 WiFi 列表的「已连接/未连接」列、设置页 4 行的值。
   ★ 必须走 fb_text_l（表行 0 对齐），**不能**走 fb_text_c —— 否则又变回「首行对齐」而错位。 */
int fb_text_r(int kind, int rx, int y, const char *utf8)
{
    /* i18n 入口：完整说明见 fb_text_c 上方（★2026-10-20 收敛，此处不再复制 4 行）。 */
    utf8 = t_(utf8);
    return fb_text_l(kind, rx - zf_width(kind, utf8), y, utf8);
}
