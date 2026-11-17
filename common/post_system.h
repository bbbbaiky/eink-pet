/* post_system.h —— 明信片系统（常规来信/笔友/我的收藏/照片明信片/未读位图） 公开接口（重构 2a 生成） */
#ifndef POST_SYSTEM_H
#define POST_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
#include "loot_data.h"
#include "main_api.h"

bool mypost_del(int e);
bool mypost_entry_unread(int e);
bool mypost_flip(void);
bool mypost_layout(void);
bool mypost_open_sel(void);
void mypost_mark_read_sel(void);   /* ★R155：进详情清未读，供触屏/收藏夹路径共用 */
bool mypost_read_row(int e, int y, uint8_t *dst);
bool mypost_sd_del(int idx);
bool mypost_text_page(int x, int y0, const char *s, int start_line);
bool post_is_got(int i);
bool post_is_read(int i);
const char *mypost_source_name(int e);
/* ★R189：卡片来源（meta[8]）。0=手机相册 / 1=AI生成 / 2=朋友转寄（预留）。
   写入方 = mypost_src_hint 在落箱前声明一次（API 生图通道置 1）。 */
void mypost_src_hint(uint8_t src);
int  mypost_read_src(int k);
const char *post_date_disp(int i, char *buf, int cap);
const char *post_sender(int i);
const post_t *post_at(int i);
int mypost_add(const uint8_t *data, uint32_t len);
int mypost_count(void);
int mypost_entry_by_ts(uint32_t ts);
int mypost_entry_rank(int e);
int mypost_rank_entry(int rank);
int mypost_read_text(int e, char *dst, int cap);
int mypost_read_title(int k, char *dst, int cap);
int mypost_read_sender(int k, char *dst, int cap);   /* ★2026-10-27 转发者名字 */
int mypost_text_screens(const char *s);
int mypost_unread(void);
int post_back_screens(const char *note);
int post_cat_cnt(int c);
int post_cat_unread(int c);
int post_disp_of(int raw);
int post_pick_base(int dest);       /* ★2026-10-26 bug②：按去向（LOOT_SER_*）抽一张基础明信片；-1 = 该去向不适用过滤 */
int post_pick_same_city(const char *city);   /* ★R75：同一城市的近郊片（优先未收到）；-1 = 该城没有 */
int post_pick_penpal(void);                  /* ★R218：未收到的笔友来信（小鸟/蝴蝶邮局）；-1 = 无 */
const char *post_dest_name(int dest);
int post_got_cnt(void);
int post_order(int k);
int post_recv_cb(const uint8_t *data, uint32_t len);
int post_sender_kind(int i);
/* ★2026-11-10：背面左上那格（邮编框下）= 寄件来源邮局名（猫猫邮局 / 小鸟邮局 / 蝴蝶邮局）。 */
const char *post_sender_post_name(int i);
/* ★2026-11-10 邮政等级（特快/普快/慢递）：0=普快 1=特快 2=慢递。见 post_system.c 的机制说明。 */
#define POST_CLS_STD    0
#define POST_CLS_EXP    1
#define POST_CLS_SLOW   2
/* ★2026-11-12：慢递「压几天」的**唯一真源**。以前只在 post_system.c 里私有定义
   却没人用、两处入队各自硬编码 +2 ⇒ 改口径必漏一处。入队侧与队列侧都认它。
   ★2026-11-12 用户令：压 2 天 → **1 天**（今天寄、明天到），只改这一个数即可。 */
#define POST_SLOW_DELAY 1
int  post_mail_cls(int i);
void post_mail_cls_set(int i, int c);
const char *post_mail_cls_name(int c);
int  post_mail_cls_roll(void);
bool post_slow_enqueue(int idx, int due_day);   /* 慢递入在途队列（NVS，掉电不丢）*/
int  post_slow_tick(int today);                 /* 把到期的慢递投进信箱（幂等）*/
int  post_slow_pending(void);                   /* 在途条数 */
extern uint8_t s_post_cls[];                    /* 每张片的等级（NVS blob "post_cls"）*/
int post_unread(void);
int post_view_i(void);
uint32_t mypost_entry_ts(int e);
void mypost_clock_set(uint32_t now_local);
void mypost_cnt_refresh(void);
void mypost_enter(void);
void mypost_fwd_begin(void);
void mypost_fwd_send(void);
void mypost_inv_inside(int x0, int y0, int x1, int y1);
void mypost_load(void);
void mypost_meta_line(int k, int tlen, bool is_front);
void mypost_notice_tick(void);
void mypost_sd_close(void);
void mypost_sd_scan(void);
void mypost_unrd_set(int e, bool v);
void post_cat_pick(int c);
void post_enter(void);
void post_flip(void);
void post_flip_back(void);
void post_goto(int delta);
void post_hold_tick(int x, int y);
void post_import_cancel(void);
void post_load(void);
void post_mark_read(int i);
void post_mark_unread(int i);
void post_save(void);
void post_set_got(int i);

#endif
