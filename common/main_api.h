/* main_api.h —— main.c 与各模块的跨文件符号（重构 2a 生成；机器维护）
 * 共享变量：定义留在 main.c（已摘 static）；模块侧 extern。
 * 借用函数：定义留在 main.c（已摘 static）。
 * ★ 先 include variant.h：本头含 #if TOUCH_DEMO 的 PG 页枚举（变体分叉 #11 迁入）。 */
#ifndef MAIN_API_H
#define MAIN_API_H
#include "variant.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <esp_attr.h>
#include "loot_data.h"
#include "loot_fx.h"
#include "net_sync.h"
#include "post_data.h"
#include "assets_rw.h"     /* ★R186：a_rd32 / assets_region_read 的单一真源 */

/* ══ 变体分叉 #11 · 触屏版 ══ typedef enum { PG_HOME = */
#if TOUCH_DEMO

/* ============================================================================
 * ★★★ 页面状态机 + 食盆（2026-09-19）
 * ----------------------------------------------------------------------------
 * 页面：主页 → （BOOT 点击）功能页 →（POWER）喂食 → 食物详情页 →（POWER）回主页
 *       功能页里还没做的 4 项（明信片 / 猫的状态 / 设置 / 收藏品）→「还在开发中」
 * 键位完全按 spec §1/§2：**长按为松手判定**，分级窗口 2s。
 * ==========================================================================*/
typedef enum {
    PG_HOME = 0,     /* 房间 + 窗 + 猫 + 食盆 */
    PG_FUNC,         /* 功能页（5 项九宫格，选中框跟着走） */
    PG_FOOD,         /* 食物详情页（唯一喂食页，4 种循环） */
    PG_WIP,          /* 还在开发中（Z Labs 居中）★R147：**无入口**（enter_wip 已删），仅为占位保留 */
    PG_PROV,         /* 配网引导（手机填表） */
    PG_PROV_OK,      /* 配网成功 */
    PG_SET,          /* 设置（★2026-09-22 新版 4 行：WIFI管理 / 窗外风景 / 时间 / 关于） */
    /* ★ 2026-09-22 新增（用户新稿）：WIFI 管理三页 */
    PG_WIFI_LIST,    /* WIFI 管理：3 条网名+状态 + 「+添加网络」 */
    PG_WIFI_ITEM,    /* WIFI 项操作：连接 / 删除（两态由 ui.bin 两页提供） */
    PG_WIFI_DEL,     /* 删除成功 */
    PG_WIFI_CONN_OK,   /* ★2026-09-23 连接成功（结果页；3 秒后自动回列表） */
    PG_WIFI_CONN_FAIL, /* ★2026-09-23 连接失败（结果页；3 秒后自动回列表） */
    PG_SYNC_OK,        /* ★2026-10-07 时间页「立即同步」成功（结果页；3 秒后自动回时间页。
                          底图复用连接结果两张、正文文字运行时盖写 —— 见 draw_sync_result_page） */
    PG_SYNC_FAIL,      /* ★2026-10-07 时间页「立即同步」失败（结果页；3 秒后自动回时间页） */
    /* ★★ 2026-09-23：设置页 3 个二级页（用户要求「设置页所有页面都搭出来」） */
    PG_LAND,           /* 窗外风景：BOOT 短按换候选 / POWER 选定落 NVS / BOOT 长按返回设置 */
    PG_TIME,           /* 时间：只读展示 + POWER 立即同步（不能移行 → 无光标） */
    PG_ABOUT,          /* 关于：只读展示（叶子页 → 任意返回键都回设置） */
    /* ★★ 2026-09-23 晚：**功能补全批次** —— 把功能页剩下 3 项（明信片/猫的状态/收藏品）
       和「关机确认 / 添加网络确认 / 低电量 / 没电 / 抚摸」全部接上，不再进「还在开发中」。 */
    PG_POST_LIST,      /* 明信片列表：BOOT 短按移行 / POWER 打开详情 / BOOT 长按回功能页 */
    PG_POST_ITEM,      /* 明信片详情：叶子页（POWER=下一张，长按/点击返回 = 列表） */
    PG_CAT_S1,         /* 猫的状态 组1：BOOT 短按 ↔ 组2 互切；叶子页 */
    PG_CAT_S2,         /* 猫的状态 组2 */
    PG_LOOT_LIST,      /* 收藏品列表 */
    PG_LOOT_ITEM,      /* 收藏品详情：叶子页 */
    PG_RST_CONF,       /* 添加网络确认（已存满 WIFI_MAX 条时才进，两态） */
    PG_RST_ALL,        /* 恢复出厂设置确认（**设置第 2 页**发起，两态：取消/确定） */
    PG_BATT_DEAD,      /* 没电自动关机：刷完 power_off()，画面长留 */
    /* ★★ 2026-10-14 清理：**PG_PET_OK / PG_PET_NO 已删除**（原「抚摸结果 · 接受/拒绝」）。
       来龙去脉：2026-10-10 心情状态机改版时 pet_touch() 就不再跳这两页了
       （接受/拒绝改由**交互动画**表达：M0/M1 播接受类动画，M2/M3 播拒绝类动画），
       只留下 M4「不理你」走文字结果页 PG_PET_M4。
       ⚠ 但当年**只改了跳转，没清下游**：枚举 / page_render 分支 / ui.bin 槽 34/35 /
       出图脚本全都没跟着删 ⇒ 这两页一直是死代码（grep go_page(PG_PET_OK) 零命中），
       却还占着槽位、还被烤进 bin，日文版 diff 审计时又冒出来「有中文没翻译」。
       本次彻底清理：枚举删、渲染分支删、槽位涂白、UI_WIP 改指向槽 0。 */
    PG_PET_M4,         /* ★★ 2026-10-10 新增：抚摸结果 · M4「不理你」（文字结果页，固件自绘） */
    PG_TEST,           /* ★2026-09-25 测试菜单（★2026-09-28 恢复版 5 行：精灵测试/背景图/
                          状态贴片/气泡/合成预览——气泡测试子页保留（用户 09-28 指定），
                          提示带去掉——规则：无提示页才允许一屏 5 行） */
    /* ★★ 2026-09-25 二期：测试子页（用户要求「测试里让我也选一下背景图，选一下天气，
       再和猫的状态结合，选择的页面可以参照现有的」；★2026-09-27 天气选择页整页退役——
       天气在合成预览按 POWER 循环即可，独立选择页无用） */
    PG_TEST_SPR,       /* 精灵测试（原素材测试页：逐个贴 cat.bin 全部猫姿态） */
    PG_TEST_BG,        /* 背景图选择 ★R147：**测试菜单无入口**（菜单 case 0 直进 PG_TEST_SPR），页面留作备用 */
    PG_TEST_PAT,       /* ★2026-09-26 四改：状态贴片**开关列表**（每片独立开关，片10/11 当 switch） */
    PG_TEST_BATT,      /* ★2026-09-26 电量状态子页：正常/低电量/充电中/充满 4 态模拟 */
    PG_TEST_NET,       /* ★flash16 网络状态子页：正常/无网络/无wifi（无网络/无wifi 本就互斥 → 合并进子页） */
    PG_TEST_BUB,       /* ★气泡测试子页（2026-09-28 恢复版保留）：无 + 气泡 4 食物 5 选 1，
                          选定态在合成预览看效果（走新放置表版 fb_draw_bubble） */
    PG_TEST_PERIOD,    /* ★2026-09-29 新增：模拟时段子页 —— 破晓/清晨/正午/傍晚/深夜 5 选 1，
                          只影响测试页（背景图/合成预览）的房间+窗+网点+月相，不落 NVS */
    PG_TEST_COMB,      /* 合成预览：选定背景×天气 + 姿态 + 贴片开关组合，按主页绘制顺序整屏复刻 */
    PG_TEST_ANIM,      /* ★★ 2026-10-09 新增：交互动画测试列表（抽帧动画逐段播放，见 draw_test_anim） */
    PG_TEST_VISIT,     /* ★2026-10-25 基座接线①：串门图测试（两组素材预览/切换，见 draw_test_visit_page） */
    /* ★★ 2026-10-11 R26：背包系统四页（机制文档 §2.1 定稿；触屏在页面末尾追加不影响任何下标） */
    PG_BAG,            /* ★R30 背包 = 可携带物品列表（tap/POWER 直接佩戴/放回） */
    PG_BAG_SWAP,       /* 满 4 替换选择页：列当前 4 件，tap = 换下它并背上新件（→ 二次确认） */
    PG_BAG_SWAP_CONF,  /* 替换二次确认：「带上 X / 换下 Y」（双框 y117..142） */
    PG_BAG_PICK,       /* ★★ 2026-10-12：可携带物品选择页（第一项「无」；选定即写回该槽，无二次确认） */
    /* ★★★ 2026-10-13 新增（用户：「背包点四个槽 → 选择页，点某件先进该件详情页，
       详情页同收藏品详情但无左右切换、无可携带标签；点底部携带/替换按钮立即回列表页」）：
       从 PG_BAG_PICK 点某件进来 → 该件「携带详情页」；底部按钮 = 携带/替换 → 写回目标槽 → 回 PG_BAG_PICK。 */
    PG_BAG_ITEM,
    /* ★★★ 2026-10-13 新增（用户报④「语言切换要改二级列表页」）：
       语言 = 设置页第 1 页第 4 行 → 进**二级列表页**（列所有语言 + 当前语言右「正使用」），
       tap 某语言行 = 就地切换 + 落 NVS + 回设置页（整屏全刷换文案）。
       页面 = 白底自绘（同背包页族：fb_back_arrow + 标题 + 行），不占 ui.bin 槽。 */
    PG_LANG,
    /* ★★C 商店（2026-10-16，机制文档 §8）：详情页样式一页一件 + 购买二次确认。
       本批入口 = 测试菜单「商店」行（功能页格子要动 ui.bin/icon.bin 出图管线，下批上）。 */
    PG_SHOP,
    PG_SHOP_CONF,      /* 购买二次确认（双框：取消/确认，仿 PG_BAG_SWAP_CONF 版式） */
    PG_ACHV,           /* ★E 成就列表（单页 4 行/页 + 底部详情；POWER/tap = 领取） */
    PG_MAP,            /* ★F 旅迹（流水账 2 页：近郊 3 + 远行 4） */
    /* ★★G（2026-10-16）：API 接口页 —— 功能页 case 9 正式入口（替换原 PG_WIP 占位）。
       POWER/tap = 切 API 模式开关；长按 = 回功能页。页面白底自绘（无 ui.bin 槽）。 */
    PG_API,
    /* ★★ 2026-10-17：OTA 线上升级六页（用户拍板「入口按新口径」：
       ① 设置第 2 页「固件升级」行 ② 关于页底部「检查版本」按钮）。
       两版**同值同序**（与触屏/按键其余页一致），几何照原型 p26..31。 */
    PG_OTA_CHECK,      /* 正在检查（转圈；★本地改版：锁输入等结果，无取消） */
    PG_OTA_FOUND,      /* 发现新版本（版本号 + 包大小 + 「安装并升级」按钮） */
    PG_OTA_NEWEST,     /* 已是最新（+ 「返回」按钮） */
    PG_OTA_PROG,       /* 正在写入（进度条；★本地改版：锁输入等结果） */
    PG_OTA_OK,         /* 升级完成（3 秒后自动重启） */
    PG_OTA_FAIL,       /* 升级失败（3 秒自动回设置；原因写正文） */
    /* ★★ 2026-10-19 本地升级改版（放弃在线 OTA）：
       入口先进**选择页**（TF 卡升级 / 网页升级，标准列表两行），再各走各路：
       TF → CHECK（读卡比版本）→ FOUND / NEWEST / FAIL（错误报告）→ PROG → OK；
       网页 → WEB（热点已开 + 网址）→ 手机 /upgrade 上传 → PROG → OK / FAIL。
       两页白底自绘不占 ui.bin 槽；两版同值同序（插在六页之后）。 */
    PG_OTA_SEL,        /* 本地升级选择：TF 卡 / 网页 两行（BOOT 移光标 / POWER 确认） */
    PG_OTA_WEB,        /* 网页升级：热点已开（WiFi 图标借 13 页 + 热点名 + 192.168.4.1） */
    PG_OTA_MODE,       /* ★R218 二级「升级方式」：手动升级（热点传包）/ 在线升级（拉公开源） */
    PG_POST_IMPORT,    /* 照片明信片导入：热点已开，手机连上自动弹导入页（2026-10-20） */
    /* ★★ 2026-10-17：成就**详情页**与领取结果页（用户报「点击后完全打不开二级页面」）。
       流程（原型 p03→p04→p05/p06→p07）：列表点某条 → 详情（达成条件/物品清单/效果
       + 底部按钮）→ 领取 → 结果页（真槽 20 圆勾）→ 回列表。 */
    PG_ACHV_DET,       /* 成就详情（三态按钮：领取 +N 贝币 / 已领取 / 无法领取） */
    PG_ACHV_OK,        /* 领取成功结果页（3 秒自动回列表） */
    /* ★★ 2026-10-18：测试「一键远行」的**二级选目的地页**（用户令：点一键远行后再选一个，
       不要四个平铺在菜单里）。4 行 = EXP_TBL 四站；tap 行/POWER = 立即出发（无视套装/冷却）；
       返回 = 测试菜单。放枚举末尾（PAGE_HINT 断言上界随之 +1）。 */
    PG_TEST_TRIP,
    /* ★★ 2026-10-18 一级**分类页**（用户令「点进去先是一个品类的划分」）：
       收藏品 = 消耗品 / 随身物品 / 功能道具 / 纪念品；
       明信片 = 常规信件 / 笔友来信 / 我的信件（朋友来信等朋友交互那批再接）。
       选一类 → 对应的列表（列表内顺序/计数自动按类过滤）。放枚举末尾。 */
    PG_LOOT_CAT,
    PG_LOOT_STACK,    /* ★2026-10-24 D5：随身物品四堆二级页（城市/山野/海滨/通用） */
    PG_POST_CAT,
    /* ★★2c 2026-10-21：我的照片明信片（mypost）两页 —— 列表 + 详情（正面照片 / 背面文字）。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +2。 */
    PG_MYPOST_LIST,
    PG_MYPOST_ITEM,
    /* ★★2026-10-22 用户令：mypost 满仓提示页（纯告知：190 将满预警 / 满 200 顶掉告知；
       无拒收按钮，自动顶掉）。放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_MYPOST_NOTICE,
    PG_MYPOST_ACT,      /* ★2026-10-22 用户令：详情页操作页（删除 / 转发；转发占位未接线） */
    PG_MYPOST_DEL,      /* ★2026-10-22 用户令：删除二次确认（双选框：BOOT 切换 / POWER 确定 / 长按取消） */
    PG_MYPOST_FWD,      /* ★2026-10-25 A2：转发好友选择页（3 行/页；tap 行/POWER = 发送） */
    PG_FAV_LIST,        /* ★2026-10-22 用户令：我的收藏（分类第 4 项）列表 */
    /* ★★2026-10-22 朋友功能（第一批 · 离线）：好友列表 + 我的二维码。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +2。 */
    PG_FRIEND,          /* 朋友列表（3 行/页 + 底部「我的二维码」） */
    PG_FRIEND_QR,       /* 我的二维码（扫码加好友；底部按钮切「添加朋友」热点模式） */
    PG_FWD_NOFRIEND,    /* ★R76：转发时暂无好友的纯文字提示页（3s 自动回 / 任意键返回） */
    PG_TEST_FRVIS,      /* ★2026-10-27：测试「猫串门」选好友页（本地预览 / 远端触发） */
    /* ★★ R92（2026-10-27 用户令）：好友详情页（名字 + 6 位码 + 双按钮「修改 / 删除」）。
       交互**整套照抄 PG_WIFI_ITEM**：触屏点哪颗哪颗反白并即时执行；按键 BOOT 切选中、POWER 执行。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_FRIEND_ITEM,
    /* ★R131（用户令「旅程点开能看那几天」）：行程详情（按天分页）。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_TRIP_DET,
    /* ★R194（用户令「房间风格要跟窗外风景一样，点了进二级页再选」）：
       设置第 2 页「房间风格」→ 本页列两个候选（默认 / 科技），选定即落 NVS 并回设置页。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_ROOM_STYLE,
} page_t;
#else   /* 按键版 */

/* ============================================================================
 * ★★★ 页面状态机 + 食盆（2026-09-19）
 * ----------------------------------------------------------------------------
 * 页面：主页 → （BOOT 点击）功能页 →（POWER）喂食 → 食物详情页 →（POWER）回主页
 *       功能页里还没做的 4 项（明信片 / 猫的状态 / 设置 / 收藏品）→「还在开发中」
 * 键位完全按 spec §1/§2：**长按为松手判定**，分级窗口 2s。
 * ==========================================================================*/
typedef enum {
    PG_HOME = 0,     /* 房间 + 窗 + 猫 + 食盆 */
    PG_FUNC,         /* 功能页（5 项九宫格，选中框跟着走） */
    PG_FOOD,         /* 食物详情页（唯一喂食页，4 种循环） */
    PG_WIP,          /* 还在开发中（Z Labs 居中）★R147：**无入口**（enter_wip 已删），仅为占位保留 */
    PG_PROV,         /* 配网引导（手机填表） */
    PG_PROV_OK,      /* 配网成功 */
    PG_SET,          /* 设置（★2026-09-22 新版 4 行：WIFI管理 / 窗外风景 / 时间 / 关于） */
    /* ★ 2026-09-22 新增（用户新稿）：WIFI 管理三页 */
    PG_WIFI_LIST,    /* WIFI 管理：3 条网名+状态 + 「+添加网络」 */
    PG_WIFI_ITEM,    /* WIFI 项操作：连接 / 删除（两态由 ui.bin 两页提供） */
    PG_WIFI_DEL,     /* 删除成功 */
    PG_WIFI_CONN_OK,   /* ★2026-09-23 连接成功（结果页；3 秒后自动回列表） */
    PG_WIFI_CONN_FAIL, /* ★2026-09-23 连接失败（结果页；3 秒后自动回列表） */
    /* ★★ 2026-10-08 深夜回灌（触屏版 2026-10-07 批次，按键版此前完全没有）：
       时间页「立即同步」的结果页。底图复用连接结果两张、正文文字运行时盖写
       —— 见 draw_sync_result_page()。回哪去见 result_back_page()（回**时间页**）。 */
    PG_SYNC_OK,        /* 时间页「立即同步」成功（结果页；3 秒后自动回时间页） */
    PG_SYNC_FAIL,      /* 时间页「立即同步」失败（结果页；3 秒后自动回时间页） */
    /* ★★ 2026-09-23：设置页 3 个二级页（用户要求「设置页所有页面都搭出来」） */
    PG_LAND,           /* 窗外风景：BOOT 短按换候选 / POWER 选定落 NVS / BOOT 长按返回设置 */
    PG_TIME,           /* 时间：只读展示 + POWER 立即同步（不能移行 → 无光标） */
    PG_ABOUT,          /* 关于：只读展示（叶子页 → 任意返回键都回设置） */
    /* ★★ 2026-09-23 晚：**功能补全批次** —— 把功能页剩下 3 项（明信片/猫的状态/收藏品）
       和「关机确认 / 添加网络确认 / 低电量 / 没电 / 抚摸」全部接上，不再进「还在开发中」。 */
    PG_POST_LIST,      /* 明信片列表：BOOT 短按移行 / POWER 打开详情 / BOOT 长按回功能页 */
    PG_POST_ITEM,      /* 明信片详情：叶子页（POWER=下一张，长按/点击返回 = 列表） */
    PG_CAT_S1,         /* 猫的状态 组1：BOOT 短按 ↔ 组2 互切；叶子页 */
    PG_CAT_S2,         /* 猫的状态 组2 */
    PG_LOOT_LIST,      /* 收藏品列表 */
    PG_LOOT_ITEM,      /* 收藏品详情：叶子页 */
    PG_RST_CONF,       /* 添加网络确认（已存满 3 条时才进，两态） */
    PG_RST_ALL,        /* 恢复出厂设置确认（**设置第 2 页**发起，两态：取消/确定） */
    PG_BATT_DEAD,      /* 没电自动关机：刷完 power_off()，画面长留 */
    /* ★★ 2026-10-14 清理：PG_PET_OK / PG_PET_NO 已删除（理由见触屏版同位置的详细注释）。 */
    PG_PET_M4,         /* ★★ 2026-10-10 新增：抚摸结果 · M4「不理你」（文字结果页，固件自绘） */
    PG_TEST,           /* ★2026-09-25 测试菜单（★2026-09-28 恢复版 5 行：精灵测试/背景图/
                          状态贴片/气泡/合成预览——气泡测试子页保留（用户 09-28 指定），
                          提示带去掉——规则：无提示页才允许一屏 5 行） */
    /* ★★ 2026-09-25 二期：测试子页（用户要求「测试里让我也选一下背景图，选一下天气，
       再和猫的状态结合，选择的页面可以参照现有的」；★2026-09-27 天气选择页整页退役——
       天气在合成预览按 POWER 循环即可，独立选择页无用） */
    PG_TEST_SPR,       /* 精灵测试（原素材测试页：逐个贴 cat.bin 全部猫姿态） */
    PG_TEST_BG,        /* 背景图选择 ★R147：**测试菜单无入口**（菜单 case 0 直进 PG_TEST_SPR），页面留作备用 */
    PG_TEST_PAT,       /* ★2026-09-26 四改：状态贴片**开关列表**（每片独立开关，片10/11 当 switch） */
    PG_TEST_BATT,      /* ★2026-09-26 电量状态子页：正常/低电量/充电中/充满 4 态模拟 */
    PG_TEST_NET,       /* ★flash16 网络状态子页：正常/无网络/无wifi（无网络/无wifi 本就互斥 → 合并进子页） */
    PG_TEST_BUB,       /* ★气泡测试子页（2026-09-28 恢复版保留）：无 + 气泡 4 食物 5 选 1，
                          选定态在合成预览看效果（走新放置表版 fb_draw_bubble） */
    PG_TEST_PERIOD,    /* ★2026-09-29 新增：模拟时段子页 —— 破晓/清晨/正午/傍晚/深夜 5 选 1，
                          只影响测试页（背景图/合成预览）的房间+窗+网点+月相，不落 NVS */
    PG_TEST_COMB,      /* 合成预览：选定背景×天气 + 姿态 + 贴片开关组合，按主页绘制顺序整屏复刻 */
    PG_TEST_ANIM,      /* ★★ 2026-10-09 新增：交互动画测试列表（抽帧动画逐段播放，见 draw_test_anim） */
    PG_TEST_VISIT,     /* ★2026-10-25 基座接线①：串门图测试（两组素材预览/切换，见 draw_test_visit_page） */
    /* ★★ 2026-10-11 R26：背包系统四页（机制文档 §2.1 定稿）—— 必须排在 PG_GUIDE **之前**
       （PG_GUIDE 必须保持末页：PAGE_HINT[] 断言按它定上界；新页条目已在 PAGE_HINT 补 NULL）。 */
    PG_BAG,            /* ★R30 背包 = 可携带物品列表（BOOT 移光标 / POWER 佩戴放回） */
    PG_BAG_SWAP,       /* 满 4 替换选择页：BOOT 移光标 / POWER 选定（→ 二次确认） */
    PG_BAG_SWAP_CONF,  /* 替换二次确认：「带上 X / 换下 Y」（双框 y117..142） */
    PG_BAG_PICK,       /* ★★ 2026-10-12：可携带物品选择页（按键：BOOT 移光标 / POWER 选定） */
    /* ★★★ 2026-10-13 新增：可携带物品选择页点某件 → 该件「携带详情页」（同收藏品详情，
       无左右切换/无标签；底部携带/替换 → 写回目标槽 → 回选择页）。与触屏版同值同序。 */
    PG_BAG_ITEM,
    /* ★★★ 2026-10-13 新增（用户报④）：语言二级列表页（列所有语言 + 当前语言右「正使用」）。
       BOOT 移光标 / POWER 选定切换 / BOOT 长 = 回设置页。与触屏版同值同序。 */
    PG_LANG,
    /* ★★ 2026-10-08 新增（本轮）：**操作提示**（按键版专属 · 两页）。
       用户 2026-10-07 原话：「有一小部分画面还是需要有区别 —— 按键版要出一个**操作指南**」
       「第一次配网之后跳出来……以后再想看，**关于页**有入口」。
       ⚠ 必须排在 page_t **末尾** —— 新页排前面会让 PAGE_HINT[] 的指定初始化器与
         `_Static_assert(PG_TEST_COMB + 1 == ...)` 一起失效。 */
    PG_GUIDE,
    /* ★★C 商店（2026-10-16，机制文档 §8）：详情页样式一页一件 + 购买二次确认。
       本批入口 = 测试菜单「商店」行（功能页格子要动 ui.bin/icon.bin 出图管线，下批上）。 */
    PG_SHOP,
    PG_SHOP_CONF,      /* 购买二次确认（双框：取消/确认，仿 PG_BAG_SWAP_CONF 版式） */
    PG_ACHV,           /* ★E 成就列表（单页 4 行/页 + 底部详情；POWER/tap = 领取） */
    PG_MAP,            /* ★F 旅迹（流水账 2 页：近郊 3 + 远行 4） */
    /* ★★G（2026-10-16）：API 接口页 —— 功能页 case 9 正式入口（替换原 PG_WIP 占位）。
       POWER/tap = 切 API 模式开关；长按 = 回功能页。与触屏版同值同序。 */
    PG_API,
    /* ★★ 2026-10-17：OTA 线上升级六页（用户拍板「入口按新口径」：
       ① 设置第 2 页「固件升级」行 ② 关于页底部「检查版本」按钮）。
       两版**同值同序**（与触屏/按键其余页一致），几何照原型 p26..31。 */
    PG_OTA_CHECK,      /* 正在检查（转圈；★本地改版：锁输入等结果，无取消） */
    PG_OTA_FOUND,      /* 发现新版本（版本号 + 包大小 + 「安装并升级」按钮） */
    PG_OTA_NEWEST,     /* 已是最新（+ 「返回」按钮） */
    PG_OTA_PROG,       /* 正在写入（进度条；★本地改版：锁输入等结果） */
    PG_OTA_OK,         /* 升级完成（3 秒后自动重启） */
    PG_OTA_FAIL,       /* 升级失败（3 秒自动回设置；原因写正文） */
    /* ★★ 2026-10-19 本地升级改版（放弃在线 OTA）：
       入口先进**选择页**（TF 卡升级 / 网页升级，标准列表两行），再各走各路：
       TF → CHECK（读卡比版本）→ FOUND / NEWEST / FAIL（错误报告）→ PROG → OK；
       网页 → WEB（热点已开 + 网址）→ 手机 /upgrade 上传 → PROG → OK / FAIL。
       两页白底自绘不占 ui.bin 槽；两版同值同序（插在六页之后）。 */
    PG_OTA_SEL,        /* 本地升级选择：TF 卡 / 网页 两行（BOOT 移光标 / POWER 确认） */
    PG_OTA_WEB,        /* 网页升级：热点已开（WiFi 图标借 13 页 + 热点名 + 192.168.4.1） */
    PG_OTA_MODE,       /* ★R218 二级「升级方式」：手动升级（热点传包）/ 在线升级（拉公开源） */
    PG_POST_IMPORT,    /* 照片明信片导入：热点已开，手机连上自动弹导入页（2026-10-20） */
    /* ★★ 2026-10-17：成就**详情页**与领取结果页（用户报「点击后完全打不开二级页面」）。
       流程（原型 p03→p04→p05/p06→p07）：列表点某条 → 详情（达成条件/物品清单/效果
       + 底部按钮）→ 领取 → 结果页（真槽 20 圆勾）→ 回列表。 */
    PG_ACHV_DET,       /* 成就详情（三态按钮：领取 +N 贝币 / 已领取 / 无法领取） */
    PG_ACHV_OK,        /* 领取成功结果页（3 秒自动回列表） */
    /* ★★ 2026-10-18：测试「一键远行」的**二级选目的地页**（用户令：点一键远行后再选一个，
       不要四个平铺在菜单里）。4 行 = EXP_TBL 四站；tap 行/POWER = 立即出发（无视套装/冷却）；
       返回 = 测试菜单。放枚举末尾（PAGE_HINT 断言上界随之 +1）。 */
    PG_TEST_TRIP,
    /* ★★ 2026-10-18 一级**分类页**（用户令「点进去先是一个品类的划分」）：
       收藏品 = 消耗品 / 随身物品 / 功能道具 / 纪念品；
       明信片 = 常规信件 / 笔友来信 / 我的信件（朋友来信等朋友交互那批再接）。
       选一类 → 对应的列表（列表内顺序/计数自动按类过滤）。放枚举末尾。 */
    PG_LOOT_CAT,
    PG_LOOT_STACK,    /* ★2026-10-24 D5：随身物品四堆二级页（城市/山野/海滨/通用） */
    PG_POST_CAT,
    /* ★★2c 2026-10-21：我的照片明信片（mypost）两页 —— 列表 + 详情（正面照片 / 背面文字）。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +2。 */
    PG_MYPOST_LIST,
    PG_MYPOST_ITEM,
    /* ★★2026-10-22 用户令：mypost 满仓提示页（纯告知：190 将满预警 / 满 200 顶掉告知；
       无拒收按钮，自动顶掉）。放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_MYPOST_NOTICE,
    PG_MYPOST_ACT,      /* ★2026-10-22 用户令：详情页操作页（删除 / 转发；转发占位未接线） */
    PG_MYPOST_DEL,      /* ★2026-10-22 用户令：删除二次确认（双选框：BOOT 切换 / POWER 确定 / 长按取消） */
    PG_MYPOST_FWD,      /* ★2026-10-25 A2：转发好友选择页（3 行/页；tap 行/POWER = 发送） */
    PG_FAV_LIST,        /* ★2026-10-22 用户令：我的收藏（分类第 4 项）列表 */
    /* ★★2026-10-22 朋友功能（第一批 · 离线）：好友列表 + 我的二维码。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +2。 */
    PG_FRIEND,          /* 朋友列表（3 行/页 + 底部「我的二维码」） */
    PG_FRIEND_QR,       /* 我的二维码（扫码加好友；底部按钮切「添加朋友」热点模式） */
    PG_FWD_NOFRIEND,    /* ★R76：转发时暂无好友的纯文字提示页（3s 自动回 / 任意键返回） */
    PG_TEST_FRVIS,      /* ★2026-10-27：测试「猫串门」选好友页（本地预览 / 远端触发） */
    /* ★★ R92（2026-10-27 用户令）：好友详情页（名字 + 6 位码 + 双按钮「修改 / 删除」）。
       交互**整套照抄 PG_WIFI_ITEM**：触屏点哪颗哪颗反白并即时执行；按键 BOOT 切选中、POWER 执行。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_FRIEND_ITEM,
    /* ★R131（用户令「旅程点开能看那几天」）：行程详情（按天分页）。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_TRIP_DET,
    /* ★R194（同触屏版）：设置第 2 页「房间风格」二级选择页（默认 / 科技）。
       放枚举末尾（两版同值同序），PAGE_HINT 断言上界随之 +1。 */
    PG_ROOM_STYLE,
} page_t;
#endif

#define BOWL_EMPTY     0
#define BOWL_FULL      1
#define BOWL_EATING    2
#define BOWL_RESIDUE   3

/* ---- 可调时间常量：口感只动这一段 ---- */
#define BOWL_WAIT_MIN_SEC   (3 * 60)       /* 放食后最短 3 分钟就来吃 */
#define BOWL_WAIT_MAX_SEC   (45 * 60)      /* 最长 45 分钟才来吃 */
#define BOWL_EAT_SEC        (60 * 60)      /* 一顿**最多吃 1 小时**（★2026-09-21 用户定：
                                              原为 10 分钟；到点还没吃完 → 直接变残渣）*/
#define BOWL_SKIP_PCT       4              /* ★ 极小概率不吃：4% —— **别动**，
                                            *   用户明确说「不加大它整体吃不吃的概率」 */
#define BOWL_NOW_PCT        25             /* ★ 2026-09-21 用户口径（两次都以此为准）：
                                            *   ①「喂食后猫立即开始吃的概率稍微加大一点
                                            *      （只加大时间上立即开吃的概率，不加大整体吃不吃的）」
                                            *   ②「**就是猫在家的情况**（可能是从外面回来还是啥的）
                                            *      看到有食物，是有可能会立即去吃的，需要有这个概率」
                                            *   → = **「猫在家 + 看到盆里有食」时，有 25% 的概率不等
                                            *     随机时长、当场就吃**。想更黏人就把这个数调大；
                                            *     整体「吃不吃」仍是 96%（= 100 − BOWL_SKIP_PCT）。
                                            *   ⚠ 真正的闸门在 bowl_step 的 `!st->outside`：
                                            *     猫在外面时**不会**吃 —— 食物留着，等它回来
                                            *     （回来那一刻若已过 eat_at，就是「一进门就开吃」）。*/
#define BOWL_NOW_SEC        1              /* 「马上」= 放下后 1 秒。
                                            *   ★ 不能写 0：本状态机里 `eat_at == 0` 是
                                            *   「这次不吃」的哨兵值（见 bowl_step 的判据）。 */
#define BOWL_LIFE_SEC       (24 * 3600)    /* 满 · 待吃 → 空：24h（没被吃则无残渣） */
#define BOWL_RESIDUE_SEC    (24 * 3600)    /* 残渣 → 空：24h */
typedef struct {
    int    state;          /* BOWL_* */
    int    food;           /* 0..N_FOOD-1 */
    time_t ts;             /* 放食时刻（本地伪 epoch） */
    time_t eat_at;         /* 预计开始吃；★ 0 = 掷骰子判为「这次不吃」 */
    time_t done_at;        /* 吃完变残渣的时刻（进「进食中」时才填） */
} bowl_t;
typedef struct { const char *name; int8_t days; uint8_t token, env, pass, special; } exp_dest_t;
#define MYPOST_MAX         200          /* 用户明信片上限（含 AI 明信片）*/
#define POST_RD_MAX   640                          /* ★586+API=587 ≤ 640 ✓ */                          /* ★2026-10-25 三.3：127→559+API=560，位图容量 256→640 */
#define POST_RD_BYTES ((POST_RD_MAX + 7) / 8)      /* = 32 字节（★2026-10-16：128→256，47 国内+80 远行=127 张）*/
#define N_POST 586   /* sizeof(g_posts) 式改常数（表 2d 迁 post_system.c），模块内 _Static_assert 对账 */
#define COIN_PRICE_FIND       9      /* ★R113：6→9 —— 首飞墙实测 170 币≈14 趟（1~2 周）太靠后，
                                        发现奖励抬到 9 币后前期收入 ≈18 币/趟，配合商店降价 ⇒ 首飞压到 ~8 趟。 */
#define DEST_W_ABROAD 18
#define DEST_W_CITY   25               /* 目的地权重（重设计稿 §6.2 全解锁口径，可调） */
#define DEST_W_SEA    22
#define DEST_W_WILD   35
#define EPD_H           200
#define EPD_W           200
#define LOOT_DROP_EMPTY_CITY    130   /* ★R120 实测空手率：130 / (表内权重 312 + 130) ≈ **29%** */
#define LOOT_DROP_EMPTY_SEA     110   /* ★R120 实测空手率：110 / (219 + 110) ≈ **33%** */
#define LOOT_DROP_EMPTY_WILD    130   /* ★R120 实测空手率：130 / (276 + 130) ≈ **32%** */
#define LOOT_FEATHER_SYNC_MAX    3     /* 第 3 次必得 */
#define LOOT_FEATHER_SYNC_PCT   40     /* 空箱保底：每次同步 40%（§0.5 羽毛行） */
#define LOOT_GONE  { "", "", 0, LOOT_SER_CITY }   /* 占位行：不显示/不掉落/不可拥有 */
#define N_POST_BASE  294                           /* ★2026-10-26 R74：4 张远行片移出近郊段后，近郊池 = [0,294)
                                                       （旧值 298；post_load 有 post_rev=2 位图置换迁移，别删） */
#define PERSONA_EXPLORER  0
#define PERSONA_OBSERVER  1
#define SHOP_ICON_LUCK 5
#define SHOP_ICON_MEAL 6
#define TRIP_EXTRA_PCT_EXPLORER 15     /* 探险家：带回 +1 概率（§5 表未给数，暂取同款 15%，可调） */
#define ZF_FONT_BODY  0
typedef struct { uint8_t idx; uint8_t w; } loot_drop_t;

#define SHOP_I_LUCK  200     /* ★2026-10-27：由 100 改为 200 —— 腾出 100/101 给真实收藏品「万用工具/回忆相册」上架，避免与 SHOP_I_LUCK 虚拟号撞号（B1 补商店通道） */
#define SHOP_I_MEAL  201     /* ★2026-10-27：由 101 改为 201 —— 同上，让出 101 给「回忆相册」 */
#define SHOP_I_WISH  102     /* ★2026-10-21 许愿签（虚拟件 LOOT_V_WISH 的商店编号） */
#define SHOP_I_SUN   106     /* ★2026-10-26 改名卡（虚拟件 LOOT_V_RENAME，原晴天符图标 ITEM_ICON 3）*/
#define SHOP_I_RECALL 107    /* ★2026-10-26 召回币（112 号收藏品改造，图标 = 原幸运币硬币图）*/
#define SHOP_POOL_N 40   /* ★2026-10-27 修：补 100/101 两件 → 实际行数 40。表在 main.c 内有 _Static_assert 对账，再追加商品忘了改这里会编译期报错。
                            表在 main.c 内有 _Static_assert 对账，再追加商品忘了改这里会编译期报错。 */
#define SHOP_SLOT_N  6      /* ★2026-10-22 四（用户令）：每日上架 3 → 6 件（页内 ◀▶ 翻看） */
#define ZF_FONT_TITLE 1
typedef struct { int item; int price; const char *name; } shop_pool_t;
#define API_POST_IDX N_POST         /* 动态明信片专用槽（127） */

/* ★R131：行程流水记账（寄出一张片 = 记一条）。ed<0 = 近郊；ai_title 非空 = AI 生成（当场快照标题）。 */
void jtrip_note_mail(int ed, int day, int post, const char *ai_title);
/* ★R131：AI 片「标题」的只读入口（g_api_t 是 main.c 内部静态，exp_system.c 记账要用）。 */
const char *api_live_title(void);

/* ★R142：明信片详情页的**翻张域**。用户报「从我的收藏点进去，上下滑翻的却是常规信件」。
   `post_goto()` 原来只认全局已收到集合；现在先问这两个函数 —— 收藏域内按收藏顺序翻。 */
bool post_fav_domain(void);
int  post_fav_step(int cur_raw, int delta);
bool post_fav_pos(int cur_raw, int *pos, int *total);
#define FAV_CAT    3        /* ★2026-10-22 用户令：第 4 项 = 我的收藏（跨两套系统的收藏件） */
#define MYPOST_ADDR_MAX    36u
#define MYPOST_CAT 2        /* ★分类页第 3 项 = 特殊信件（0=常规信件 / 1=笔友来信 / 2=特殊信件） */
#define MYPOST_DATA_BYTES  2268u        /* = POST_BYTES(164×108 1bpp / 行 21B)；写死避免前向引用 */
#define MYPOST_FIELD_BLK   (1u + MYPOST_TITLE_MAX + 1u + MYPOST_ADDR_MAX)   /* 68B */
#define MYPOST_HDR_OFF     0x0000u      /* header 扇区（分区内相对 s_mp_off）*/
#define MYPOST_LEN_OFF     MYPOST_DATA_BYTES                    /* 2268 */
#define MYPOST_MAGIC       0x5350594Du  /* "MYPS" */
#define MYPOST_META_OFF    2560u                                /* 元数据（给文字让位） */
#define MYPOST_PAYLOAD_MAX (MYPOST_TEXT_OFF + 1u + MYPOST_TEXT_MAX + 1u +                             MYPOST_TITLE_MAX + 1u + MYPOST_ADDR_MAX + 1u + MYPOST_SENDER_MAX)
#define MYPOST_SD_MAX    280          /* 卡上溢出信上限（扫目录最多收这么多；触屏版 DRAM 更紧，280×4B=1120，给收藏表腾空间）*/
#define MYPOST_SIZE        0xC9000u     /* ★2026-10-22 用户令 99→200 槽：804KB = header 4KB + 200 槽×4KB。                                            空间代价：内容可用 3.30→2.90 MB，照片上限 883→701 张（仍 > 500 目标）。                                            ⚠ 区起点随之前移 404KB（分区尾-804K）⇒ **老卡还在旧偏移**，                                              由 mypost_load() 里的一次性迁移搬过来，见 mypost_migrate_once()。 */
#define MYPOST_SLOT_MAGIC  0x4350594Du  /* "MYPC"（槽有效标识）*/
#define MYPOST_SLOT_OFF    0x1000u      /* 数据槽区起点（第 0 槽）*/
#define MYPOST_SLOT_SZ     0x1000u      /* 每槽 4KB（1 扇区）*/
#define MYPOST_TEXT_MAX    180u                                 /* 60 汉字 × 3B */
#define MYPOST_TEXT_OFF    (MYPOST_DATA_BYTES + 1u)             /* 2269 */
#define MYPOST_TITLE_MAX   30u
#define MYPOST_TITLE_OFF   2449u
#define MYPOST_SENDER_OFF  2520u    /* ★2026-10-27 转发者名字：1B 长度 + 30B（空 = 0） */
#define MYPOST_SENDER_MAX  30u
#define MYPOST_VER         1
#define MYP_LIST_NVIS   4          /* 卡片行数（★2026-10-22 B：制作行已删；★用户令：                                       第 4 行空着也是浪费 → 3→**4 行/页**（WIFI_ROW_Y[0..3]）*/
#define POST_BYTES   (POST_ROW * POST_H)                     /* = 2268/张 */
#define POST_DATE_X   TP_FRAME_R                                   /* ★2026-10-07：日期右对齐到**照片框右沿**（=182；卡片版左右内缩对称） */
#define POST_H       108
#define POST_HOLD_MS   3000          /* 与 net_prov.c::PROV_TOUCH_SKIP_HOLD_MS 同值 */
#define POST_NOTE_LINES  4
#define POST_PLACE_W 124
#define POST_PLACE_X  (POST_CARD_L + POST_INSET)                   /* = 17 */
#define POST_ROW     ((POST_W + 7) / 8)                      /* = 21 */
enum { POST_SENDER_CAT = 0, POST_SENDER_BIRD = 1, POST_SENDER_BUTTERFLY = 2, POST_SENDER_ORIG = 3 };
typedef struct { uint8_t kind; uint32_t key; bool can_delete; } mail_h;
typedef uint32_t mypost_sd_t;        /* s_sd[] 元素 = 卡信 ts（文件名 = 十进制 ts.bin）*/
typedef struct { int l, t, r, b; } touch_zone_t;
#define POST_W       164
#define POST_CARD_L   8
#define POST_INSET    9                                            /* 卡内边距（左/右/上同档） */
#define TP_FRAME_R   (POST_CARD_R - POST_INSET)      /* = 182 ★2026-10-07 卡片版：右沿从屏右 191 收回卡内（左右对称） */
#define POST_CARD_R   191
typedef struct {
    ws_config_t *cfg;
    ws_state_t st;
} app_ctx_t;

extern app_ctx_t g_app;
/* ★R208：128 → 256。OpenAI/部分中转的项目 key 会超过 127 字节，原来服务端
   （配网页 h_api_save）只收 128B、静默截断后仍回「已保存」⇒ 设备端 401，
   页面却显示成功。NVS 读写与 net_prov 的接收缓冲必须同步放大。 */
#define API_KEY_MAX  256
#define EXP_CD_DAYS   5   /* ★R120（用户令 2026-11-02）：7 → 5 —— 与各站天数压缩配套，缩短「一站在外面卡一周」的空白期 */
#define EXP_N 10   /* sizeof 式改常数（表 extern 化） */
#define EXP_W_BASE     100   /* ★2026-10-27 机制甲：每个「可去」目的地的基础权重（远行全站加权抽签用） */
#define EXP_W_CLIMATE  100   /* ★2026-10-27 机制甲：拥有对应气候装备/首去 → 该地权重 +100（约 2:1 倾向，可调） */
#define EXP_POST_GRP  40
/* ★2026-10-26：远行纪念池池宽 4→8（用户令「远行纪念溢出 → 扩池」）。
   埃及/摩洛哥各有 7 件当地件、印度 5 件，原 4 格装不下 ⇒ 144-146/150-152/159 永远发不出来。
   8 = exp_souv_got 的 uint8_t 掩码上限（一字节 8 位），再宽就要换 uint16_t + NVS blob 扩容。 */
#define EXP_SOUV_W    8

extern const uint8_t EXP_SOUV_POOL[EXP_N][EXP_SOUV_W];
extern const exp_dest_t EXP_TBL[];
extern const shop_pool_t SHOP_POOL[];
extern const char *TAG;
extern const touch_zone_t TZ_PROV_SKIP;

/* ★R192：取名页（scr_B / scr_B2）的触摸探针 —— 由 net_prov.c 的取名阻塞循环每 50ms 调一次。
   为什么必须有：那段循环原来**只读 BOOT 按键**，触屏用户点「用默认名猫猫」按钮毫无反应、
   想退出也只能干等（用户实报「卡死在取名页，既不能返回也不能操作」）。
   screen：0 = 等待屏（底部一个整宽按钮）；1 = 默认名二次确认屏（左返回 / 右确定）。
   按键版恒返回 0（无触摸），行为不变。 */
#define PET_NAME_TOUCH_NONE     0
#define PET_NAME_TOUCH_SKIP     0x02   /* 两屏通用：点底部「暂不取名」/ 左上返回 = 跳过取名（不写名字）*/
#define PET_NAME_TOUCH_CANCEL   0x04   /* 确认屏：点左「返回」= 回等待屏 */
#define PET_NAME_TOUCH_OK       0x08   /* 确认屏：点右「确定」= 采用默认名 */
int pet_naming_touch_probe(int screen);
extern uint8_t exp_souv_got[EXP_N];
extern char g_api_key[API_KEY_MAX];
extern post_t g_api_live;
extern bool g_api_on;
extern int g_bag_from;
extern int g_bag_new;
extern int g_bag_sel;
extern bool g_bag_slots_ready;
extern bowl_t g_bowl;
extern bool g_fwd_is_post;
extern int g_fwd_off;
extern int g_fwd_post_idx;
extern int g_fwd_sel;
extern bool g_fwd_sending;
extern int g_fwd_slot;
extern loot_bits_t g_loot_carry;
extern int g_loot_cat;
extern loot_bits_t g_loot_own;
extern loot_bits_t g_loot_rd;
extern int g_loot_stack;
extern int g_loot_view;
extern page_t g_page;
extern int g_persona;
extern char g_pet_name[];
void pet_name_clear(void);          /* ★2026-10-26 改名卡：清名（pet_name + named_once）*/
void pet_draw_naming_wait(void);    /* ★R94：取名引导屏（配网子流程 / 改名卡用掉后都上这张）*/
extern int g_post_cat;
extern uint8_t g_post_got[POST_RD_BYTES];
extern uint8_t g_post_rd[POST_RD_BYTES];
extern int g_post_side;
extern int g_post_unread;
extern int g_post_view;
extern int g_shop_sel;
extern int8_t g_rtc_mood_lvl;          /* 定义在 main.c，带 RTC_DATA_ATTR；extern 侧不能重复带段属性（否则 -Wattributes：两个 .rtc.data.N 冲突）*/
extern bool s_coin_rep;
extern int s_coin_rep_m;
extern int s_coin_rep_n;
extern bool s_coin_rep_seen;
extern int s_coin_rep_trips;
extern uint16_t s_exp_post_idx[EXP_N][EXP_POST_GRP];
extern int s_exp_post_n[EXP_N];
extern int s_grant_dest;
extern uint32_t s_mp_now;
extern uint32_t s_mp_off;
extern bool s_mp_ready;
extern uint32_t s_mp_ts[];
extern page_t s_myp_back;
extern int s_myp_cnt;
extern int s_myp_notice;
extern page_t s_myp_notice_back;
extern int s_myp_notice_show;
extern int s_myp_off;
extern int s_myp_sel;
extern int s_myp_side;
extern char s_myp_txt[];
extern mail_h s_op;
extern uint16_t s_post_arr[N_POST];
extern bool s_post_open;
extern mypost_sd_t s_sd[MYPOST_SD_MAX];
extern FILE *s_sd_fp;
extern int s_sd_n;
extern bool s_shop_sold[];
extern bool s_upg_open;
/* ══ ★2026-10-26：friend_ui.c 外置后需要的绘图原语（原全是 main.c 内 static）══
   只开放「画一帧」必需的几个，别把整个 fb 工具集都摊开。 */
#define UI_PROV_GUIDE  11        /* 配网引导页槽位（好友二维码「添加朋友」屏照抄它）*/
#define PROV_SSID_TOP  122       /* 配网引导页「热点名」那行的墨迹顶（两版共用）*/
extern const int AROW_Y[4];      /* 列表行位 52/88/124/160 */
void fb_clear_white(void);
void fb_fill_white(int l, int t, int r, int b);
void fb_load_ui(int page);
void fb_ring(int cx, int cy, int r);
const uint8_t *zf_glyph(int kind, uint32_t cp, int *h, int *rb, int *adv);
#if TOUCH_DEMO
void fb_back_arrow(void);        /* 触屏版专属：固件自绘页左上「‹」（friend_ui.c 也画）*/
void fb_back_erase(void);        /* ★R191：擦掉底图烤死的左上「‹」（本页不可返回时用）*/
#endif
/* 字形点位取法（与 main.c 同源；friend_ui.c 的 3 倍放大码要用）*/
#ifndef ZF_BIT
#define ZF_BIT(b, r, rb, c)  ((const uint8_t)((b)[(r) * (rb) + ((c) >> 3)] >> (7 - ((c) & 7))) & 1)
#endif

extern uint8_t (*fb)[EPD_W];

/* ── 模块向 main.c 借用的函数 ── */
bool api_live_fill(int ed, int day, time_t now);
/* ★R170：一张 AI 明信片的统一简报（画面 / 配文 / 寄自哪），出图与测试入口共用。
   用户令：画面**不再从固件清单里挑**，由文字模型每张现编（ws_api_scene），
   固件只掷「寄自哪 + 地标/日常」并负责画风尾巴；失败回落本地池。
   ai_scene=true 表示画面确实来自模型；false = 用了本地兜底（串口日志可分辨）。 */
typedef struct {
    char where[64];     /* 寄自哪座城（中文全名，ai_place_line 会去「市」） */
    char scene[224];    /* 英文画面描述（不含画风尾巴） */
    char prompt[384];   /* 完整出图提示词 = scene + 固定画风尾巴 */
    char title[32];     /* 标题（已过字库清洗） */
    char body[128];     /* 正文（已过字库清洗 + 断行） */
    bool landmark;      /* true = 地标类（允许带该城味道） */
    bool ai_scene;      /* true = 画面由模型现编；false = 本地场景池/地标关键词 */
} post_brief_t;
void post_brief_make(post_brief_t *b);
/* ★R173：远行归来专用 —— 地点固定为目的地（冰岛/京都…），画面/配文仍由模型现编。
   fixed_where 传中文短名；NULL = 走「按用户城市辐射」的常规口径。 */
void post_brief_make_at(post_brief_t *b, const char *fixed_where);
/* ★R162：AI 明信片地点行 =「<地名去市> <天气>」，与内置明信片「大院 山西平遥 晴」同口径 */
void ai_place_line(const char *city, char *out, int cap);
bool assets_post_photo(int i, uint8_t *dst);
/* ★R186：assets_region_read / a_rd32 声明收口到 assets_rw.h（本头已 include）*/
/* ★R188：统一的 PSRAM 分配（定义在 main.c；text_core.c 装字库索引也要用）*/
uint8_t *psram_u8(size_t n);
/* ★R189：按字库清洗一串上屏文字（丢掉无字形字符）。post_system.c 的读卡路径也调它 ——
   网页上传/好友转发/历史卡没经过生成期清洗，读出来直接画会出空心方框。 */
void post_font_fix(char *s, int cap);
/* ★2026-11-12：给 friend_ui.c 的「串门」测试页画当前房间背景（与合成预览同源）*/
void test_draw_visit_bg(void);
void test_draw_visit_sign(const char *name);
bool exp_souv_grant(int ed);
bool fb_text_note_page(int kind, int x, int y0, const char *utf8, int start_line);
/* ★R150：原本这里还有 loot_fx_is_night / loot_fx_is_rain 两行 —— 本头第 13 行已 include
   "loot_fx.h"，那里（102/103 行）早就有**逐字相同**的声明，两处并存让每个 TU 都触发
   -Wredundant-decls（实测 105 条）。声明收口到 loot_fx.h 一处，这里不再重复。 */
bool mail_force_hit(int dest, bool night, bool rain, bool clear);
bool post_is_got(int i);
int  post_ungot_count(void);      /* ★R157：未收到的预制片数（0 = 抽完 → 全 AI 片） */
const char *t_(const char *zh);
const uint8_t *fb_pack(void);
/* ★R149：去掉 inline。tz_hit 在 main.c 里是**非 inline 的外部定义**，头文件却声明成 inline ——
   按 C99 6.7.4p7，凡声明含 inline 又无 extern 的 TU（本工程 8 个 .c 都包含本头）都必须自带 inline 定义，
   否则行为未定义；GCC 在这些 TU 各报一条 "inline function 'tz_hit' declared but never defined"。
   当前靠 GCC 兜底生成外部引用才没崩，属定时炸弹，改普通声明即可。 */
bool tz_hit(int x, int y, const touch_zone_t *z);
/* ★R149：下面 5 个由 main.c 定义、被 net_prov.c / ota_up.c 以**手写 extern** 引用（原无任何原型，
   -Wmissing-prototypes 逐条报警）。统一登记到此，跨 TU 契约有单一真源；调用方的手写 extern 留着不冲突。 */
bool zf_has_title_glyph(uint32_t cp);
bool pet_name_valid(const char *u8, char *bad_out);
void pet_draw_naming_confirm(void);
/* ★R192：按键版「选中 → 反白」重绘。
   wait_sel(sel)：sel >= 0 = 底部「暂不取名」按钮选中（反白）；sel < 0 = 无选中。
   confirm_sel(csel)：-1 无选中 / 0 左「返回」/ 1 右「确定」。 */
void pet_draw_naming_wait_sel(int sel);
void pet_draw_naming_confirm_sel(int csel);
bool ota_ver_newer(const char *ver);
bool ota_batt_low(void);
int bag_cap(void);
int coin_add(int delta);
int coin_get(void);
int coin_value_of(int item);
int exp_active(void);
int exp_day_of(time_t now);
int exp_pick_ready(void);
int exp_start_trip(int ed, time_t now);
int fb_text_c(int kind, int cx, int y, const char *utf8);
int fb_text_c_fit(int kind, int cx, int y, const char *utf8, int max_w);
int fb_text_l(int kind, int x, int y, const char *utf8);
int fb_text_l_fit(int kind, int x, int y, const char *utf8, int max_w);
int fb_text_note_lines(int kind, const char *utf8);
int fb_text_r(int kind, int rx, int y, const char *utf8);
/* ★R150：删掉 loot_fx_back_bonus / back_pp / coin_pct / drop_pct / home_pp 五行 —— 本头第 13 行
   已 include "loot_fx.h"，那批函数的真源在 loot_fx.h；两处并存让每个 TU 都触发 -Wredundant-decls。*/
int mail_backlog_keep_pct(void);
int mail_pp_total(bool night);
int mood_bonus_pp(void);
int post_unread(void);
int zf_width(int kind, const char *utf8);
void bag_inv_inside(int x0, int y0, int x1, int y1);
void bag_slots_note(int i, bool on);
void bag_slots_rebuild(void);
void bell_add(const char *key, int d);
void bowl_save(void);
void epd_commit(const uint8_t *buf, bool allow_fast);
void exp_check_toast(void);
void exp_day_mail(int ed, time_t now);
void exp_return_settle(int ed, int *granted, int *coin_gain);
void exp_souv_load(void);
void fav_enter(void);
void fb_invert_rect(int l, int t, int r, int b);
void fb_put(int y, int x);
void fb_round_rect(int l, int t, int r, int b);
/* ★R92：全站「双按钮反白」—— 把选中那颗的**框内**填黑（框线保持黑），几何用 TP_BTN2_*。
   friend_ui.c 的好友详情页要用（原为 main.c static，R92 提到共有可见）。 */
void btn2_invert(int isel);

/* ★R92：全站双按钮几何（原在 main.c 的触屏分叉块里，好友详情页也要用 ⇒ 上移到共有区）。
   ⚠ 必须与出图侧 build_ui_touch.py 的 TWO_BTN_* 一致（那边自检⑫ 会报错）。 */
#define TP_BTN2_Y0    117         /* 按钮带上下沿（含） */
#define TP_BTN2_Y1    142
#define TP_BTN2_L0      8         /* 左钮 */
#define TP_BTN2_L1     97
#define TP_BTN2_R0    102         /* 右钮 */
#define TP_BTN2_R1    191

/* ★R92：好友列表每页行数（**唯一来源** —— main.c 的按钮翻页/滑动翻页、friend_ui.c 的
   排版与页码全用它）。原 main.c 与 friend_ui.c 各定义一份（4 / 4），最容易埋
   「列表画 3 行、翻页按 4 行算」这种错。
   R92 由 4 改 3：列表底部要加「添加朋友」整宽按钮（y164..191），
   原第 4 行 AROW_Y[3]=160 的选中框会正面压住它。 */
#define FR_PER_PAGE 3
void food_feed(void);
void go_page(page_t p);
void fwd_result_show_ex(bool ok, page_t back, const char *why);  /* ★2026-10-26：失败页附原因 */
void result_page_show(const char *txt, bool ok, page_t back);    /* ★2026-10-27：通用结果页（一行文案） */
void app_friend_sync(void);   /* ★2026-10-26：好友链同步（每次 ws_sync 收尾调用） */
int  app_self_vis(void);      /* ★2026-10-27 R91：本机对外 vis（0 在家 / 1 串门窗口内 / 2 正常在外或远行） */
void map_date_str(int day, char *buf, int cap);
void map_note_exp(int ed);
void map_note_near(int dest);
void page_render(bool in_page_sel);
void pet_touch(void);
void post_arr_set(int i, int day);
void post_mark_unread(int i);
void post_save(void);
void post_set_got(int i);
void rr_invert_inside(int x0, int y0, int x1, int y1, int rr);

/* ★R150：net_prov.c 的「网页升级门户 / 照片导入门户」对外接口（原先**无任何原型**，
   调用方 main.c、post_system.c 各自手写 extern —— -Wmissing-prototypes 的根因）。
   定义在 net_prov.c，这里做跨 TU 单一真源；ap_ssid 给页面上屏用（热点名同源）。 */
bool ws_upgrade_open(void);
void ws_upgrade_close(void);
void ws_upgrade_ap_ssid(char *out, int cap);
bool ws_post_open(void);
void ws_post_close(void);
void ws_post_ap_ssid(char *out, int cap);
void ws_post_set_recv_cb(int (*cb)(const uint8_t *data, uint32_t len));

#endif
