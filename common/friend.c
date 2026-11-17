/* ══════════════════════════════════════════════════════════════════════
 * friend.c —— 好友码与好友表（NVS petns）
 *
 * 键位（全部 ≤15 字符）：
 *   fr_mycode   str  本机 6 位好友码
 *   fr_cnt      i32  好友数 0..FRIEND_MAX
 *   fr<n>_code  str  第 n 位好友的好友码
 *   fr<n>_name  str  第 n 位好友的昵称（添加时由用户填）
 * ══════════════════════════════════════════════════════════════════════ */
#include <esp_attr.h>       /* ★R198：EXT_RAM_BSS_ATTR（BSS 外置 PSRAM） */
#include "friend.h"
#include "friend_net.h"        /* ★R92：好友表变动后作废 fn<i>_* 缓存（friend_net_forget_*）*/
#include "net_sync.h"          /* ★2026-10-26：添加朋友热点 ws_friend_open/close/ssid/add_cb */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "friend";

/* 去掉 I/L/O 的 33 字符表（扫码后手输也不易错） */
static const char FR_ALPHA[] = "0123456789ABCDEFGHJKMNPQRSTUVWXYZ";

static char          s_my_code[FRIEND_CODE_LEN + 1] = "------";
static EXT_RAM_BSS_ATTR friend_info_t s_fr[FRIEND_MAX];
static int           s_fr_n = 0;

static bool code_ok(const char *c)
{
    if (!c || (int)strlen(c) != FRIEND_CODE_LEN) return false;
    for (int i = 0; i < FRIEND_CODE_LEN; ++i)
        if (!strchr(FR_ALPHA, c[i])) return false;
    return true;
}

static void code_gen(char out[FRIEND_CODE_LEN + 1])
{
    for (int i = 0; i < FRIEND_CODE_LEN; ++i)
        out[i] = FR_ALPHA[esp_random() % (sizeof(FR_ALPHA) - 1)];
    out[FRIEND_CODE_LEN] = 0;
}

/* ★2026-10-23 修（M14）：按 UTF-8 码点边界回退截断，避免 %.18s 切碎多字节序列 → NVS 存非法 UTF-8 上屏乱码 */
static void u8_copy_trunc(char *dst, size_t dstsz, const char *src)
{
    if (!dst || dstsz == 0) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= dstsz) {
        n = dstsz - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;   /* 回退到序列起点 */
    }
    if (src) memcpy(dst, src, n);
    dst[n] = 0;
}

void friend_init(void)
{
    s_fr_n = 0;

    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打不开 → 好友功能禁用");
        return;
    }

    size_t len = sizeof(s_my_code);
    if (nvs_get_str(h, "fr_mycode", s_my_code, &len) != ESP_OK || !code_ok(s_my_code)) {
        code_gen(s_my_code);
        nvs_set_str(h, "fr_mycode", s_my_code);
        ESP_LOGW(TAG, "首次生成好友码：%s", s_my_code);
    }

    int32_t n = 0;
    nvs_get_i32(h, "fr_cnt", &n);
    if (n < 0) n = 0;
    if (n > FRIEND_MAX) n = FRIEND_MAX;

    int kept = 0;
    int first_bad = -1;                    /* ★R119：第一个坏槽下标 —— 它之后所有 fn<i>_* 缓存都错位 */
    for (int i = 0; i < n; ++i) {
        char kc[20], kn[20];
        snprintf(kc, sizeof(kc), "fr%d_code", i);
        snprintf(kn, sizeof(kn), "fr%d_name", i);
        char c[8] = {0}, nm[20] = {0};
        size_t lc = sizeof(c), ln = sizeof(nm);
        if (nvs_get_str(h, kc, c, &lc) != ESP_OK || !code_ok(c)) {
            if (first_bad < 0) first_bad = i;
            continue;
        }
        if (nvs_get_str(h, kn, nm, &ln) != ESP_OK || !nm[0]) snprintf(nm, sizeof(nm), "%s", "好友");
        snprintf(s_fr[kept].code, sizeof(s_fr[kept].code), "%s", c);
        snprintf(s_fr[kept].name, sizeof(s_fr[kept].name), "%s", nm);
        ++kept;
    }
    s_fr_n = kept;

    /* ★2026-10-23 修（M14）：有坏槽被跳过（kept != n）时，把紧凑结果整表重写 + fr_cnt=kept，
       消除"内存下标与 NVS 槽号错位"—— 否则 friend_add 用 idx=s_fr_n 写槽会覆盖一条仍有效的好友。 */
    if (kept != n) {
        for (int i = 0; i < FRIEND_MAX; ++i) {          /* 先擦所有旧槽，避免残留坏槽 */
            char kc[20], kn[20];
            snprintf(kc, sizeof(kc), "fr%d_code", i);
            snprintf(kn, sizeof(kn), "fr%d_name", i);
            nvs_erase_key(h, kc);
            nvs_erase_key(h, kn);
        }
        for (int i = 0; i < kept; ++i) {                 /* 按紧凑顺序重写 */
            char kc[20], kn[20];
            snprintf(kc, sizeof(kc), "fr%d_code", i);
            snprintf(kn, sizeof(kn), "fr%d_name", i);
            nvs_set_str(h, kc, s_fr[i].code);
            nvs_set_str(h, kn, s_fr[i].name);
        }
        nvs_set_i32(h, "fr_cnt", kept);
        nvs_commit(h);
        /* ★R119 修：friend.c 只重写了 fr<i>_code/name，**没动作废 fn<i>_* 缓存**
           （留言/心情/未读/昵称）。下标一前移，旧缓存就挂到别人名下。
           从第一个坏槽起整段作废（同一下标之前的好友位置没变，保留其缓存），
           下次同步自动重建。 */
        friend_net_forget_from(first_bad < 0 ? kept : first_bad);
        ESP_LOGW(TAG, "★ 好友表有坏槽（%d→%d）→ 已紧凑重写，消除槽号错位", n, kept);
    }

    nvs_close(h);
    ESP_LOGI(TAG, "好友：我的码 %s · 已加 %d/%d", s_my_code, s_fr_n, FRIEND_MAX);
}

const char *friend_my_code(void) { return s_my_code; }

int friend_count(void) { return s_fr_n; }

const friend_info_t *friend_at(int i)
{
    return (i >= 0 && i < s_fr_n) ? &s_fr[i] : NULL;
}

/* ★R150：删掉 friend_exists()（原 139-145，7 行）—— 全工程零调用（用户报的重复添加
   判重走的是 friend_add 内部那段「已存在 → 返回下标」，本函数是早期独立查重接口，
   从来没有调用点，链接期已被 gc 掉）。要恢复：git 里 R149 及之前都有。 */

int friend_add(const char *code, const char *name)
{
    if (!code_ok(code)) return -1;

    /* 已存在 → 直接返回下标（重复扫码不重复写） */
    for (int i = 0; i < s_fr_n; ++i)
        if (strcmp(s_fr[i].code, code) == 0) return i;

    if (s_fr_n >= FRIEND_MAX) return -1;

    const int idx = s_fr_n;
    snprintf(s_fr[idx].code, sizeof(s_fr[idx].code), "%s", code);
    if (name && name[0]) u8_copy_trunc(s_fr[idx].name, sizeof(s_fr[idx].name), name);
    else                 snprintf(s_fr[idx].name, sizeof(s_fr[idx].name), "%s", "好友");

    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) {
        /* ★R216 全量检查：内存态已写、落盘却没开始 → 必须回滚，否则留半初始化好友条目 */
        s_fr[idx].code[0] = 0; s_fr[idx].name[0] = 0;
        return -1;
    }
    char kc[20], kn[20];
    snprintf(kc, sizeof(kc), "fr%d_code", idx);
    snprintf(kn, sizeof(kn), "fr%d_name", idx);
    nvs_set_str(h, kc, s_fr[idx].code);
    nvs_set_str(h, kn, s_fr[idx].name);
    nvs_set_i32(h, "fr_cnt", idx + 1);
    esp_err_t ce = nvs_commit(h);          /* ★2026-10-23 修（M14）：检查返回值，flash 写失败时回滚内存态 */
    nvs_close(h);
    if (ce != ESP_OK) {
        ESP_LOGE(TAG, "★ 好友落 NVS 失败（%d）→ 回滚内存态（重启即丢）", (int)ce);
        s_fr[idx].code[0] = 0; s_fr[idx].name[0] = 0;
        return -1;
    }

    ++s_fr_n;
    ESP_LOGW(TAG, "★ 加好友：%s（%s）→ 共 %d/%d", s_fr[idx].code, s_fr[idx].name, s_fr_n, FRIEND_MAX);
    return idx;
}

/* ══ ★★ R92（2026-10-27 用户令）：删除 / 修改好友 ══════════════════════════
 * 「删除」交互按用户拍板 = **没有二次确认**：详情页点一下「删除」就直接删
 *   （与 WiFi 列表完全一致 —— 那边 PG_WIFI_ITEM 的删除也是点一下即删）。
 * 落盘策略 = **整表重写**（先擦 0..FRIEND_MAX-1 全部槽，再按紧凑顺序写回 + fr_cnt）。
 *   理由：删中间一条会让后面所有人下标前移，逐槽挪动极易留残影 —— 旧槽残留会让
 *   friend_init 下次又把删掉的人读回来。整表最多 40 次写，代价可接受。
 * ⚠ 下标前移后 friend_net.c 的 fn<i>_* 缓存（状态/心情/留言/未读）会**张冠李戴**
 *   ⇒ 必须作废：del 作废 i 及之后；update 只作废该位（换人了，状态不该沿用）。 */
static bool fr_store_all(void)
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return false;
    for (int j = 0; j < FRIEND_MAX; ++j) {          /* 先擦光，避免旧槽残留 */
        char kc[20], kn[20];
        snprintf(kc, sizeof(kc), "fr%d_code", j);
        snprintf(kn, sizeof(kn), "fr%d_name", j);
        nvs_erase_key(h, kc);
        nvs_erase_key(h, kn);
    }
    for (int j = 0; j < s_fr_n; ++j) {              /* 按紧凑顺序重写 */
        char kc[20], kn[20];
        snprintf(kc, sizeof(kc), "fr%d_code", j);
        snprintf(kn, sizeof(kn), "fr%d_name", j);
        nvs_set_str(h, kc, s_fr[j].code);
        nvs_set_str(h, kn, s_fr[j].name);
    }
    nvs_set_i32(h, "fr_cnt", s_fr_n);
    const esp_err_t ce = nvs_commit(h);
    nvs_close(h);
    return ce == ESP_OK;
}

bool friend_del(int i)
{
    if (i < 0 || i >= s_fr_n) return false;

    friend_info_t save[FRIEND_MAX];
    const int     old_n = s_fr_n;
    memcpy(save, s_fr, sizeof(save));

    for (int j = i; j + 1 < s_fr_n; ++j) s_fr[j] = s_fr[j + 1];
    --s_fr_n;
    memset(&s_fr[s_fr_n], 0, sizeof(s_fr[0]));

    if (!fr_store_all()) {                          /* 落盘失败 → 内存态回滚，绝不"看着删了其实没删" */
        memcpy(s_fr, save, sizeof(save));
        s_fr_n = old_n;
        ESP_LOGE(TAG, "★ 删好友落 NVS 失败 → 回滚内存态（第 %d 位仍在）", i);
        return false;
    }

    friend_net_forget_from(i);                      /* 下标整体前移 → i 及之后的 fn 缓存全部作废 */
    ESP_LOGW(TAG, "★ 删好友：第 %d 位 → 剩 %d/%d（其后好友的 fn 缓存已作废，下次同步重建）",
             i, s_fr_n, FRIEND_MAX);
    return true;
}

bool friend_update(int i, const char *code, const char *name)
{
    if (i < 0 || i >= s_fr_n) return false;
    if (!code_ok(code))       return false;

    for (int j = 0; j < s_fr_n; ++j)                /* 改成的码不能与**别人**重复 */
        if (j != i && strcmp(s_fr[j].code, code) == 0) {
            ESP_LOGW(TAG, "★ 改好友：码 %s 已被第 %d 位占用 → 拒绝", code, j);
            return false;
        }

    const friend_info_t old = s_fr[i];
    snprintf(s_fr[i].code, sizeof(s_fr[i].code), "%s", code);
    if (name && name[0]) u8_copy_trunc(s_fr[i].name, sizeof(s_fr[i].name), name);
    else                 snprintf(s_fr[i].name, sizeof(s_fr[i].name), "%s", "好友");

    if (!fr_store_all()) {
        s_fr[i] = old;
        ESP_LOGE(TAG, "★ 改好友落 NVS 失败 → 回滚内存态");
        return false;
    }

    friend_net_forget_one(i);                       /* 换人了 → 旧好友的状态/留言缓存不能留着 */
    ESP_LOGW(TAG, "★ 改好友：第 %d 位 → %s（%s）", i, s_fr[i].code, s_fr[i].name);
    return true;
}

/* ══ 编辑目标（详情页「修改」用）═══════════════════════════════════════════
 * 设备只有 BOOT/POWER 两个键，6 位码 + 昵称在设备上没法敲 ⇒「修改」= 开**同一个**
 * 添加朋友热点，手机网页预填该好友的码与名，提交走 friend_update 而不是 friend_add。
 * 于是「修改」和「添加」共用一套门户，只在网页预填 + 回调分支上不同。 */
static int s_edit = -1;

void friend_edit_begin(int i) { s_edit = (i >= 0 && i < s_fr_n) ? i : -1; }
int  friend_edit_target(void) { return s_edit; }

/* ══ 添加朋友热点（portal）+ 二维码页左右两屏状态 ══════════════════════
 * ★2026-10-26 重构第 3 步（方案第 2 步收尾）：从 common/main.c 原样搬出 ——
 *   这一组是**朋友域的状态机**（谁开热点、当前在哪一屏），跟画图无关，放 friend.c 才对。
 *   main.c 只保留 draw_friend_* 画笔，通过 friend_pane_is_add() / friend_pane_portal_open() 读状态。
 * 口径：切到「添加朋友」屏才开热点，切回/离页立即收。 */
static bool s_fr_portal   = false;   /* 添加朋友热点是否已开 */
static bool s_fr_add_view = false;   /* 二维码页当前显示「添加朋友」说明 */

static int friend_portal_cb(const char *code, const char *name)
{
    /* ★R92：编辑目标有效 → 走「修改」，否则照旧「新增」 */
    if (s_edit >= 0 && s_edit < s_fr_n) {
        const int i = s_edit;
        return friend_update(i, code, name) ? i : -1;
    }
    return friend_add(code, name);
}

void friend_portal_start(void)
{
    if (s_fr_portal) return;
    ws_friend_set_add_cb(friend_portal_cb);
    ws_friend_set_edit(s_edit);          /* ★R92：把编辑目标交给门户，网页据此预填 */
    if (ws_friend_open()) { s_fr_portal = true; s_fr_add_view = true; }
    else                  ESP_LOGW(TAG, "朋友：添加热点开不起来");
}

void friend_portal_stop(void)
{
    if (s_fr_portal) { ws_friend_close(); s_fr_portal = false; }
    s_fr_add_view = false;
    s_edit = -1;                         /* ★R92：离页即清编辑态，下次进来默认是「新增」 */
}

void friend_pane_set(int add)
{
    if (add) {
        friend_portal_start();
        s_fr_add_view = s_fr_portal;      /* 热点开不起来 → 留在二维码屏 */
    } else {
        friend_portal_stop();
        s_fr_add_view = false;
    }
}

bool friend_pane_is_add(void)      { return s_fr_add_view; }
bool friend_pane_portal_open(void) { return s_fr_portal; }
