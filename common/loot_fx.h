/* ══════════════════════════════════════════════════════════════════════════════
 * loot_fx.h —— 收藏品效果**聚合核心**（2026-10-16 收拢 · 用户：「把核心收拢」）
 * ==============================================================================
 * 为什么要有这个头
 * ----------------
 * 在本文件出现之前，「哪件收藏品加什么效果」**散落在两个 .c 的好几处**：
 *   · main.c   mood_calm_sec()   里 `if (loot_is_owned(LOOT_I_SHELL)) s -= 180;`
 *   · main.c   出门结算里         `if (loot_is_owned(LOOT_I_ACORN)) extra = …;`
 *   · net_sync.c 自己在 NVS 里**直接读位图**（ns_loot_owned_bit，还只读得到低 32 位）
 * ⇒ 三个后果，每一个都真实踩过：
 *   ① 加一件新收藏品要改 N 处，漏一处就是「详情页写着、实际不生效」；
 *   ② 效果上限（90% / 85% / 48h…）散在各处，没人知道全局功率；
 *   ③ net_sync.c 只读低 32 位 ⇒ 下标 ≥32 的收藏品（本轮 43 件！）**它根本看不见**。
 *
 * 现在的口径（唯一真相）
 * ---------------------
 *   · **一张表** `loot_fx_tbl[]`（在 main.c，紧挨 g_loot[]）：哪件 + 哪条杠杆 + 档位；
 *   · **一次聚合** `loot_fx_eval()`：按拥有位扫表，按杠杆规则加总（带全局上限）；
 *   · **N 个取数** `loot_fx_xxx()`：业务代码只问聚合值，不再自己 if。
 *   加新收藏品 = **只往表里加一行**（外加图标），不用碰任何业务函数。
 *
 * 杠杆只有这 13 条（机制文档 §4.0；挂不上就别写效果）
 * --------------------------------------------------------------------------
 *   CALM 消气 · OUT_PP 出门概率 · TRIP_H 出门时长 · WX 天气门槛 · BACK_PP 带回率
 *   MAIL_PP 明信片率 · MAIL_FORCE 必定寄回 · DECAY_PCT 亲密度衰减 · GAIN_PP 亲密增益
 *   COIN_PCT 贝币折算 · MOOD_FLOOR 心情下限 · DROP_PCT 掉率倍率 · BACK_BONUS 归来旧物加成
 *   （★R111 订正：原写「11 条」且列了 PITY 空箱保底。LFX_PITY 已于 2026-10-27 B4 删除
 *     —— 它是死杠杆，g_loot_fx[] 零行使用、loot_fx_pity() 恒返回 0；其余 13 条与枚举一致。）
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ── 杠杆编号（表里写这个）────────────────────────────────────────────── */
enum {
    LFX_CALM = 0,      /* 消气加速：mag = 省下的秒数（180/60…） */
    LFX_OUT_PP,        /* 出门概率 +百分点 */
    LFX_TRIP_H,        /* 出门时长 +小时 */
    LFX_WX,            /* 天气门槛豁免：mag = 位掩码（1 大雨不拦 / 2 雪天不拦 / 4 雷也不拦） */
    LFX_BACK_PP,       /* 带回东西几率 +百分点 */
    LFX_MAIL_PP,       /* 明信片寄出几率 +百分点 */
    LFX_MAIL_FORCE,    /* 必定寄回：mag = 场景位（1 晴天 / 2 山野 / 4 海滨；夜雨=夜行者成就，在调用点判）*/
    LFX_DECAY_PCT,     /* 亲密度衰减系数：mag = 百分比（100 = 原样，50 = 减半）*/
    LFX_GAIN_PP,       /* 亲密增益 +点数（抚摸/喂食/回家/明信片，scope 区分）*/
    LFX_COIN_PCT,      /* 贝币折算倍率：mag = 百分比（120 = ×1.2）*/
    LFX_MOOD_FLOOR,    /* 心情下限：mag = 1 最差只到「平淡」*/
    LFX_DROP_PCT,      /* 掉率倍率：mag = 百分比，scope = 系列（见 LFX_SC_*）*/
    LFX_BACK_BONUS,    /* 归来旧物加成 +百分点（海星 +20）*/
    LFX_LEVER_N
};

/* ── scope 位（同一杠杆下区分"管哪个场合"）────────────────────────────── */
enum {
    LFX_SC_NONE = 0,
    LFX_SC_CITY = 1u << 0, LFX_SC_WILD = 1u << 1, LFX_SC_SEA = 1u << 2,
    LFX_SC_NIGHT = 1u << 3, LFX_SC_RAIN = 1u << 4, LFX_SC_CLEAR = 1u << 5,
    LFX_SC_HOME = 1u << 6, LFX_SC_FEED = 1u << 7, LFX_SC_PET = 1u << 8,
    LFX_SC_ALL = 1u << 9
};

/* 聚合结果（一次算全，业务按需取）*/
typedef struct { uint8_t item, lever, mag; uint16_t scope; } loot_fx_t;   /* ★2026-10-26 重构 2a 自 main.c 迁入（g_loot_fx[] 表本体仍在 main.c，main_api.h 有 extern） */
typedef struct {
    int calm_save;      /* 消气省下的秒数（取高，不叠加）*/
    int out_pp;         /* 出门概率加点（加总，上限 90-70=20 → 由取数侧夹）*/
    int trip_h;         /* 出门时长 +小时（加总，上限 48）*/
    int back_pp;        /* 带回 +pp（上限 90）*/
    int mail_pp;        /* 寄信 +pp（上限 85）*/
    int mail_force;     /* 必定寄回的场景位（0 = 无）*/
    int decay_pct;      /* 衰减系数（取最小；100 = 原样）*/
    int pet_pp, feed_pp, home_pp;                 /* 亲密增益加点 */
    int coin_pct;       /* 贝币倍率（★R120：主件全额 + 次件半量，封顶 250）*/
    int mood_floor;     /* 心情下限（取最高）*/
    int drop_city, drop_wild, drop_sea;           /* 掉率倍率（★R120：主件全额 + 次件半量，封顶 250；100 = 原样）*/
    int back_bonus;     /* 归来旧物 +pp */
    int wx_mask;        /* 天气豁免位（1 大雨 / 2 雪天 / 4 雷）*/
} loot_fx_sum_t;

/* ── 唯一聚合入口 + 业务取数（实现全在 main.c）───────────────────────────
 * ★★ 2026-10-18（用户拍板「方案甲：场合接线」）：scope 之前**根本没判**（除
 * DROP_PCT/GAIN_PP），「在城里+4小时」实际全天候生效。现在聚合带**场合上下文**：
 * series（去向系列）/ night / rain。WX 与 MAIL_FORCE 的条件编码在 mag 里，
 * 聚合恒收、判定在调用点 —— 其余杠杆一律先过场合匹配再计入。 */
/* ★R113 删 loot_fx_eval() —— 零调用，取数统一走 loot_fx_xxx → loot_fx_eval_ctx。 */
void loot_fx_eval_ctx(loot_fx_sum_t *o, int series, bool night, bool rain);
int  loot_fx_calm_save(bool night, bool rain);
int  loot_fx_out_pp(int series);
int  loot_fx_trip_h(int series);
int  loot_fx_back_pp(int series);
int  loot_fx_mail_pp(bool night);
int  loot_fx_decay_pct(bool night, bool rain);
int  loot_fx_mood_floor(void);
int  loot_fx_coin_pct(void);
int  loot_fx_back_bonus(void);
int  loot_fx_pet_pp(void);
int  loot_fx_feed_pp(void);
int  loot_fx_home_pp(void);     /* 回家一次性增益（暖手炉 +2；回家 +3 基数在调用侧） */
int  loot_fx_wx_mask(void);
int  loot_fx_drop_pct(int series);
int  loot_fx_mail_force_mask(void);
bool loot_fx_is_night(time_t now_local);   /* 夜 = 20:00..04:59（与 ws_period_of 深夜边界同源）*/
bool loot_fx_is_rain(int weather);         /* 雨 = WX_RAIN_L..WX_RAIN_H（小雨/中雨/大雨） */
