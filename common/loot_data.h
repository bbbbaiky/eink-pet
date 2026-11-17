/* ═══════════════════════════════════════════════════════════════════
 * loot_data.h —— 收藏品数据真源（类型/宏/表 extern）。表定义在 loot_system.c。
 * ★2026-10-26 重构 2a：loot_t、LOOT_SER、LOOT_I、LOOT_V 系列、N_LOOT、loot_bits_t
 *   自 main.c 迁入；g_loot[] 从 static 定义改 extern。
 * ═════════════════════════════════════════════════════════════════════ */
#ifndef LOOT_DATA_H
#define LOOT_DATA_H

/* 开发满编分叉（R27 定稿）：触屏=0（空箱真实进度）/ 按键=1（满编开发态）。★重构 2a 后 loot_load 在 loot_system.c，宏须随头走。
   ★★R117（用户拍板「关掉」）：**两版统一 0**。原因 = 按键板开机强制满编 178 件，
   导致 ① 成就「达成」判定（拥有全部件）恒真 ⇒ 41 条成就全亮勾，用户看着像没解锁却打了勾；
        ② 测试菜单的「全解锁」开关在按键板上**等于空操作** —— 它的备份是在强制满编之后拍的，
           关开关只会把满编再恢复一遍。
   ⇒ 想要"全有"统一走测试菜单「全解锁」开关（现在两块板都真正生效），开机不再塞东西。 */
#define LOOT_DEV_GIVE_ALL 0


#include <stdint.h>
#include <stdbool.h>


typedef struct {
    const char *name;      /* 列表行标题 + 详情标题（12px） */
    const char *note;      /* 详情副文案单行（10px = §0.5 上屏引导文案） */
    uint8_t     carry;     /* 1 = 携带生效（背包基础 6 格，套装全携带最多 9）/ 0 = 被动（拥有即生效） */
    uint8_t     series;    /* 掉落出处系列（LOOT_SER_*） */
} loot_t;
#define LOOT_SER_CITY    0   /* 城市（默认目的地） */
#define LOOT_SER_WILD    1   /* 山野（拥有小背包解锁 · ★2026-10-18 改被动） */
#define LOOT_SER_SEA     2   /* 海滨（拥有小背包解锁 · ★2026-10-18 改被动） */
#define LOOT_SER_ABROAD  3   /* 异国（携带旧邮票解锁；P0 只寄信不掉） */
#define LOOT_SER_MILE    4   /* 里程碑 / 空箱保底 / 笔友信附 —— 不走系列掉落表 */
#define LOOT_I_SHELL     0   /* 半片贝壳 */
#define LOOT_I_FEATHER   1   /* 一根羽毛 */
#define LOOT_I_BUTTON    2   /* 旧纽扣 */
#define LOOT_I_FISH      3   /* 小鱼干 */
#define LOOT_I_YARN      4   /* 毛线球 */
#define LOOT_I_KEY       5   /* 旧钥匙 */
#define LOOT_I_PINE      6   /* 松果 */
#define LOOT_I_BELL      7   /* 铃铛 */
#define LOOT_I_SNOW      8   /* 雪花结晶 */
#define LOOT_I_WING      9   /* 蝶翼 */
#define LOOT_I_CONCH    10   /* 海螺壳 */
#define LOOT_I_STONE    11   /* 旋涡石 */
#define LOOT_I_GINKGO   12   /* 银杏叶 */
#define LOOT_I_EGG_STONE 13  /* 鹅卵石（2026-10-16 补：原来只写在表里没给宏）*/
#define LOOT_I_STAMP    14   /* 旧邮票 */
#define LOOT_I_ACORN    15   /* 橡果 */
#define LOOT_I_BOTTLE   16   /* 玻璃瓶 */
#define LOOT_I_FLOWER   17   /* 干花枝 */
#define LOOT_I_STAR     18   /* 纸星星 */
#define LOOT_I_STARFISH 19   /* 海星（2026-10-16 补：同上）*/
#define LOOT_I_BAG      20   /* 小背包 */

/* ★ 2026-10-16 扩编下标记号（下标 21..63；图标同序入 loot_icons.h）*/
#define LOOT_I_DANDELION      21   /* 蒲公英 */
#define LOOT_I_SLEEP_HERB     22   /* 安眠香草 */
#define LOOT_I_DOORMAT        23   /* 门垫 */
#define LOOT_I_SILL_PAD       24   /* 窗台垫子 */
#define LOOT_I_SPYGLASS       25   /* 望远镜 */
#define LOOT_I_CLOVER         26   /* 四叶草 */
#define LOOT_I_STAFF          27   /* 探路杖 */
#define LOOT_I_BARK           28   /* 树皮 */
#define LOOT_I_DRIFTWOOD      29   /* 浮木 */
#define LOOT_I_MOSS           30   /* 苔藓 */
#define LOOT_I_SEAGLASS       31   /* 海玻璃 */
#define LOOT_I_PENCIL         32   /* 铅笔头 */
#define LOOT_I_TERU           33   /* 晴天娃娃 */
#define LOOT_I_PHOTO_TOGETHER 34   /* 合照 */
#define LOOT_I_SHARED_UMBRELLA 35   /* 双人伞 */
#define LOOT_I_WAND           36   /* 逗猫棒 */
#define LOOT_I_HAND_WARMER    37   /* 暖手炉 */
#define LOOT_I_COIN_PURSE     38   /* 小钱袋 */
#define LOOT_I_STAMP_ALBUM    39   /* 纪念邮票册 */
#define LOOT_I_WISH_COIN      40   /* 许愿硬币 */
#define LOOT_I_POCKET_WATCH   41   /* 旧怀表 */
#define LOOT_I_BOOTS          42   /* 远行靴 */
#define LOOT_I_ATLAS          43   /* 地图册 */
#define LOOT_I_KITE_SPOOL     44   /* 风筝线轴 */
#define LOOT_I_TRAVEL_HAT     45   /* 旅行帽 */
#define LOOT_I_MINI_UMBRELLA  46   /* 小雨伞 */
#define LOOT_I_SCARF          47   /* 羽绒围巾 */
#define LOOT_I_TREASURE_MAP   48   /* 藏宝图 */
#define LOOT_I_HORSESHOE      49   /* 幸运马蹄铁 */
#define LOOT_I_SKETCHBOOK     50   /* 写生本 */
#define LOOT_I_MOONSTONE      51   /* 月亮石 */
#define LOOT_I_IVY            52   /* 常青藤 */
#define LOOT_I_KNOT           53   /* 同心结 */
#define LOOT_I_CUSHION        54   /* 软垫 */
#define LOOT_I_CAT_BELL_DRUM  55   /* 猫铃鼓 */
#define LOOT_I_OLD_PHOTO      56   /* 旧照片 */
#define LOOT_I_SHELL_CHIME    57   /* 贝壳风铃 */
#define LOOT_I_LANTERN        58   /* 小灯笼 */
#define LOOT_I_RATTAN_CHAIR   59   /* 藤椅 */
#define LOOT_I_PASSPORT       60   /* 护照 */
#define LOOT_I_DINGHY         61   /* 充气艇 */
#define LOOT_I_TICKET         62   /* 机票 */
#define LOOT_I_COMPASS        63   /* 指南针 */
#define LOOT_I_SAKURA_GUIDE     64   /* 樱花路引 */
#define LOOT_I_GONDOLA_TICKET   65   /* 贡多拉船票 */
#define LOOT_I_ICELAND_TOKEN    66   /* 冰岛指引物 */
#define LOOT_I_DESERT_MAP       67   /* 沙漠地图 */
#define LOOT_I_WOOL_HAT         68   /* 毛线帽 */
#define LOOT_I_FOLDING_FAN      69   /* 折扇 */
#define LOOT_I_SUN_SCARF        70   /* 防晒围巾 */
#define LOOT_I_RAIN_BOOTS       71   /* 小雨靴 */
#define LOOT_I_AURORA_STONE     72   /* 极光石 */
#define LOOT_I_SAKURA_SPECIMEN  73   /* 樱花标本 */
#define LOOT_I_DESERT_HOURGLASS 74   /* 沙漠沙漏 */
#define LOOT_I_VENICE_MASK      75   /* 威尼斯面具 */
/* ★2026-10-26 用户最终拍板（槽位复用，不再新增道具）：
     · 好运铃回原槽（虚拟件 76，NVS item_luckbell，"下趟必带回一件"）；
     · 112 号「幸运币」→「召回币」（用掉 → 猫 1 小时内回家，叠加计数 item_recall，沿用硬币图）；
     · 「改名卡」占原晴天符槽（虚拟件 82，NVS item_rename，用原晴天符的图）。 */
#define LOOT_I_RECALL           112  /* 召回币（原幸运币；用掉 → 猫 1 小时内回家，item_recall 计数）*/
#define LOOT_GOT_SZ 4
#define LOOT_GOT_BYTES (N_LOOT * LOOT_GOT_SZ)
#define N_LOOT 178   /* sizeof 式改常数：g_loot[] 已 extern 化，模块内有 _Static_assert 对账 */
#define LOOT_V_LUCK  N_LOOT        /* 虚拟件 76：好运铃（用掉 → 下趟必带回一件；item_luckbell） */
#define LOOT_V_MEAL  (N_LOOT + 1)  /* 虚拟件 77：开饭铃（盆里有食才可用 → 猫马上来吃） */
/* ★2026-10-21 新玩法⑥：虚拟件 78「许愿签」（商店 3 币的消耗品）。
   为什么加它：贝币此前**只有一个出口**（商店一次性买断），攒着的钱没有"花在当下"的
   快感；而出门去向（城市/山野/海滨）此前是系统掷的，玩家只能被动接受。
   许愿签 = 花 3 币把下一趟的去向**按自己心意定**（实现：去最近去得最少的那一类 ——
   这样既满足"我想去山野"，又不用做三选一 UI；文案里写明）。 */
#define LOOT_V_WISH  (N_LOOT + 2)
/* ★2026-10-25 阶段7：消耗品补 3 件虚拟件（图标=ITEM_ICON 0/1/2，商店有售）——凑 8 种 */
#define LOOT_V_CATNIP (N_LOOT + 3)   /* 猫薄荷：亲密度 +3（★R120 订正：旧注释把 +1/+3 写反了） */
#define LOOT_V_BISCUIT (N_LOOT + 4)  /* 小鱼饼干：亲密度 +1 */
#define LOOT_V_HERB   (N_LOOT + 5)   /* 提神草：心情降 3 档（不是全消） */
/* ★2026-10-26 用户令：消耗品里的「晴天符」槽位（图标 ITEM_ICON 3）此前**只有图标没有实装**，
   最终改为「**改名卡**」：用掉 → 名字清空 + 手机连热点改名（计数键 item_rename）。
   · 猫不在外面也随时可用；热点开不起来不消耗。
   · 商店 50 币，可反复买。 */
#define LOOT_V_RENAME (N_LOOT + 6)   /* 改名卡（原晴天符槽位）*/
#define LOOT_V_N      7             /* 7 件虚拟消耗品（好运铃/开饭铃/许愿签/猫薄荷/小鱼饼干/提神草/改名卡）*/
typedef struct { uint64_t lo, hi, hi2; } loot_bits_t;
static inline bool lb_get(const loot_bits_t *b, int i)
{
    if (i < 64)  return ((b->lo  >> i) & 1ull) != 0;
    if (i < 128) return ((b->hi  >> (i - 64)) & 1ull) != 0;
    return ((b->hi2 >> (i - 128)) & 1ull) != 0;
}
static inline void lb_set(loot_bits_t *b, int i, bool v)
{
    if (i < 64)       { if (v) b->lo  |= (1ull << i); else b->lo  &= ~(1ull << i); }
    else if (i < 128) { const uint64_t m2 = 1ull << (i - 64);  if (v) b->hi  |= m2; else b->hi  &= ~m2; }
    else              { const uint64_t m3 = 1ull << (i - 128); if (v) b->hi2 |= m3; else b->hi2 &= ~m3; }
}

extern const loot_t g_loot[];              /* 收藏品总表（定义在 loot_system.c） */
extern const char *const g_loot_desc[N_LOOT];  /* ★R198：定义已是 const 指针数组，放 .flash.rodata */

#endif
