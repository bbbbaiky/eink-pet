/* ══════════════════════════════════════════════════════════════════════
 * friend.h —— 朋友功能的数据层（好友码 + 好友表，全部存 NVS，不联网）
 *
 * 设计口径（2026-10-22 与用户对齐）：
 *   · 每台设备首次开机生成 **6 位好友码**（Crockford 风格字母表，去掉 I/L/O 防混淆）
 *   · 加好友 = 对方手机扫我的二维码 → 打开**对方设备**的添加页 → 存进对方 NVS
 *   · 好友表最多 20 人；昵称由添加方填写（首版不从网络取）
 *   · 本层不含 MQTT / 串门协议，那属于下一批
 * ══════════════════════════════════════════════════════════════════════ */
#ifndef FRIEND_H
#define FRIEND_H

#include <stdbool.h>

#define FRIEND_MAX       20
#define FRIEND_CODE_LEN  6

typedef struct {
    char code[8];           /* 6 位 + NUL */
    char name[20];          /* UTF-8 昵称（≤18B 可见字符 + NUL）*/
} friend_info_t;

/* 开机装载：读/生成自己的好友码，载入好友表（NVS petns） */
void friend_init(void);

const char *friend_my_code(void);                 /* 6 位好友码 */
int         friend_count(void);
const friend_info_t *friend_at(int i);            /* i < friend_count() */

/* 加好友：成功返回下标，已存在返回其下标，失败/满返回 -1 */
int  friend_add(const char *code, const char *name);
/* ★R150：friend_exists() 已删（全工程零调用；重复添加的判重就在 friend_add 内部）。*/

/* ★★ R92（2026-10-27 用户令）：好友详情页要能删 / 能改。
   del  = 删掉第 i 位（后面整体前移 + NVS 整表重写 + 作废该位及之后的 fn 缓存）。
   update = 改第 i 位的码/名（码不得与**其它**好友重复；换人 ⇒ 作废该位 fn 缓存）。
   两者越界/参数非法/写 NVS 失败都返回 false，内存态保持原样。 */
bool friend_del(int i);
bool friend_update(int i, const char *code, const char *name);

/* 编辑目标：>=0 时「添加朋友」热点页的网页会预填该好友的码与名，提交走 update 而不是 add。
   （设备上只有两个按键，6 位码 + 昵称没法在设备上敲 ⇒ 改也必须开热点用手机网页改） */
void friend_edit_begin(int i);     /* 设编辑目标（-1 = 普通新增） */
int  friend_edit_target(void);     /* 当前编辑目标，-1 = 新增模式 */

/* ★2026-10-26 重构第 3 步：添加朋友热点 + 二维码页左右两屏状态（自 main.c 搬入 friend.c） */
void friend_portal_start(void);
void friend_portal_stop(void);
void friend_pane_set(int add);        /* 1 = 添加朋友屏（开热点）/ 0 = 我的二维码屏（收热点） */
bool friend_pane_is_add(void);        /* 当前是否在「添加朋友」屏 */
bool friend_pane_portal_open(void);   /* 热点是否已开 */

#endif /* FRIEND_H */
