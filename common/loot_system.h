/* loot_system.h —— 收藏品系统（拥有/携带/分类/堆/获得记录/掉落掷骰/使用消耗） 公开接口（重构 2a 生成） */
#ifndef LOOT_SYSTEM_H
#define LOOT_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
#include "loot_data.h"

bool loot_alive(int i);
bool loot_carry_set(int i, bool on);
bool loot_got_get(int i, int *day, int *place);
bool loot_grant(int i);
bool loot_is_carried(int i);
bool loot_is_owned(int i);
bool loot_is_read(int i);
bool loot_is_usable(int i);
bool loot_nvs_get_blob(const char *key, void *buf, size_t n);
const char *loot_desc_of(int i);
const char *loot_name_of(int i);
const char *loot_note_of(int i);
const uint8_t *loot_icon_load(int idx);
int loot_cat_cnt(int c);
int loot_cat_of(int i);
int loot_cat_unread(int c);
int loot_cnt_add(const char *key, int delta);
int loot_cnt_get(const char *key);
int loot_disp_of(int item);
int loot_nvs_i32(const char *key, int32_t defv);
int loot_order(int k);
int loot_owned_cnt(void);
int loot_roll_drop(int dest);
int loot_stack_cnt(int k);
int loot_stack_of(int i);
int loot_trip_plan_cb(time_t out_at, int weather);
int loot_trip_start_cb(time_t out_at, int weather);
int loot_unread(void);
int loot_stack_unread(int k);   /* ★R204：随身四堆各自的未读数（二级页红点） */
int loot_v_icon(int i);
void loot_bits_save(const char *base, const loot_bits_t *b);
void loot_cat_pick(int c);
void loot_cheat_toggle(void);
/* ★2026-11-12：测试菜单「全部清零」—— 收藏品拥有/已读/携带位全归零并落 NVS */
void loot_clear_all(void);
void loot_consume(int i);
void loot_detail_carry_action(void);
void loot_got_draw(int i);
void loot_got_set(int i, int day, int place);
void loot_load(void);
void loot_mark_read(int i);
void loot_milestone_check(const char *why);
void loot_nvs_set_blob(const char *key, const void *buf, size_t n);
void loot_nvs_set_i32(const char *key, int32_t v);
void loot_stack_pick(int k);
void loot_sync_done_cb(bool ok);
void loot_trip_return_cb(time_t out_at, time_t out_ts);

#endif
