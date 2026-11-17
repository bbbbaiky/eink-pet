/* exp_system.h —— 远行系统（套装判定/链式掷骰/旅途发信/归乡结算/纪念池） 公开接口（重构 2a 生成） */
#ifndef EXP_SYSTEM_H
#define EXP_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
#include "loot_data.h"

/* ★R115：远行缺件原因（exp_lack 的返回值）。判据只在 exp_system.c 里写一遍，
   UI 想显示「缺哪件」时读这里，别在外面再抄一份条件。 */
#define EXP_LACK_NONE      0   /* 可去 */
#define EXP_LACK_BAD       1   /* 目的地下标越界（内部错误） */
#define EXP_LACK_CD        2   /* 该站冷却中 */
#define EXP_LACK_PASSPORT  3   /* 护照没带 */
#define EXP_LACK_BAG       4   /* 远行行囊（旧怀表/地图册/旅行帽）一件都没带 */
#define EXP_LACK_TOKEN     5   /* 该站信物没带 */
#define EXP_LACK_ENV       6   /* 该站环境件没带（仅首去要求） */

bool exp_can_go(int ed, int today);   /* ★R115：== exp_lack() == NONE */
int  exp_lack(int ed, int today);     /* ★R115：远行资格唯一判据（返回 EXP_LACK_*） */
int exp_active(void);
int exp_cd_get(int ed);
int exp_cnt_get(int ed);
int exp_day_of(time_t now);
int exp_last_get(int ed);
int exp_pick_ready(void);
int exp_start_trip(int ed, time_t now);
void exp_day_mail(int ed, time_t now);
void exp_post_index_build(void);
void exp_return_settle(int ed, int *granted, int *coin_gain);
void exp_souv_load(void);

#endif
