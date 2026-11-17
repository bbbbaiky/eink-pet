/* friend_ui.h —— 朋友/串门三页绘制（实现 common/friend_ui.c）
 * ★2026-10-26 起重构：这一组从 main.c 搬出，main.c 只保留页面分发调用点。 */
#ifndef FRIEND_UI_H
#define FRIEND_UI_H

#include <stdbool.h>

/* 三页绘制（两版共用；页内 #if TOUCH_DEMO 保留原来的返回箭头差异） */
void draw_friend_page(void);        /* PG_FRIEND      好友列表（3 行/页 + 底部「添加朋友」） */
void draw_friend_qr_page(void);     /* PG_FRIEND_QR   我的二维码 / 添加朋友（左右两屏） */
void draw_friend_item_page(void);   /* ★R92 PG_FRIEND_ITEM 好友详情（名字 + 6 位码 + 修改/删除） */

/* ★R92：列表页光标（0..cnt-1 = 某位好友，cnt = 底部「添加朋友」，-1 = 无光标/触屏进页）*/
extern int g_fr_sel;
/* ★R92：详情页双按钮选中（-1 = 无选中 / 0 = 修改 / 1 = 删除）*/
extern int g_fr_isel;

/* 串门图测试页（设置菜单第 14 项）：两组手绘素材预览，BOOT/点按切组 */
void draw_test_visit_page(void);
extern int g_tvisit_grp;            /* 预览组号 0/1（main.c 里按键/BTN 切换它） */
extern int g_fr_off;                /* 好友列表分页偏移（翻页在 main.c 侧改） */

/* 主页告示牌：任一好友的猫在外面（vis>0）→ 返回 true 并写出名字 */
bool friend_visit_sign(char *out, int cap);

/* ★2026-10-27 用户令：好友的猫在串门时，**主页房间里的猫**要换成串门那对精灵
   （不是自家猫的姿态）。which 返回正在串门的好友下标（无 = -1）；
   draw_cats 把该好友对应的那组「阴影 + 两只猫」按原坐标落进当前帧。 */
int  friend_visit_which(void);
void friend_visit_draw_cats(int idx);

/* 串门场景触发（原 main.c 主循环里直接改三个静态量，现在收成入口） */

#endif /* FRIEND_UI_H */
