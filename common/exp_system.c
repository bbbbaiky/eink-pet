/* ══════════════════════════════════════════════════════════════════════
 * exp_system.c —— 远行系统（套装判定/链式掷骰/旅途发信/归乡结算/纪念池）
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
#include "post_system.h"      /* ★2026-11-10：邮政等级 post_mail_cls_* / 慢递 post_slow_* */
#include "api_image.h"
#include "loot_system.h"
#include "exp_system.h"

/* TAG 由 main.c 定义、main_api.h extern（全工程同一 TAG） */

/* ★R150：exp_souv_grant / exp_check_toast 两行本地原型已删 —— exp_system.h 才是真源
   （本文件第 25 行已 include，两处并存触发 -Wredundant-decls）。下面三行 static 是必需的。*/
static bool exp_bag_ready(void);
static bool exp_souv_have(int ed, int k);
static void exp_cd_set(int ed, int v);
static void exp_souv_save(void);

static bool exp_souv_have(int ed, int k) { return (exp_souv_got[ed] >> k) & 1u; }

static void exp_souv_save(void)
{
    loot_nvs_set_blob("exp_souv_got", exp_souv_got, sizeof(exp_souv_got));
}

void exp_souv_load(void)
{
    memset(exp_souv_got, 0, sizeof(exp_souv_got));
    (void)loot_nvs_get_blob("exp_souv_got", exp_souv_got, sizeof(exp_souv_got));
}

/* 归来送一件：从池里挑**没得过的**（且该件已拥有则跳过——不重复给）；全齐 → return false（亲密度兜底）。 */
bool exp_souv_grant(int ed)
{
    /* ★★★ 2026-10-26 修（真崩溃，R63~R66 都中）：本函数是**所有回家**共用的，
       近郊回家时 ed = exp_active() = **-1** ⇒ EXP_SOUV_POOL[-1][k] / EXP_TBL[-1].name
       全是越界读。旧代码 k<4 时越界落点**恰好**是 0xFF（跳过）或命中已拥有件，
       所以一直没炸；池宽 4→8 后多读 32 字节，取到一个"合法件号" ⇒ 走 loot_grant
       ⇒ 下一行 EXP_TBL[-1].name 是 0xffffffff ⇒ LoadProhibited 重启。
       实测日志：近郊回家「回家结算：目的地 1」之后立刻 Guru Meditation。
       正确做法：非远行（ed 非法）直接返回 false，让调用方走亲密度兜底。 */
    if (ed < 0 || ed >= EXP_N) return false;
    for (int k = 0; k < EXP_SOUV_W; ++k) {
        const int it = EXP_SOUV_POOL[ed][k];
        if (it == 255 || exp_souv_have(ed, k) || loot_is_owned(it)) continue;
        exp_souv_got[ed] |= (uint8_t)(1u << k);
        exp_souv_save();
        loot_grant(it);
        ESP_LOGW(TAG, "★B8 远行纪念池：%s 送「%s」", EXP_TBL[ed].name, g_loot[it].name);
        return true;
    }
    return false;
}

int  exp_active(void)            { int v = loot_nvs_i32("exp_dest", -1); return (v < 0 || v >= EXP_N) ? -1 : v; }   /* ★2026-10-25 三.1：上界改 EXP_N（NVS 脏值夹取，防越界读） */   /* ★2026-10-23 修：NVS 脏值夹取 [0,3]，防 EXP_TBL[ed]/s_exp_post_idx[ed] 越界读 */

int  exp_day_of(time_t now)      { return (int)(now / 86400); }  /* now_local 已含时区 → 本地日序 */

static bool    s_exp_post_ready = false;

void exp_post_index_build(void)
{
    if (s_exp_post_ready) return;
    for (int i = 0; i < N_POST; ++i) {
        const char *c = g_posts[i].city;
        int ed = -1;
        if      (!strcmp(c, "冰岛") || !strcmp(c, "海上") || !strcmp(c, "中转站")) ed = 0;
        else if (!strcmp(c, "京都"))   ed = 1;
        else if (!strcmp(c, "撒哈拉")) ed = 2;
        else if (!strcmp(c, "威尼斯")) ed = 3;
        /* ★2026-10-25 三.1波一：6 新城市分支先行接入（g_posts 里该城卡片 = 0 张时池为空，
           波三（432 张回写）入库后自动装满 —— 空池路径安全：exp 寄信走预制兜底）。 */
        else if (!strcmp(c, "土耳其")) ed = 4;
        else if (!strcmp(c, "秘鲁"))   ed = 5;
        else if (!strcmp(c, "埃及"))   ed = 6;
        else if (!strcmp(c, "摩洛哥")) ed = 7;
        else if (!strcmp(c, "新西兰")) ed = 8;
        else if (!strcmp(c, "印度"))   ed = 9;
        if (ed >= 0 && ed < EXP_N && s_exp_post_n[ed] < EXP_POST_GRP)
            s_exp_post_idx[ed][s_exp_post_n[ed]++] = (uint16_t)i;   /* ★R122：uint8_t 会把 ≥256 的下标截断 */
    }
    s_exp_post_ready = true;
    {   /* ★2026-10-25 三.1：日志改逐站输出（10 站打一行太长） */
        char lg[128]; int o = snprintf(lg, sizeof(lg), "远行明信片分组：");
        if (o < 0) o = 0;
        if (o > (int)sizeof(lg) - 1) o = (int)sizeof(lg) - 1;
        /* ★2026-11-12 修：snprintf 返回的是「本想写的长度」，直接累加会在
           截断后把 o 推过 sizeof(lg)，后续站名全被循环条件跳过。按实写长度累加。 */
        for (int e2 = 0; e2 < EXP_N && o < (int)sizeof(lg) - 8; ++e2) {
            const int room = (int)sizeof(lg) - o;
            int w = snprintf(lg + o, (size_t)room, " %s%d",
                             EXP_TBL[e2].name, s_exp_post_n[e2]);
            if (w <= 0) break;
            o += (w < room) ? w : room - 1;
        }
        ESP_LOGI(TAG, "%s", lg);
    }
}

/* ★B6 冷却：exp_cd0..3 = 各地「最早可再出发日」（日序）。没存过 = 0 = 随时可去。 */
int exp_cd_get(int ed)      { char k[24]; snprintf(k, sizeof(k), "exp_cd%d", ed); return loot_nvs_i32(k, 0); }

static void exp_cd_set(int ed, int v) { char k[24]; snprintf(k, sizeof(k), "exp_cd%d", ed); loot_nvs_set_i32(k, v); }

static bool exp_bag_ready(void)            /* 远行行囊：三件任一在包里 */
{
    return loot_is_carried(LOOT_I_POCKET_WATCH) || loot_is_carried(LOOT_I_ATLAS) ||
           loot_is_carried(LOOT_I_TRAVEL_HAT);
}

/* ★★R115（用户拍板「方案 A」）：**远行资格的唯一判据**。
   以前三处调用点各算一套口径，结果互相打脸：
     · 列表页「可去/缺件」= 护照 + 行囊 + 冷却（不含信物）
     · 自动出门           = 信物 + 冷却（不含护照/行囊）
     · 一键远行           = 什么都不查
   ⇒ 列表页写着「缺件」却能出发、写着「可去」却走不了。现在三处全部走这里。
   条件 = 护照在身 + 行囊三件任一在身 + 该站信物在身 + 该站环境件在身（★新增）+ 不在冷却。
   ★环境件的**豁免**：exp_cnt>0（去过）的站不再要求环境件 —— 老进度里「只靠信物去过」的站，
     不该因为多了一条新门槛就突然去不了（用户 2026-11-01 定的口径，防挫折感）。 */

/* 缺什么（0 = 可去）。判据只写一遍，UI 想显示「缺哪件」时调这里，别在外面再抄一份条件。 */
int exp_lack(int ed, int today)
{
    if (ed < 0 || ed >= EXP_N)                 return EXP_LACK_BAD;
    if (today < exp_cd_get(ed))                return EXP_LACK_CD;      /* 冷却中 */
    if (!loot_is_carried(LOOT_I_PASSPORT))     return EXP_LACK_PASSPORT;
    if (!exp_bag_ready())                      return EXP_LACK_BAG;     /* 远行行囊 ≥1 件 */
    const exp_dest_t *t = &EXP_TBL[ed];
    if (!loot_is_carried(t->token))            return EXP_LACK_TOKEN;   /* 信物（一直是硬门槛） */
    if (exp_cnt_get(ed) == 0 && !loot_is_carried(t->env))
        return EXP_LACK_ENV;                                            /* ★环境件：首去必须带上 */
    return EXP_LACK_NONE;
}

bool exp_can_go(int ed, int today) { return exp_lack(ed, today) == EXP_LACK_NONE; }

int exp_pick_ready(void)
{
    /* ★2026-10-27 机制甲（用户拍板）：**全站加权抽签** —— 原来按 EXP_TBL 顺序逐站掷骰、首个命中即走，
       导致后段目的地（印度…）几乎去不成、顺序偏向严重。现改为：先收集所有「可去」站，
       再按权重抽签，顺序不再影响结果。
       权重 = 基础(EXP_W_BASE) + 气候/首去倾向(EXP_W_CLIMATE) + 通行档助力(EXP_W_BASE/10)。 */
    const int today = exp_day_of(g_app.st.now_local);
    int idx[EXP_N]; int w[EXP_N]; int n = 0; int total = 0;
    for (int ed = 0; ed < EXP_N; ++ed) {
        if (!exp_can_go(ed, today)) continue;      /* ★R115：与列表页/一键远行同一判据 */
        const exp_dest_t *t = &EXP_TBL[ed];
        char eck[20]; snprintf(eck, sizeof(eck), "exp_cnt%d", ed);      /* 首次去（exp_cnt<ed> 计数键）*/
        const bool first = loot_nvs_i32(eck, 0) == 0;
        int weight = EXP_W_BASE;                                  /* 每个可去站的基础权重 */
        /* ★R115 说明：环境件升格成硬门槛后，进池的站基本都是「首去且带着环境件」，
           这一项多半恒成立；保留它只为**去过但没拥有环境件**的老存档仍能拿到气候倾向，
           不改变已经进池各站的相对关系（池内权重仍由通行档拉开）。 */
        if (first || loot_is_owned(t->env)) weight += EXP_W_CLIMATE;  /* 首去/拥有环境件 → 2:1 气候倾向 */
        /* ★2026-10-26 用户令：通行档只是助力（owned 即可，不占格）。 */
        if (loot_is_owned(t->pass)) weight += EXP_W_BASE / 10;        /* 通行档拥有 → +10 助力 */
        idx[n] = ed; w[n] = weight; total += weight; n++;
        ESP_LOGI(TAG, "远行加权 %s：权重 %d", t->name, weight);
    }
    if (n == 0) return -1;                          /* 没有可去站 → 近郊 */
    if (n == 1) return idx[0];                      /* 仅一套可去 → 100% 去（与「更容易去」语义一致）*/
    /* 加权抽签：r 落在 [0,total) 内，按权重分配区间 */
    int r = (int)(esp_random() % (uint32_t)total);
    for (int i = 0; i < n; ++i) {
        r -= w[i];
        if (r < 0) return idx[i];
    }
    return idx[n - 1];
}

/* ★B1「集齐瞬间弹提示」→ 主页立牌一次性显示（出门即清；NVS 标志，渲染幂等） */
void exp_check_toast(void)
{
    if (loot_nvs_i32("exp_toast", 0) || exp_active() >= 0) return;
    if (exp_pick_ready() >= 0) {
        loot_nvs_set_i32("exp_toast", 1);
        ESP_LOGW(TAG, "★★B1 远行套装集齐 → 主页提示已挂（下次出门自动出发）");
    }
}

/* ★B2 自动出发（loot_trip_start_cb 在出门掷骰命中那刻调）：写远行态，返回覆盖小时数 */
int exp_start_trip(int ed, time_t now)
{
    loot_nvs_set_i32("exp_dest", ed);
    loot_nvs_set_i32("exp_start", (int32_t)now);
    loot_nvs_set_i32("exp_mail_day", exp_day_of(now));   /* 出发当日不发，明日起掷 */
    loot_nvs_set_i32("exp_toast", 0);
    loot_nvs_set_i32("trip_dest", LOOT_SER_ABROAD);      /* 防普通回家结算误读旧去向 */
    ESP_LOGW(TAG, "★★B2 自动出发：%s（%d 天，第 %d 日启程）",
             EXP_TBL[ed].name, EXP_TBL[ed].days, exp_day_of(now));
    return (int)EXP_TBL[ed].days * 24;                   /* net_sync 用它覆盖 out_ts */
}

void exp_day_mail(int ed, time_t now)
{
    exp_post_index_build();
    const int today = exp_day_of(now);
    int last = loot_nvs_i32("exp_mail_day", today);
    if (last > today) {              /* ★R144：校时回拨 / 脏值 → 自愈，否则循环不执行且永不写回 */
        last = today;
        loot_nvs_set_i32("exp_mail_day", today);
    }
    const int d0    = loot_nvs_i32("exp_start", today);      /* 启程日 → 算「第 N 天」用 */
    const bool night = loot_fx_is_night(now);
    const bool rain  = loot_fx_is_rain(g_app.st.weather);
    const int  pp    = mail_pp_total(night);
    /* ★2026-10-23 用户令：远行逐日寄信基准 45→20（原无藏品期望≈0.7 张/天、常几天收不到一张；
       调后≈0.95，配合心情/藏品稳上 1，符合「基本一天一张」）。
       ★★R113 重写口径：把「no0 = 20 − pp」显式展开成「寄出概率 p_send = 80 + pp」，
       再乘「积压未读保留率」—— keep=100 时与旧式**完全等价**，不改变手感。 */
    int p_send = 80 + pp;                                    /* 改动前的寄出概率 */
    if (p_send > 100) p_send = 100;                          /* pp>20 时旧式靠 no0 夹 0 天然封顶 */
    {
        const int keep = mail_backlog_keep_pct();             /* ★R113 积压未读梯度惩罚 */
        const int raw  = p_send;
        p_send = p_send * keep / 100;
        if (keep < 100)
            ESP_LOGI(TAG, "远行寄信：积压未读 %d 封 → 保留 %d%%（日寄出 %d%% → %d%%）",
                     post_unread(), keep, raw, p_send);
    }
    if (p_send < 0) p_send = 0;
    const int no0 = 100 - p_send;                            /* 换回「不寄」的百分点 */
    /* ★★R113 修「名义 105pp 实到 20pp」：pp ≤ 20 影响的是「寄不寄」；pp > 20 的部分
       改为**提高「一趟 2 张」的比例**。改动前 pp≥20 之后 no0 恒夹 0，pp 从 20 堆到 45
       完全没有收益 —— 那 25pp 是白给的。
       ⚠ 方向别搞反：下面 cnt 的判定是 `r < no0 ? 0 : (r < two_lo ? 1 : 2)`，
         所以 **two_lo 越小 → 2 张区间 [two_lo,100) 越大**。抬 2 张概率要**减**。 */
    int two_lo = 85 - ((pp > 20) ? (pp - 20) : 0);
    /* ★R120（用户令 2026-11-02）：封底 60 → **40**。旧封底让 pp≥45 之后完全没收益
       （mail_pp 聚合上限 60 ⇒ 45~60 那 15pp 白给）。新口径下 pp 越高「一趟 2 张」比例继续涨：
       pp=45 → 2 张 40%（与旧版同）· pp=60（聚合上限）→ two_lo=45 → 2 张 55%。
       ★R123（用户令 2026-11-03，第四点）：封底 40 → **20**。
       mail_pp 的**实际聚合上限是 87**（fx 60 + 心情最多 12 + 飞鸟帖 15），R120 只按 60 算
       ⇒ pp 65~87 那 22pp 仍白给。新封底下 pp 一路有效到 ≈85：
       pp=65 → two_lo=40（与 R120 同）· pp=85 → 20（2 张 80%）。 */
    if (two_lo < 20) two_lo = 20;
    /* ★ 积压未读时 no0 会冲到 85 以上（≥10 封时 no0=94）。若 two_lo 仍停在 60，
       区间 [60,94) 已被 no0 判成「0 张」⇒ 落到「寄出」的样本**全部**是 2 张。
       把 two_lo 顶到 no0 之上，保住「积压时即便寄、也是寄 1 张」的手感。 */
    if (two_lo < no0 + 8) two_lo = no0 + 8;
    if (two_lo > 99) two_lo = 99;
    if (two_lo <= no0) two_lo = no0 + 1;                     /* 兜底：区间恒非空 */
    const bool force = mail_force_hit(LOOT_SER_ABROAD, night, rain,
                                      g_app.st.weather == WX_CLEAR);
    bool sent = false;
    /* ★R157（用户口径）：远行开了 API 就优先出 AI 片，但仍保留预制片、比例较低。
       api_live_fill 失败（接口不通 / 已熔断）当场回落预制片＝「当作没开」。
       预制片抽完 → 100% AI，不让玩家收不到新片。 */
    const bool all_got = (post_ungot_count() == 0);
    for (int d = last + 1; d <= today; ++d) {
        int day_no = d - d0 + 1;                 /* ★2026-10-23 修：clock 回拨/异常时夹下界，防"第 N 天"≤0 进日志与 API */
        if (day_no < 1) day_no = 1;
        const uint32_t r = esp_random() % 100u;
        int cnt = ((int)r < no0) ? 0 : (r < two_lo) ? 1 : 2;
        if (force && cnt == 0) cnt = 1;
        if (cnt > 2) cnt = 2;
        for (int k = 0; k < cnt; ++k) {
            const bool pool_empty = (s_exp_post_n[ed] <= 0);
            const int ai_pct = (pool_empty || all_got) ? 100 : 70;
            if (g_api_on && g_api_key[0] &&
                ((int)(esp_random() % 100u) < ai_pct) &&
                api_live_fill(ed, day_no, now)) {
                sent = true;
                jtrip_note_mail(ed, day_no, API_POST_IDX, api_live_title());  /* ★R131：AI 片也记（标题快照） */
                { int ni = ws_intimacy_add_auto(2); if (ni >= 0) g_app.st.intimacy = ni; }
                continue;
            }
            const int n = s_exp_post_n[ed];
            if (n <= 0) break;
            int a = s_exp_post_idx[ed][esp_random() % (unsigned)n];   /* 组内随机，偏开已收到的 */
            if (post_is_got(a) && n > 1) a = s_exp_post_idx[ed][esp_random() % (unsigned)n];
            /* ★★2026-11-10 用户令：邮政等级（特快 15% / 普快 70% / 慢递 15%）——
               远行口径：特快 = 当天多送一张；慢递 = 压 2 天（到期由 post_slow_tick 投放）。 */
            const int cls = post_mail_cls_roll();
            post_mail_cls_set(a, cls);
            bool in_transit = false;
            if (cls == POST_CLS_SLOW && post_slow_enqueue(a, d + POST_SLOW_DELAY)) {
                /* 已入在途：此刻不算收到，等 post_slow_tick 投（本函数末尾会 tick 一次）*/
                in_transit = true;
            } else {
                if (cls == POST_CLS_SLOW) post_mail_cls_set(a, POST_CLS_STD);
                post_set_got(a);
                post_arr_set(a, d);                     /* ★2026-10-22 记到达日（d=补掷对应当天） */
            }
            jtrip_note_mail(ed, day_no, a, NULL);  /* ★R131：这一趟第 N 天寄了哪张 */
            /* ★2026-11-12 修：慢递片还没到，亲密不提前发；到期由 post_slow_tick() 结 +3
               （基础 2 + 等得值 1，见 post_system.c:1288 规格）。入队失败降级当场投的照旧 +2。 */
            if (!in_transit) { int ni = ws_intimacy_add_auto(2); if (ni >= 0) g_app.st.intimacy = ni; }   /* ★R125：回写 RAM，防深睡覆写 */
            sent = true;
            if (cls == POST_CLS_EXP && cnt < 3) ++cnt;  /* 特快白送一张：本轮多跑一次（封顶 3）*/
            ESP_LOGW(TAG, "★★B3 远行明信片寄到：「%s」（%s 第%d天 · %s%s）",
                     g_posts[a].title, EXP_TBL[ed].name, day_no,
                     post_mail_cls_name(post_mail_cls(a)),
                     cls == POST_CLS_SLOW ? "· 在路上" : "");
        }
    }
    if (today > last) {
        loot_nvs_set_i32("exp_mail_day", today);
        if (sent) post_save();
    }
    /* ★2026-11-10：补掷跨了多天时，慢递可能已经到点 —— 尾巴上统一投一次（幂等）。 */
    post_slow_tick(today);
}

/* ★B5 归来结算 + ★B6 冷却（loot_trip_return_cb 开头调；之后照常走公共回家 +3 / 里程碑） */
void exp_return_settle(int ed, int *granted, int *coin_gain)
{
    /* ★2026-10-22：这一批发件的「所在地」= 本次远行目的地（纪念品详情要显示"在哪获得"）。
       函数末尾复位成 -1（家），否则后续在家获得的件会被误记成异国。 */
    const int prev_dest = s_grant_dest;
    s_grant_dest = ed;
    /* ★R213（用户令）：召回币「立即回家」这趟猫**并没有真的走到目的地** ——
       不再按完整归来结算：不给首去专属保底、不掷 4 发大包旧物、不吃「远方来信」补掷，
       也不记旅迹/次数成就、不触发雪花北方渠道。贝币的 1/3 削减仍在 loot_trip_return_cb。
       标志由 ws_out_recall 写下，那边在本函数返回后读一次即清。 */
    const bool recall_cut = (loot_nvs_i32("recall_cut", 0) != 0);
    /* 专属藏品：首去必得（100%）/ 再去 60% —— 再去命中就多给两掷旧物当彩蛋 */
    int rolls = recall_cut ? 0 : 4;                    /* ★B5「更大一包旧物」= 4 掷（普通近郊 3 掷） */
    const int sp = EXP_TBL[ed].special;
    if (!recall_cut) {
        if (!loot_is_owned(sp)) {
            if (loot_grant(sp)) (*granted)++;
        } else if ((int)(esp_random() % 100u) < 60) {
            rolls += 2;
        }
    }
    for (int roll = 0; roll < rolls; ++roll) {         /* 旧物从三个近郊系列随机抽（走折算） */
        const int d = loot_roll_drop((int)(esp_random() % 3u));
        if (d < 0) continue;
        if (loot_grant(d)) { (*granted)++; continue; }
        *coin_gain += coin_value_of(d) * loot_fx_coin_pct() / 100;
    }
    /* ★E 联动：远方来信（海星+旧邮票）远行归来必定带回 2 件收藏品 —— 兜底补掷 */
    if (!recall_cut && loot_is_owned(LOOT_I_STARFISH) && loot_is_owned(LOOT_I_STAMP))
        for (int t = 0; *granted < 2 && t < 8; ++t) {
            const int d = loot_roll_drop((int)(esp_random() % 3u));
            if (d >= 0 && loot_grant(d)) (*granted)++;
        }
    char k[24]; snprintf(k, sizeof(k), "exp_cnt%d", ed);
    if (!recall_cut) loot_cnt_add(k, 1);               /* 成就记一笔（E 批取数）；召回不算到过 */
    /* ★R125（用户令 2026-11-03）：雪花原来只在「雪天+山野」必带，无雪城市永远拿不到
       （连带「四季宁静」「静心三件」不可达）。补一条北方渠道：冰岛/京都远行归来
       25% 带回雪花（已拥有则不再给，一次性件）。 */
    if (!recall_cut && (ed == 0 || ed == 1) && !loot_is_owned(LOOT_I_SNOW) &&
        (int)(esp_random() % 100u) < 25) {
        if (loot_grant(LOOT_I_SNOW)) {
            (*granted)++;
            ESP_LOGW(TAG, "★★ %s之行带回「雪花结晶」（北方城市渠道）", EXP_TBL[ed].name);
        }
    }
    if (!recall_cut) map_note_exp(ed);                 /* ★F 旅迹记账（召回不记） */
    exp_cd_set(ed, exp_day_of(g_app.st.now_local) + EXP_CD_DAYS);
    loot_nvs_set_i32("exp_dest", -1);
    ESP_LOGW(TAG, "★★B5 远行归来：%s（冷却到第 %d 日 · 成就累计 %d）%s",
             EXP_TBL[ed].name, exp_cd_get(ed), loot_cnt_get(k),
             recall_cut ? " —— 召回立即回家：未按完整归来结算" : "");
    s_grant_dest = prev_dest;                          /* ★恢复「家」口径 */
}

int exp_cnt_get(int ed)     { char k[24]; snprintf(k, sizeof(k), "exp_cnt%d", ed);  return loot_nvs_i32(k, 0); }

int exp_last_get(int ed)    { char k[24]; snprintf(k, sizeof(k), "exp_last%d", ed); return loot_nvs_i32(k, 0); }
