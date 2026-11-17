/* ══════════════════════════════════════════════════════════════════════
 * shop_system.c —— 商店系统（每日货架/购买/售罄/确认页）
 * ★2026-10-26 重构第 2 步：自 main.c 原位剪切（内容零改动，只摘 static）。
 * ══════════════════════════════════════════════════════════════════════ */
#include "variant.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <esp_log.h>
#include <esp_random.h>
#include <nvs.h>

#include "loot_data.h"
#include "loot_fx.h"
#include "main_api.h"
#include "assets.h"
#include "assets/loot_icons.h"
#include "i18n.h"
#include "post_data.h"
#include "api_image.h"
#include "loot_system.h"
#include "shop_system.h"

/* TAG 由 main.c 定义、main_api.h extern（全工程同一 TAG） */

static int shop_day_of(time_t now);
static void shop_ensure(time_t now);
static void shop_load(void);
static void shop_reroll(time_t now);

/* ── 模块专属状态（原 static 变量/表）── */

static int  s_shop_slot[SHOP_SLOT_N];

static bool s_shop_ready = false;



int shop_item_icon(int item)
{
    if (item == SHOP_I_LUCK) return SHOP_ICON_LUCK;  /* 好运铃 = 原铃铛图（ITEM_ICON 5）*/
    if (item == SHOP_I_MEAL) return SHOP_ICON_MEAL;
    if (item == SHOP_I_WISH) return 4;    /* 许愿签 → 顺风符（ITEM_ICON 4）*/
    if (item == SHOP_I_SUN)  return 3;    /* 改名卡 = 原晴天符图（ITEM_ICON 3）*/
    if (item == SHOP_I_RECALL) return 0;  /* 召回币实际画 112 硬币图（见 main.c 商店页特判）*/
    if (item == 103) return 0;    /* 猫薄荷 */
    if (item == 104) return 1;    /* 小鱼饼干 */
    if (item == 105) return 2;    /* 提神草 */
    return 0;
}

/* ★2026-10-22 四：换日边界 12:00 → **24:00**（本地日序 = now_local/86400，午夜整点换日） */
static int shop_day_of(time_t now) { return (int)(now / 86400); }

static void shop_load(void)
{
    uint8_t slots[SHOP_SLOT_N] = { 0, 1, 2, 3, 4, 5 };
    if (loot_nvs_get_blob("shop_slot", slots, sizeof(slots)))
        for (int k = 0; k < SHOP_SLOT_N; ++k) s_shop_slot[k] = slots[k] % SHOP_POOL_N;
    /* ★2026-10-25 修（用户报「商店刷出 6 个一模一样的护照」）：槽位 blob 可能被
       旧固件/异版布局写脏（全 0、全 0xFF 等 → %SHOP_POOL_N 全落 0 = 六本护照）。
       自愈口径：重号 或 越界 → 判脏，当场重掷并落盘（换日逻辑 shop_ensure 不变）。 */
    {
        bool dirty = false;
        for (int k = 0; k < SHOP_SLOT_N; ++k)
            if (s_shop_slot[k] < 0 || s_shop_slot[k] >= SHOP_POOL_N) dirty = true;
        for (int a = 0; a < SHOP_SLOT_N && !dirty; ++a)
            for (int b = a + 1; b < SHOP_SLOT_N; ++b)
                if (s_shop_slot[a] == s_shop_slot[b]) { dirty = true; break; }
        if (dirty) {
            ESP_LOGW(TAG, "★ 商店槽位 blob 脏（重号/越界）→ 重掷 6 件");
            shop_reroll(0);          /* now 不可得（load 无参）→ 仅日志日序显示 0 */
            return;
        }
    }
    const int sold = loot_nvs_i32("shop_sold", 0);
    for (int k = 0; k < SHOP_SLOT_N; ++k) s_shop_sold[k] = ((sold >> k) & 1) != 0;
    s_shop_ready = true;
}

/* ★2026-10-25：掷 6 件上架（Fisher-Yates 不放回）并落盘 —— shop_ensure 换日与 load 自愈共用。 */
static void shop_reroll(time_t now)
{
    int pool[SHOP_POOL_N];
    for (int i = 0; i < SHOP_POOL_N; ++i) pool[i] = i;
    for (int i = SHOP_POOL_N - 1; i > 0; --i) {        /* Fisher-Yates，取前 6 */
        const int j = (int)(esp_random() % (unsigned)(i + 1));
        const int t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    uint8_t slots[SHOP_SLOT_N];
    for (int k = 0; k < SHOP_SLOT_N; ++k) {
        s_shop_slot[k] = pool[k];
        s_shop_sold[k] = false;
        slots[k] = (uint8_t)pool[k];
    }
    loot_nvs_set_blob("shop_slot", slots, sizeof(slots));
    loot_nvs_set_i32("shop_sold", 0);
    s_shop_ready = true;
    ESP_LOGW(TAG, "★★C2 商店上架 6 件（商业日 %ld）", (long)shop_day_of(now));
}

static void shop_ensure(time_t now)
{
    const int today = shop_day_of(now);
    if (loot_nvs_i32("shop_day", -1) == today) {
        if (!s_shop_ready) shop_load();
        return;
    }
    loot_nvs_set_i32("shop_day", today);
    shop_reroll(now);
}

const shop_pool_t *shop_item(int k) { return &SHOP_POOL[s_shop_slot[k] % SHOP_POOL_N]; }

bool shop_is_sold(int k) { return s_shop_sold[k]; }

/* ★2026-10-27：由「阈值 >= SHOP_I_LUCK」改为**显式虚拟件集合** —— 旧阈值会把真实收藏品
   100/101（万用工具/回忆相册）和 160..177（新六国信物/环境件）误判成消耗品（永远可买、不显「已拥有」）。
   只有下面这些才是可重复买的虚拟/消耗品。 */
bool shop_is_consumable(const shop_pool_t *it) {
    return it->item == SHOP_I_LUCK  || it->item == SHOP_I_MEAL  || it->item == SHOP_I_WISH ||
           it->item == SHOP_I_SUN   || it->item == SHOP_I_RECALL ||
           it->item == 103 || it->item == 104 || it->item == 105;   /* 三件虚拟消耗品（猫薄荷/小鱼饼干/提神草） */
}

bool shop_owned(const shop_pool_t *it)
{
    if (shop_is_consumable(it)) return false;                 /* 消耗品永远可买 */
    return (it->item >= 0 && it->item < N_LOOT) ? loot_is_owned(it->item) : false;
}

/* 商品效果一句话：收藏品 = g_loot[].note；两铃 = 旧 C6 文案（用户令回退，不抽象） */
const char *shop_note(const shop_pool_t *it)
{
    if (it->item == SHOP_I_LUCK) return T("下次出门必带回物品");
    if (it->item == SHOP_I_MEAL) return T("召唤猫猫过来吃饭");
    if (it->item == SHOP_I_WISH) return T("下次去最冷清的地方");
    if (it->item == 103) return T("使用后中等增加亲密度");
    if (it->item == 104) return T("使用后略微增加亲密度");
    if (it->item == 105) return T("使用后大幅安抚猫的心情");
    if (it->item == SHOP_I_SUN) return T("用掉后给猫改个名字");
    if (it->item == SHOP_I_RECALL) return T("使用后，猫猫会在一小时内回家");
    if (it->item >= 0 && it->item < N_LOOT) return t_(g_loot[it->item].note);
    return "";
}

/* ★R213（用户令）：消耗品入袋走 main.c::bell_add，而它对每个计数键**封顶 99**。
   到顶后再买，钱照扣、数量不变 = 白花。这里先按商品映射到 NVS 键，到顶直接拒买。 */
#define SHOP_BELL_CAP 99           /* 与 main.c::BELL_CAP 同值 */
static const char *shop_consumable_key(int item)
{
    if (item == SHOP_I_LUCK)   return "item_luckbell";
    if (item == SHOP_I_MEAL)   return "item_mealbell";
    if (item == SHOP_I_WISH)   return "item_wish";
    if (item == 103)           return "item_catnip";
    if (item == 104)           return "item_biscuit";
    if (item == 105)           return "item_herb";
    if (item == SHOP_I_SUN)    return "item_rename";
    if (item == SHOP_I_RECALL) return "item_recall";
    return NULL;
}

/* ★C3 购买（确认页确认后调）：扣款 → 发货 → 售罄。false = 没买成 */
bool shop_buy(int k)
{
    const shop_pool_t *it = shop_item(k);
    if (s_shop_sold[k] || coin_get() < it->price) return false;
    /* ★R213（用户令）：消耗品已到 99 封顶 → 不扣款、不发货（原来扣了钱数量不变）。 */
    const char *ck = shop_consumable_key(it->item);
    if (ck && loot_nvs_i32(ck, 0) >= SHOP_BELL_CAP) {
        ESP_LOGW(TAG, "★ 商店：「%s」已到上限 %d → 拒绝购买（不扣款）", it->name, SHOP_BELL_CAP);
        return false;
    }
    /* ★ 收藏品（非消耗品）已拥有 → 不扣款直接售罄（别让用户白花钱/重复买）；消耗品永远可买 */
    if (!shop_is_consumable(it) && it->item >= 0 && it->item < N_LOOT && loot_is_owned(it->item)) {
        s_shop_sold[k] = true;
        loot_nvs_set_i32("shop_sold", loot_nvs_i32("shop_sold", 0) | (1 << k));
        ESP_LOGW(TAG, "★★ 商店：「%s」已拥有（全解锁中）→ 不扣款直接售罄", it->name);
        return true;
    }
    /* ★占位件（LOOT_GONE）不可购买（稳定 ID 口径）—— 否则扣款后 grant 被拒 = 白花钱 */
    if (it->item >= 0 && it->item < N_LOOT && !loot_alive(it->item)) return false;
    coin_add(-it->price);
    if (it->item == SHOP_I_LUCK) {
        bell_add("item_luckbell", 1);
        ESP_LOGW(TAG, "★★ 好运铃 +1（现有 %d）：用掉后下趟必带回一件", loot_nvs_i32("item_luckbell", 0));
    } else if (it->item == SHOP_I_WISH) {
        bell_add("item_wish", 1);
        ESP_LOGW(TAG, "★★C6 许愿签 +1（现有 %d）：下次出门去最近最少去的地方",
                 loot_nvs_i32("item_wish", 0));
    } else if (it->item == SHOP_I_MEAL) {
        /* ★★2026-10-18 用户令「开饭铃买了也入计数」：不再即买即用，改为**入袋** ——
           消耗点 = 放饭那一刻（food_feed：盆里有食 + 有货 → 花掉 1 个，猫马上来吃）。 */
        bell_add("item_mealbell", 1);
        ESP_LOGW(TAG, "★★C6 开饭铃 +1（现有 %d）：放饭时猫马上来吃", loot_nvs_i32("item_mealbell", 0));
    } else if (it->item == 103) {
        /* ★2026-10-26 修：原来 103/104/105 落到 loot_grant → 花钱发成 g_loot[103] 号收藏品，
           item_* 计数永远 0 → 消耗品页永远看不到这三件。 */
        bell_add("item_catnip", 1);
        ESP_LOGW(TAG, "★★ 猫薄荷 +1（现有 %d）：亲密度 +3", loot_nvs_i32("item_catnip", 0));
    } else if (it->item == 104) {
        bell_add("item_biscuit", 1);
        ESP_LOGW(TAG, "★★ 小鱼饼干 +1（现有 %d）：亲密度 +1", loot_nvs_i32("item_biscuit", 0));
    } else if (it->item == 105) {
        bell_add("item_herb", 1);
        ESP_LOGW(TAG, "★★ 提神草 +1（现有 %d）：消气全消", loot_nvs_i32("item_herb", 0));
    } else if (it->item == SHOP_I_SUN) {
        /* ★2026-10-26 用户拍板：改名卡（原晴天符槽位）—— 消耗品，买了 +1 个，可反复买 */
        bell_add("item_rename", 1);
        ESP_LOGW(TAG, "★★ 改名卡 +1（现有 %d）：用掉后手机连热点改名",
                 loot_nvs_i32("item_rename", 0));
    } else if (it->item == SHOP_I_RECALL) {
        /* ★2026-10-26 用户拍板：召回币（112 号改造）—— 消耗品，买了 +1 个，可反复买 */
        bell_add("item_recall", 1);
        ESP_LOGW(TAG, "★★ 召回币 +1（现有 %d）：用掉后猫 1 小时内回家",
                 loot_nvs_i32("item_recall", 0));
    } else {
        loot_grant(it->item);
    }
    s_shop_sold[k] = true;
    int sold = loot_nvs_i32("shop_sold", 0) | (1 << k);
    loot_nvs_set_i32("shop_sold", sold);
    return true;
}

/* ── 商店动作（两版 handler 共用，各自薄封装）──────────────────────────── */
void shop_open(void)
{
    shop_ensure(g_app.st.now_local);
    g_shop_sel = 0;
    go_page(PG_SHOP);
}

void shop_next(void) { g_shop_sel = (g_shop_sel + 1) % SHOP_SLOT_N; page_render(true); }

void shop_btn_band(const char *txt)
{
    fb_round_rect(8, 164, 191, 191);
    fb_text_c_fit(ZF_FONT_TITLE, 100, 172, txt, 178);
}

void shop_conf_btn(int which, const char *txt, bool sel)
{
    const int l = which ? 102 : 8, r = which ? 191 : 97;
    fb_round_rect(l, 117, r, 142);
    fb_text_c(ZF_FONT_TITLE, (l + r) / 2, 124, txt);    /* ★先画字 */
    /* ★★ 2026-10-17：选中态改**框内整块反白**（墨底白字）—— 原型 p11 用 bag_inv_inside，
       与全站固件自绘的双按钮页（恢复出厂确认）同一套表达；原来的「双框」是底图烤死
       那一类（添加网络确认）的做法，固件这边没有第二张底图可换。 */
    if (sel) fb_invert_rect(l + 1, 118, r - 1, 141);
}
