/* ═══════════════════════════════════════════════════════════════════════════
 * friend_net.h — 好友跨设备链路 v1（免注册公共 MQTT · 2026-10-24）
 * -----------------------------------------------------------------------------
 * 背景：friend.c 是纯本地数据层（好友码+好友表，NVS）；本模块补上「设备间通道」。
 * 方案（用户拍板方向：免注册、不要自建后端、跨 WiFi）：
 *   · 服务器 = EMQX 免注册公共 broker（国内接入点 broker-cn.emqx.io:1883；
 *     海外备用 broker.emqx.io。巴法云 bemfa 要注册私钥，不用）。
 *   · 会合键 = 6 位好友码。每台设备把猫的状态以 **retained** 消息挂在
 *     `petcat/v1/<自己的码>/state`；每次联网同步收尾时：
 *       连 broker → 发布自己状态 → 逐个订阅好友 topic（retained 秒回对方最后状态）
 *       → 收 4 秒 → 断开。⇒ 双方**不需要同时在线**，跨 WiFi（各自连自家路由）。
 *   · payload v1 = `v1|<mood>|<intim>|<day>`（day = 本地日序；不含昵称/隐私）。
 *   · 对方状态落 NVS petns: fn<i>_md（心情档）/ fn<i>_dy（最后在线日序），
 *     i = friend 表下标；UI 读 friend_net_state_of()。
 * 安全备忘：公共 broker 无鉴权，6 位码 ≈ 12 亿空间、payload 只有心情档位 ——
 *   v1 够用；将来若挂更敏感内容，换巴法云私钥或自建 broker 再升级。
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef FRIEND_NET_H
#define FRIEND_NET_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* 每轮同步前调：塞入自己的可发布状态（main.c 的心情档/亲密度/本地日序）。
   ★R121：多传一个 now_local —— 串门信号的「剩余窗口」与收件侧到期时刻都要用同一套钟。 */
void friend_net_set_self(int mood, int intim, int day, int vis, time_t now_local);   /* vis: 0 在家 1 出门 2 远行 */

/* ★R128：只更新本地时钟（主页每帧调一次）。
   为什么必须有：s_now_local 是普通静态，深睡唤醒复位后归 0；而 `fn<i>_vend` 到期判断
   依赖它 ⇒ 若只在同步时更新，唤醒后 0 >= vend 恒假，串门猫/告示牌会无限驻留。 */
void friend_net_set_now(time_t now_local);

/* 批量同步一次（连-发-收-断，最坏 ~12s）。没有任何好友时直接返回 false，
   不会动 WiFi；broker 连不上也静默失败（绝不让好友链路挡住正常同步）。
   返回 true = 本轮至少收到一位好友的状态。 */
bool friend_net_sync(void);

/* 好友 i（friend 表下标）的远端状态；从未收到过返回 false */
/* ★R150：friend_net_state_of() 已删（全工程零调用，链接期早已被 gc）。*/
/* ★R121：对方猫的动态（0 在家 / 1 串门中 / 2 远行）；没收到过 = -1。
   串门信号写下的「有效期内」也返回 1 —— 到期后自动按 0 处理。 */
bool friend_net_vis_of(int i, int *vis);

/* ── 对方猫名（state v3 · 2026-10-27）────────────────────────────────
 * 背景：用户要串门告示牌上写**对方猫的名字**，可 v2 只有 mood/intim/day/vis。
 * v3 payload = `v3|<mood>|<intim>|<day>|<vis>|<pet_name>`（末段可缺省，向后兼容 v1/v2）。
 * 发：每轮同步前 friend_net_set_self_name() 塞入自己的猫名；名字里的 '|' 会被剔掉。
 * 读：friend_net_name_of() 取最近一次收到的对方猫名（没收到 = false）。 */
void friend_net_set_self_name(const char *name);
bool friend_net_name_of(int i, char *out, int cap);

/* ── 留言 v1（信箱模式）──────────────────────────────────────────────
 * 发：★R150 起 send_msg 已删 —— 发留言走 friend_net_send_post()/串门那条短连接；
 *     v1 的 `friend_net_send_msg()` 全工程零调用，链接期早被 gc。
 * 收：同步时自动收（收到即存 NVS + 回发空 payload 清信箱），不调接口。
 * 读：unread 判未读；★R151 起 mark_read 由 go_page(PG_FRIEND_ITEM) 在进好友详情时
 *     调用清未读（msg_of 仍不恢复 —— 留言正文没有上屏路径）。 */
bool friend_net_msg_unread(int i);

/* ★★ R92（2026-10-27）：好友表变动后作废本地状态缓存（fn<i>_*）。
   one  = 只作废第 i 条（改码/换人时用）
   from = 作废 i 及之后所有（删好友后下标整体前移时用，防「张三留言挂到李四名下」） */
void friend_net_forget_one(int i);
void friend_net_forget_from(int from);
void friend_net_msg_mark_read(int i);    /* ★R151：恢复 —— 清 fnN_mr 未读位 */

/* ── 明信片转发 v1（2026-10-24 夜）──────────────────────────────────
 * 发：send_post(好友, photo2268, rest, restlen) → retained 挂到对方
 *     petcat/v1/<码>/post（payload = `p1|<day>|` + photo 原始 2268B + mypost 字段块
 *     [tl][text][tal][title][aal][addr]，二进制安全，~2.4KB）。
 * 收：同步时自动收 → 回调 post_cb（main.c 侧装进 mypost_add，标题=转发者昵称）
 *     → 回发空 payload 清信箱。 */
typedef int (*friend_net_post_fn)(const uint8_t *photo, const uint8_t *rest, int restlen, int day);
void friend_net_set_post_cb(friend_net_post_fn fn);
int  friend_net_send_post(int friend_idx, const uint8_t *photo, const uint8_t *rest, int restlen);

/* ── 串门主动触发（2026-10-27 测试入口 · ★R121 语义改）───────────────
 * 发：send_visit(好友, lvl) → retained 挂到自己的码下 `.../visit`。
 *     payload = `w1|<lvl>` 或 `w1|<lvl>|<剩余小时>` —— 剩余小时按本机真实串门窗口算
 *     （不在窗口内传 0 ⇒ 收件侧按 VISIT_MAX_H=8h 兜底）。
 * 收：同步时订阅 /visit → **直接写对方 `fn<i>_vis=1` + 到期时刻**，主页立刻显示串门猫
 *     与告示牌，直到到期或对方下次上报别的状态。原「3 秒全屏场景页」已按用户令去掉。
 * ⚠ 与明信片链路完全独立。 */
int  friend_net_send_visit(int friend_idx, int lvl);

/* ★R121：本机模拟「好友 i 的猫来串门」（测试页「本地预览」用，不走网络）。
   hours <= 0 → 按 8 小时兜底。 */
void friend_net_local_visit(int i, int hours);

/* ★R121：后台同步收到串门信号 → 置「主页待重绘」标志；主循环取走即清
   （跨任务只递标志，绝不在后台任务里碰 UI）。 */
bool friend_net_take_home_dirty(void);

#endif /* FRIEND_NET_H */
