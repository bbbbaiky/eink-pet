/* ui_asset.c —— UI 页面容器（'UIRL'）读取层
 * 2026-11-11（C1 第四步第一片）：从 main.c **逐字搬出**，行为不变。
 *
 * 容器格式（与 scripts/_tools/ui_rle_compress.py **严格对应**）：
 *   [0..3] 魔数 'UIRL' · [4..7] 页数 N · [8..11] 页大小(=5000)
 *   [12..] 偏移表 (N+1) × uint32 LE ⇒ 第 i 页 = buf[off[i] : off[i+1])
 * RLE 令牌（与 Python 端 rle_encode/rle_decode 逐字对应）：
 *   b <  0x80 → 字面量：后跟 b 个字节原样拷贝
 *   b >= 0x80 → 重复  ：后跟 1 个字节值，重复 (b & 0x7F) 次
 * ⚠ 改任一侧都必须同步改另一侧，否则 UI 全花（先跑脚本的往返自检）。
 */
#include <esp_attr.h>       /* ★R198：EXT_RAM_BSS_ATTR（BSS 外置 PSRAM） */
#include <string.h>

#include <esp_log.h>

#include "assets.h"
#include "assets_rw.h"
#include "fb_core.h"        /* EPD_BUF_BYTES / EPD_W / EPD_H */
#include "ui_asset.h"

static const char *TAG_U = "ui_asset";

/* 两版 UI 容器不同 ⇒ 编译期按 TOUCH_DEMO 选本版的 AID。
   ★R108：分版打包（pack_assets.py --variant btn|touch），另一版只留 16B stub 保目录条目。 */
#if TOUCH_DEMO
#define AID_UI_CUR   AID_UI_TOUCH
#else
#define AID_UI_CUR   AID_UI_BTN
#endif
#define UI_PAGE_BYTES  EPD_BUF_BYTES   /* 5000 */
#define UI_MAX_PAGES   256             /* 页数上界（当前 208 = 52 × LANG_N(4)）*/
#define UI_COMP_MAX    5184            /* 单页压缩块上界（实测最大 2057B，留足余量）*/

static EXT_RAM_BSS_ATTR uint32_t s_ui_off[UI_MAX_PAGES + 1];   /* 偏移表（页数+1 项）进 RAM */
static uint32_t s_ui_n, s_ui_psz;
static bool     s_ui_ok = false;
static EXT_RAM_BSS_ATTR uint8_t  s_ui_cbuf[UI_COMP_MAX];       /* 单页压缩块缓冲 */
static EXT_RAM_BSS_ATTR uint8_t  s_ui_page[UI_PAGE_BYTES];     /* 解压缓冲（5000 B 常驻 BSS；一次只解一页） */

/* 开机一次：读容器头 12B + 偏移表进 RAM，并自检偏移表。
   任何异常 → false；之后每页取不到 → fb_load_ui 画白，**不崩、不花屏**。 */
bool ui_asset_init(void)
{
    if (s_ui_ok) return true;
    uint8_t hdr[12];
    if (!assets_region_read(AID_UI_CUR, 0, hdr, 12)) return false;
    if (hdr[0] != 'U' || hdr[1] != 'I' || hdr[2] != 'R' || hdr[3] != 'L') return false;
    s_ui_n   = a_rd32(hdr + 4);
    s_ui_psz = a_rd32(hdr + 8);
    if (s_ui_psz != (uint32_t)UI_PAGE_BYTES || s_ui_n == 0 || s_ui_n > UI_MAX_PAGES) {
        ESP_LOGW(TAG_U, "★ UI 容器头异常：n=%u psz=%u（期望 psz=%d）",
                 (unsigned)s_ui_n, (unsigned)s_ui_psz, UI_PAGE_BYTES);
        return false;
    }
    if (!assets_region_read(AID_UI_CUR, 12, s_ui_off, (s_ui_n + 1) * 4u)) return false;
    uint32_t len = 0;
    if (!assets_region(AID_UI_CUR, NULL, &len)) return false;
    if (s_ui_off[0] < 12u + (s_ui_n + 1) * 4u) return false;     /* 表头都不够 → 容器坏 */
    for (uint32_t i = 0; i < s_ui_n; ++i) {
        if (s_ui_off[i + 1] <= s_ui_off[i] || s_ui_off[i + 1] > len) return false;
    }
    s_ui_ok = true;
    return true;
}

/* 解压第 lp 页。任何异常都返回 NULL，由调用方兜底画白，
   ★ 绝不因资产损坏而越界读写 / 崩溃。 */
const uint8_t *ui_asset_page(int lp)
{
    if (!s_ui_ok && !ui_asset_init()) return NULL;
    if (lp < 0 || (uint32_t)lp >= s_ui_n || s_ui_psz != (uint32_t)UI_PAGE_BYTES) return NULL;

    const uint32_t o0 = s_ui_off[lp];
    const uint32_t o1 = s_ui_off[lp + 1];
    if (o1 <= o0) return NULL;                     /* 空页/偏移错乱 */
    const size_t clen = (size_t)(o1 - o0);
    if (clen > UI_COMP_MAX) return NULL;           /* 单页压缩块超缓冲 → 拒（不越界）*/
    if (!assets_region_read(AID_UI_CUR, o0, s_ui_cbuf, clen)) return NULL;

    const size_t page = UI_PAGE_BYTES;
    const uint8_t *s = s_ui_cbuf, *se = s_ui_cbuf + clen;
    uint8_t *dst = s_ui_page;
    size_t w = 0;
    while (s < se && w < page) {
        const uint8_t t = *s++;
        if (t & 0x80) {                            /* 重复 */
            size_t cnt = (size_t)(t & 0x7F);
            if (s >= se) break;
            const uint8_t v = *s++;
            if (w + cnt > page) cnt = page - w;    /* 截断保护 */
            memset(dst + w, v, cnt);
            w += cnt;
        } else {                                   /* 字面量 */
            size_t cnt = (size_t)t;
            if (s + cnt > se) cnt = (size_t)(se - s);
            if (w + cnt > page) cnt = page - w;
            memcpy(dst + w, s, cnt);
            s += cnt;
            w += cnt;
        }
    }
    if (w < page) memset(dst + w, 0xFF, page - w);  /* ★2026-10-23 修（M3）：补白填 0xFF（bit=1→白）*/
    return dst;
}
