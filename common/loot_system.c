/* ══════════════════════════════════════════════════════════════════════
 * loot_system.c —— 收藏品系统（拥有/携带/分类/堆/获得记录/掉落掷骰/使用消耗）
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
#include "city_place.h"   /* ★R158：出图提示词要英文地名 */
#include "post_data.h"
#include "post_system.h"
#include "api_image.h"
#include "loot_system.h"
#include "net_sync.h"

/* TAG 由 main.c 定义、main_api.h extern（全工程同一 TAG） */


static bool loot_bits_migrate_u64(const char *base, loot_bits_t *b);
static bool loot_is_key(int i);
static bool loot_nvs_get_u64(const char *key, uint64_t *out);
static bool loot_stack_owned(int i);
static int loot_bits_load(const char *base, loot_bits_t *b);
static int loot_carry_cnt(void);
static int loot_pick_dest(void);
static uint64_t loot_nvs_u64(const char *key, uint64_t defv);
static void loot_rd_save(void);
static void loot_stack_add(int i, int d);

/* ── 数据表本体（自 loot_data.h / loot_desc_data.h 搬入）── */
const loot_t g_loot[] = {
    { "半片贝壳", "生气消解速度中等提高",         0, LOOT_SER_SEA },
    { "一根羽毛", "和小鸟成为朋友啦",         0, LOOT_SER_MILE },
    { "旧纽扣",   "出门概率略微提高", 1, LOOT_SER_CITY },   /* ★2026-10-18 换效果（原「空箱保底」死结）+ 用户令改**携带生效** */
    /* ---- 第 4..8 位 = 2026-09-30 第二批入册（位次不可动，图标按位对齐）---- */
    { "小鱼干",   "喂食亲密度略微提高",         0, LOOT_SER_MILE },
    { "毛线球",   "解锁玩球姿态",           0, LOOT_SER_MILE },   /* ★2026-10-18 文案：功能写明白（姿态门控 31） */
    { "旧钥匙",   "去城市多停留4小时", 1, LOOT_SER_CITY },      /* ★2026-10-18 文案：去空格；★scope 接线后真的只在城里生效 */
    { "松果",     "山野带回物品概率大幅提高",      1, LOOT_SER_WILD },
    { "铃铛",     "抚摸亲密度略微提高",      0, LOOT_SER_MILE },
    { "雪花结晶", "夏天看着凉快",        0, LOOT_SER_WILD },
    /* ---- 第 10..20 位 = 2026-10-05 第三批 + R26（位次不可动）----
       ⚠ 文案 = 机制文档 §0.5「上屏引导文案」列逐字对齐；改动先过字检
         （python scripts/check_font_cjk.py / mock 脚本内置字检）。 */
    { "蝶翼",     "解锁看蝴蝶姿态",     0, LOOT_SER_MILE },   /* ★2026-10-18 文案：功能写明白 */
    { "海螺壳",   "生气消解速度略微提高",        0, LOOT_SER_SEA },
    { "旋涡石",   "心情不会掉到最差",        1, LOOT_SER_WILD }, /* ★22:30 用户令「强效的也要变成携带生效的」：文档 §4.2⑧ 心情下限全表唯一「强效果，可取下」+ 第四轮原话「旋涡石改带上才生效（强效果必须可取下）」；拿到就永久生效 = 猫的负面状态永远看不见（文档 L85）*/
    { "银杏叶",   "带回贝币数量中等提高",         1, LOOT_SER_WILD },
    { "鹅卵石",   "打雷也敢出门",      1, LOOT_SER_CITY },
    { "旧邮票",   "寄明信片概率中等提高",  1, LOOT_SER_MILE },
    { "橡果",     "山野带回物品概率中等提高",       1, LOOT_SER_WILD },  /* ★2026-10-18 二次收短：统一「场合+对象+档位提升」句式（≤10 字，给 EN 翻译留宽度） */
    { "玻璃瓶",   "去海滨必定寄回明信片",    1, LOOT_SER_SEA },
    { "干花枝",   "感情淡化大幅变慢",      0, LOOT_SER_CITY },
    { "纸星星",   "夜间寄明信片概率大幅提高",           1, LOOT_SER_CITY }, /* ★21:50 用户令「强力效果改携带」：MAIL_PP 20 全表最高（旧邮票15/铅笔头8）*/
    { "海星",     "旧物换贝币略微增多",     1, LOOT_SER_MILE }, /* ★2026-10-18 用户令：原「旧物换贝币 多 20%」——折算制下重复件直接变钱，就是贝币多 20%，去空格 */
    { "小背包",   "拥有就能去山野和海滨", 0, LOOT_SER_MILE },   /* ★2026-10-18 用户令：前期道具，改为**不用携带、拥有即生效**（原 carry=1「背上它哪都敢去」太苛刻） */
    /* ═══ 第 22..64 件 = 2026-10-16 扩编（43 件 · 图标同批烤入 loot_icons.h 下标 21..63）═══
       ⚠ 同样**位次不可动**：图标按本表下标一一对位。
       ⚠ note = 机制文档 §4 的「上屏文案」逐字；过字检（scripts/_tools/_check_loot_font_1016.py）。
       ⚠ 本轮先打通「名录 + 图标 + 携带位」；各条新杠杆的**数值生效**另批接线（见 HANDOVER）。 */
    { "蒲公英",      "使用后略微安抚猫的心情",               0, LOOT_SER_WILD },  /* ★★2026-10-18 使用型（B 真消耗）：详情页「用掉它」→ 气立刻消一档；文案补「用掉」与按钮对上 */
    { "安眠香草",      "使用后中等安抚猫的心情",             0, LOOT_SER_WILD }, /* ★★2026-10-18 使用型（B 真消耗）：原被动 CALM240 作废 */
    { "门垫",          "去城市概率略微提高",                  1, LOOT_SER_CITY },
    /* ★R112：与门垫文案原本逐字相同，但效果 10pp vs 5pp ⇒ 玩家无从分辨，且"略微"低估了它 */
    { "窗台垫子",      "去城市概率中等提高",                  1, LOOT_SER_CITY },
    { "望远镜",        "去山野概率中等提高",               1, LOOT_SER_WILD },   /* ★2026-10-19 用户拍板短句（出门省字） */
    { "四叶草",        "山野带回物品概率略微提高",                1, LOOT_SER_WILD },
    { "探路杖",        "山野带回物品概率中等提高",                 1, LOOT_SER_WILD },
    { "树皮",          "山野带回物品概率中等提高",                   1, LOOT_SER_WILD },
    { "浮木",          "海滨带回物品概率中等提高",                1, LOOT_SER_SEA },
    /* ★R112：scope 已从 WILD 改 CITY（城市原本一个掉落扩散器都没有；且旧值与树皮 150/WILD
       完全同值，DROP_PCT 取最大 ⇒ 拿到其中一件，另一件永久归零）⇒ 文案同步改为「城市」。 */
    { "苔藓",          "城市带回物品概率中等提高",                    1, LOOT_SER_WILD },
    { "海玻璃",        "海滨带回物品概率大幅提高",                    1, LOOT_SER_SEA }, /* ★21:50 用户令：DROP_PCT 200 与松果并列最高（树皮/浮木/苔藓150）*/
    { "铅笔头",        "寄明信片概率略微提高",                 1, LOOT_SER_CITY },
    { "晴天娃娃",      "晴天必定寄回明信片",                 1, LOOT_SER_MILE }, /* ★21:50 用户令：兄弟件玻璃瓶(海滨)/写生本(山野)均携带，必寄三件统一*/
    { "合照",          "感情淡化略微变慢",             1, LOOT_SER_CITY },
    { "双人伞",        "雨天感情淡化略微变慢",                    0, LOOT_SER_CITY }, /* ★scope 接线后只在雨天生效 */
    { "逗猫棒",        "抚摸亲密度略微提高",                  0, LOOT_SER_CITY },
    { "暖手炉",        "回家亲密度中等提高",                    0, LOOT_SER_MILE },
    { "小钱袋",        "带回贝币数量略微提高",                  1, LOOT_SER_CITY },
    { "纪念邮票册",    "带回贝币数量大幅提高",               1, LOOT_SER_MILE }, /* ★21:50 用户令：COIN_PCT 200 全表最高（银杏叶150/小钱袋120）*/
    /* ★R112：原「出门稍微更容易」与旧纽扣的「略微更容易」几乎同义，但效果 10pp vs 5pp */
    { "许愿硬币",      "出门概率中等提高",                       1, LOOT_SER_MILE },  /* ★2026-10-18 换效果 + 用户令改**携带生效** */
    { "旧怀表",        "出门在外多停留2小时",              1, LOOT_SER_MILE },
    { "远行靴",        "去山野多停留6小时",                  1, LOOT_SER_WILD },
    { "地图册",        "出门在外多停留6小时",                1, LOOT_SER_MILE },
    { "风筝线轴",      "去海滨多停留10小时",                  1, LOOT_SER_SEA },
    { "旅行帽",        "出门概率大幅提高",                      1, LOOT_SER_MILE },
    { "小雨伞",        "下大雨也敢出门",                1, LOOT_SER_CITY },
    { "羽绒围巾",      "下雪也敢出门",                    1, LOOT_SER_WILD }, /* ★2026-10-18 用户拍板：加「雪天不出门」门槛，围巾放行（原「冷天」无门槛可解 = 死效果） */
    { "藏宝图",        "带回物品概率中等提高",              1, LOOT_SER_MILE },
    { "幸运马蹄铁",    "带回物品概率大幅提高",                 1, LOOT_SER_MILE },
    { "写生本",        "去山野必定寄回明信片",                  1, LOOT_SER_WILD },
    { "月亮石",        "夜间感情淡化大幅变慢",           0, LOOT_SER_WILD },  /* ★2026-10-19 用户拍板：保留原句（不改「淡化得慢」） */
    { "常青藤",        "雨天感情淡化大幅变慢",                    0, LOOT_SER_WILD },  /* ★2026-10-19 同上 */
    { "同心结",        "感情淡化大幅变慢",                 1, LOOT_SER_MILE },  /* ★R111(2026-10-30) 用户拍板保留 carry=1（主动设计变更，不是修 bug）。
                                                                                      复核结论：achv_set_carried() 自 2026-10-26(c81218fa) 起按件区分
                                                                                      （carry=0 的件按「拥有」算），故 R109 报告与 R110「甲方案」所称
                                                                                      「同心结 carry=0 导致飞鸟帖 +15% 永不生效」是**误判** ——
                                                                                      飞鸟帖 {39 纪念邮票册 carry=1, 53 同心结 carry=0} 本来就能生效。
                                                                                      但用户确认要把同心结定为**随身物品**（占一格、效果改为携带时生效），
                                                                                      与写生本等 carry=1 件的文案惯例一致（note 描述的是「携带时」的效果）。 */
    { "软垫",          "抚摸亲密度略微提高",                  0, LOOT_SER_CITY },
    { "猫铃鼓",        "摇一摇它跟着哼",                 0, LOOT_SER_CITY },
    { "旧照片",        "看一眼就想起那天",               0, LOOT_SER_CITY },
    { "贝壳风铃",      "风一吹像在海边",                 0, LOOT_SER_SEA },
    { "小灯笼",        "夜里点亮不怕黑",               0, LOOT_SER_CITY },
    { "藤椅",          "晒太阳打个盹",                 0, LOOT_SER_CITY },
    { "护照",          "远行的必需品",                 1, LOOT_SER_MILE },
    { "充气艇",        "坐船远行更容易去成",                 1, LOOT_SER_MILE },
    { "机票",          "跨洋远行更容易去成",                 1, LOOT_SER_MILE },
    { "指南针",        "陆路穿沙更容易去成",                1, LOOT_SER_MILE },
    /* ═══ 第 65..76 件 = 2026-10-16 上午第 4 批（12 件 · 图标同批入 loot_icons.h 下标 64..75）═══
       · 信物 4（樱花路引/贡多拉船票/冰岛指引物/沙漠地图）+ 环境件 4（毛线帽/折扇/防晒围巾/小雨靴）
       · 远行归来专属 4（极光石/樱花标本/沙漠沙漏/威尼斯面具）
       · 出处对齐机制文档 §6.3：信物·环境件可按地标就近掉落；专属只从远行归来拿（不折算）。
       · 信物 = carry 1（硬门槛）；环境件 R115 起也 = carry 1；远行专属件 = carry 0（纯纪念）。 */
    { "樱花路引",       "去京都的必需品",               1, LOOT_SER_WILD },
    { "贡多拉船票",     "去威尼斯的必需品",                  1, LOOT_SER_SEA },
    { "冰岛指引物",     "去冰岛的必需品",              1, LOOT_SER_CITY },
    { "沙漠地图",       "去撒哈拉的必需品",              1, LOOT_SER_CITY },
    /* ★★R115（用户拍板「方案 A」· 环境件升格为**真必须件**）：
       以前这 4 件的文案是「更容易去成 X」，机制上只是「拥有即加成权重、不占携带格」——
       玩家看到「毛线帽」期待它是装备，实际却是个概率 buff，而且 carry=0 让它在详情页
       连「带上它」都点不了。现在改成：**首去该站必须携带**（与信物同级的硬门槛），
       所以 carry 0→1。★R143：门槛件文案统一成「去 X 的必需品」，护照收成「远行的必需品」。
       ★已去过（exp_cnt>0）的站豁免，见 exp_system.c:exp_lack()——老进度不会突然去不了。 */
    { "毛线帽",         "去冰岛的必需品",        1, LOOT_SER_CITY },
    { "折扇",           "去京都的必需品",        1, LOOT_SER_CITY },
    { "防晒围巾",       "去撒哈拉的必需品",      1, LOOT_SER_SEA },
    { "小雨靴",         "去威尼斯的必需品",      1, LOOT_SER_SEA },
    { "极光石",         "冰岛带回的纪念",                0, LOOT_SER_MILE },
    { "樱花标本",       "京都带回的纪念",             0, LOOT_SER_MILE },
    { "沙漠沙漏",       "撒哈拉带回的纪念",              0, LOOT_SER_MILE },
    { "威尼斯面具",     "威尼斯带回的纪念",                0, LOOT_SER_MILE },
    /* ══ 2026-10-25 合并批：从主干（特性线）追加 77..128 共 52 件 ══
       前 76 件与修复线逐一一致，故为**纯尾部追加**，既有 id 0..75 全部不变（存档安全）。 */
    { "折叠地图", "城市多停留2小时", 1, LOOT_SER_CITY },
    { "旧纪念币", "城市寄明信片概率略微提高", 1, LOOT_SER_CITY },
    { "陶瓷咖啡杯", "去城市概率略微提高", 1, LOOT_SER_CITY },
    { "旧门牌", "城市带回物品概率略微提高", 0, LOOT_SER_CITY },
    { "黄铜钥匙扣", "城市带回物品概率中等提高", 1, LOOT_SER_CITY },
    { "防水布袋", "海滨带回物品概率略微提高", 0, LOOT_SER_SEA },
    { "贝壳项链", "去海滨概率略微提高", 1, LOOT_SER_SEA },
    { "海盐罐", "海滨寄明信片概率略微提高", 1, LOOT_SER_SEA },
    { "玻璃浮球", "海滨多停留2小时", 1, LOOT_SER_SEA },
    { "条纹沙滩巾", "海滨带回物品概率略微提高", 1, LOOT_SER_SEA },
    { "绳捆", "山野带回物品概率略微提高", 1, LOOT_SER_WILD },
    { "旧马灯", "夜间出门概率中等提高", 1, LOOT_SER_MILE },
    { "折叠雨衣", "雨天出门概率中等提高", 1, LOOT_SER_MILE },
    { "铁皮水壶", "去山野概率略微提高", 1, LOOT_SER_WILD },
    { "旅行披风", "去各地概率略微提高", 1, LOOT_SER_MILE },
    { "搪瓷缸", "回家亲密度中等提高", 1, LOOT_SER_CITY },
    { "玻璃弹珠", "抚摸亲密度中等提高", 1, LOOT_SER_CITY },
    { "邮戳印", "城市寄明信片概率略微提高", 1, LOOT_SER_CITY },
    { "螃蟹壳", "生气消解速度中等提高", 1, LOOT_SER_SEA },
    { "海胆壳", "带回贝币数量略微提高", 1, LOOT_SER_SEA },
    { "渔网结", "海滨带回物品概率略微提高", 1, LOOT_SER_SEA },
    { "小帆船", "去海滨概率略微提高", 1, LOOT_SER_SEA },
    { "旧车票根", "城市多停留2小时", 1, LOOT_SER_CITY },
    { "小手捞网", "海滨带回物品概率略微提高", 1, LOOT_SER_SEA },
    { "万用工具", "城市和山野带回物品概率略微提高", 1, LOOT_SER_MILE },
    { "回忆相册", "寄明信片概率略微提高", 1, LOOT_SER_MILE },
    { "铁皮饼干盒", "城市带回物品概率略微提高", 1, LOOT_SER_CITY },
    { "旧月票", "城市寄明信片概率略微提高", 1, LOOT_SER_CITY },
    { "火山石", "冰岛带回的纪念", 0, LOOT_SER_MILE },
    { "冰川蓝冰", "冰岛带回的纪念", 0, LOOT_SER_MILE },
    { "和纸书签", "京都带回的纪念", 0, LOOT_SER_MILE },
    { "小木屐", "京都带回的纪念", 0, LOOT_SER_MILE },
    { "星沙瓶", "撒哈拉带回的纪念", 0, LOOT_SER_MILE },
    { "骆驼骨哨", "撒哈拉带回的纪念", 0, LOOT_SER_MILE },
    { "彩色玻璃珠", "威尼斯带回的纪念", 0, LOOT_SER_MILE },
    { "贡多拉船桨", "威尼斯带回的纪念", 0, LOOT_SER_MILE },
    { "召回币", "使用后，猫猫会在一小时内回家", 0, LOOT_SER_MILE },   /* ★2026-10-26 用户令：原幸运币改造，硬币图沿用 */
    { "硫磺结晶", "冰岛带回的纪念", 0, LOOT_SER_MILE },
    { "登山扣", "山野多停留2小时", 1, LOOT_SER_WILD },
    { "琥珀块", "山野寄明信片概率略微提高", 1, LOOT_SER_WILD },
    { "蕨叶标本", "山野寄明信片概率略微提高", 1, LOOT_SER_WILD },
    { "野雉羽毛", "去山野概率略微提高", 1, LOOT_SER_WILD },
    { "干蘑菇", "山野带回物品概率略微提高", 1, LOOT_SER_WILD },
    { "叠石", "山野带回物品概率略微提高", 1, LOOT_SER_WILD },
    { "桦树皮卷", "山野多停留2小时", 1, LOOT_SER_WILD },
    { "鸟巢", "山野寄明信片概率略微提高", 1, LOOT_SER_WILD },
    { "玄武岩柱", "冰岛带回的纪念", 0, LOOT_SER_MILE },
    { "抹茶碗", "京都带回的纪念", 0, LOOT_SER_MILE },
    { "皮革水袋", "撒哈拉带回的纪念", 0, LOOT_SER_MILE },
    { "绣花手帕", "威尼斯带回的纪念", 0, LOOT_SER_MILE },
    { "旧报纸", "城市寄明信片概率略微提高", 1, LOOT_SER_CITY },
    { "渔夫浮标", "海滨带回物品概率略微提高", 0, LOOT_SER_SEA },
/* ══ 2026-10-25 阶段1（A1）：远行道具 44 件（清单序 01..44 · 下标 128..171）══
   系列：01..38（专属6+纪念26+信物6）= LOOT_SER_ABROAD（异国·非掉落）；39..44 备选 = LOOT_SER_MILE。carry=0。══ */
    { "托钵咖啡壶", "旋转着煮咖啡 苦香转出来", 0, LOOT_SER_ABROAD },
    { "羊驼毛织物", "织得密不透风 高原也暖", 0, LOOT_SER_ABROAD },
    { "纸莎草卷", "草茎压平 画着旧船", 0, LOOT_SER_ABROAD },
    { "阿甘油瓶", "一滴就香满屋", 0, LOOT_SER_ABROAD },
    { "绿玉人像", "玉色温润 雕工很细", 0, LOOT_SER_ABROAD },
    { "铜制圣水壶", "壶身敲出花纹", 0, LOOT_SER_ABROAD },
    { "土耳其软糖", "糖粉簌簌往下掉", 0, LOOT_SER_ABROAD },
    { "基里姆毯片", "红的一角 花纹没织完", 0, LOOT_SER_ABROAD },
    { "郁金香灯", "点亮满墙花影", 0, LOOT_SER_ABROAD },
    { "印加小神像", "小小一尊 笑得神秘", 0, LOOT_SER_ABROAD },
    { "彩色编织带", "红黄蓝拧在一起", 0, LOOT_SER_ABROAD },
    { "羊驼玩偶", "毛绒绒 站得歪歪的", 0, LOOT_SER_ABROAD },
    { "印加陶罐", "罐口缺了个小角", 0, LOOT_SER_ABROAD },
    { "象形石碑片", "石片上刻着符号", 0, LOOT_SER_ABROAD },
    { "沙玫瑰", "风沙堆出的花瓣", 0, LOOT_SER_ABROAD },
    { "圣蛇像", "蛇身盘了三圈", 0, LOOT_SER_ABROAD },
    { "法老面具", "金面 闭着眼", 0, LOOT_SER_ABROAD },
    { "荷鲁斯鹰像", "鹰头神站着", 0, LOOT_SER_ABROAD },
    { "河马摆件", "蓝釉的小河马", 0, LOOT_SER_ABROAD },
    { "摩洛哥茶壶", "壶嘴翘得老高", 0, LOOT_SER_ABROAD },
    { "彩绘皮鞋", "鞋尖翘起 绘着花", 0, LOOT_SER_ABROAD },
    { "塔吉锅", "尖盖子扣得严实", 0, LOOT_SER_ABROAD },
    { "摩洛哥皮灯", "皮面上镂着星", 0, LOOT_SER_ABROAD },
    { "摩洛哥香料罐", "掀开盖子冲鼻子", 0, LOOT_SER_ABROAD },
    { "摩洛哥草篮", "草茎编得很紧", 0, LOOT_SER_ABROAD },
    { "麦卢卡蜂蜜罐", "琥珀色的蜜", 0, LOOT_SER_ABROAD },
    { "英式橄榄球", "皮面磨得发亮", 0, LOOT_SER_ABROAD },
    { "蕨类木雕", "蕨叶纹一圈一圈", 0, LOOT_SER_ABROAD },
    { "大象摆件", "鼻子卷着小花", 0, LOOT_SER_ABROAD },
    { "沉香木块", "闻着有点甜", 0, LOOT_SER_ABROAD },
    { "香料盒", "一格一格分得清", 0, LOOT_SER_ABROAD },
    { "万寿菊花环", "橙黄的花还新鲜", 0, LOOT_SER_ABROAD },
    { "恶魔眼", "蓝眼睛直勾勾的", 1, LOOT_SER_ABROAD },
    { "印加太阳盘", "金盘上刻着光芒", 1, LOOT_SER_ABROAD },
    { "圣甲虫", "推着圆球的模样", 1, LOOT_SER_ABROAD },
    { "阿拉丁神灯", "灯身磨得发亮", 1, LOOT_SER_ABROAD },
    { "银蕨叶", "叶背银白一翻就亮", 1, LOOT_SER_ABROAD },
    { "莲花", "花瓣尖上沾着水", 1, LOOT_SER_ABROAD },
    { "小恶魔眼", "和图鉴那枚是一对", 0, LOOT_SER_MILE },
    { "印加十字", "三层阶梯十字", 0, LOOT_SER_MILE },
    { "法蒂玛之手", "五指张开的手", 0, LOOT_SER_MILE },
    { "摩洛哥宫灯", "宫灯镂花很密", 0, LOOT_SER_MILE },
    { "印度手鼓", "鼓面绷得紧紧的", 0, LOOT_SER_MILE },
    { "孔雀羽", "翎眼一圈套一圈", 0, LOOT_SER_MILE },
/* ══ 2026-10-24 B7：环境件 6 件（id 172..177 · 图标待我方，暂占位）══
   ★R115：与上面 4 件同口径 —— 升格为「首去该站的必须件」⇒ carry 0→1、文案改必需。 */
    { "遮阳帽", "去土耳其的必需品", 1, LOOT_SER_ABROAD },
    { "登山靴", "去秘鲁的必需品", 1, LOOT_SER_ABROAD },
    { "水壶", "去埃及的必需品", 1, LOOT_SER_ABROAD },
    { "薄披风", "去摩洛哥的必需品", 1, LOOT_SER_ABROAD },
    { "防风衣", "去新西兰的必需品", 1, LOOT_SER_ABROAD },
    { "遮阳伞", "去印度的必需品", 1, LOOT_SER_ABROAD },
};
const char *const g_loot_desc[N_LOOT] = {   /* ★R198：指针数组本身 const ⇒ 进 .flash.rodata，省 712B DRAM */
    "海边捡的半片贝壳",
    "小鸟送的一根羽毛",
    "掉了漆的旧纽扣",
    "烘干的小鱼干",
    "毛线滚成的球",
    "不知道开哪扇门",
    "捡来的松果",
    "走起路来叮当响",
    "冬天落下的结晶",
    "蝴蝶留下的翅膀",
    "能听到海的声音",
    "海浪磨出的旋涡",
    "扇形的金黄叶子",
    "河水磨圆的石头",
    "盖过邮戳的邮票",
    "橡树掉下的果子",
    "装过漂流的心事",
    "风干的花枝",
    "折起来的小星星",
    "海里爬来的五角星",
    "能装下整个外出",
    "吹一吹就能消气",
    "闻一闻就能睡着",
    "放在门口的垫子",
    "窗台上的软垫子",
    "看得见很远的地方",
    "四片叶子的幸运",
    "探路用的木杖",
    "树上剥下的树皮",
    "漂上岸的浮木",
    "石头上的绿绒",
    "海浪磨平的玻璃",
    "写短了的铅笔",
    "晴天就挂出来",
    "两个头的合影",
    "下雨天一起撑",
    "逗猫专用的小棒",
    "冬天揣着暖手",
    "装贝币的小袋子",
    "集邮的纪念册",
    "许个愿再出门",
    "滴滴答答走个不停",
    "走远路穿它刚好",
    "一页一页翻着看",
    "放风筝剩下的线",
    "戴着它去旅行",
    "下雨就撑开",
    "围上就不怕冷",
    "画着藏宝的路线",
    "听说能带来好运",
    "画下沿途的风景",
    "夜里发光的石头",
    "雨天感情不变淡",
    "系起来就不分开",
    "摸起来软软的",
    "摇一摇跟着哼",
    "看一眼就想起那天",
    "风一吹像在海边",
    "夜里点亮不怕黑",
    "晒太阳打个盹",
    "出远门全靠它",
    "坐船渡海的小艇",
    "跨洋远行的机票",
    "认得方向的针",
    "京都的路上指一指",
    "威尼斯的水上路票",
    "冰岛路上的指引",
    "撒哈拉里认路",
    "毛线织的帽子",
    "扇一扇就凉快",
    "防晒的薄围巾",
    "踩水也不湿脚",
    "冰岛带回的纪念",
    "京都带回的纪念",
    "撒哈拉带回的纪念",
    "威尼斯带回的纪念",
    "折起来塞进兜里",
    "花纹磨得发亮",
    "杯底还有茶渍",
    "门牌上留着地址",
    "黄铜的钥匙扣",
    "防水的布袋子",
    "贝壳串成的项链",
    "装着海的味道",
    "海上漂来的浮球",
    "条纹的沙滩巾",
    "捆东西刚刚好",
    "夜里点一盏灯",
    "折起来挡雨",
    "铁皮的小水壶",
    "披上就出发",
    "搪瓷的老缸子",
    "玻璃滚的小珠",
    "盖出一个邮戳",
    "海边捡的螃蟹壳",
    "带刺的海胆壳",
    "渔网上打的结",
    "扬帆的小木船",
    "根票根留个纪念",
    "捞鱼的小手网",
    "什么都能修一修",
    "一页一页的回忆",
    "铁皮的饼干盒",
    "月票上印着旧站名",
    "火山喷出的黑石",
    "透着冰川的蓝",
    "和纸做的书签",
    "木屐踏过石板路",
    "装着一瓶星光",
    "吹出沙漠的回声",
    "彩色滚动的珠子",
    "威尼斯划船的桨",
    "下次出门必带回物品",
    "火山口的结晶",
    "登山扣一挂就走",
    "树脂凝成的琥珀",
    "蕨叶压成的标本",
    "野雉掉落的羽毛",
    "山里晒的干蘑菇",
    "一块一块叠起来",
    "桦树皮卷的筒",
    "树上搭的小鸟巢",
    "黑色的石柱子",
    "一碗抹茶的温热",
    "皮革缝的水袋",
    "绣花的旧手帕",
    "印满旧闻的报纸",
    "渔夫留下的浮标",
    "土耳其的咖啡壶",
    "羊驼毛织的布",
    "纸莎草编的卷轴",
    "摩洛哥的阿甘油",
    "绿玉雕的小人像",
    "铜制的圣水壶",
    "甜而不腻的软糖",
    "织花的老毯片",
    "玻璃点的彩灯",
    "印加的小神像",
    "彩线编的织带",
    "毛绒绒的羊驼",
    "印加人的陶罐",
    "刻着象形的石片",
    "沙子堆成的玫瑰",
    "圣蛇盘成的雕像",
    "金色的法老面具",
    "鹰头神的雕像",
    "胖乎乎的河马",
    "摩洛哥的银茶壶",
    "彩绘的尖皮鞋",
    "尖顶的塔吉锅",
    "皮面的小灯笼",
    "装满香料的罐子",
    "草编的大篮子",
    "一罐麦卢卡蜜",
    "椭圆的橄榄球",
    "蕨纹刻上木雕",
    "鼻子长长的摆件",
    "一块沉香木",
    "装着各色香料",
    "橙黄的花编成环",
    "蓝眼睛挡住厄运",
    "太阳纹的金圆盘",
    "推滚动的小甲虫",
    "擦一擦愿望成真",
    "新西兰的银蕨",
    "水面上开的花",
    "备用的恶魔眼",
    "印加的十字纹样",
    "保佑平安的手掌",
    "镂花的宫灯",
    "拍出印度的节拍",
    "翠蓝的孔雀羽",
    "硬檐帽挡住日头",
    "拄着它翻山越岭",
    "皮囊装着凉白开",
    "风一吹就飘起来",
    "连帽的外套挡风",
    "伞骨张开的遮阳伞",
};

_Static_assert(sizeof(g_loot) / sizeof(g_loot[0]) == N_LOOT, "N_LOOT != g_loot 表项数");

static EXT_RAM_BSS_ATTR uint8_t s_loot_icon_px[LOOT_ICON_BYTES_PER_ICON];

const uint8_t *loot_icon_load(int idx)
{
    if (idx < 0 || idx >= N_LOOT_ICON + N_ITEM_ICON) return NULL;
    if (!assets_region_read(AID_LOOT, (uint32_t)idx * LOOT_ICON_BYTES_PER_ICON,
                            s_loot_icon_px, LOOT_ICON_BYTES_PER_ICON)) return NULL;
    return s_loot_icon_px;
}

bool loot_alive(int i)
{
    return i >= 0 && i < N_LOOT && g_loot[i].name[0] != 0;
}

bool loot_is_usable(int i)
{
    if (i == LOOT_I_RECALL) return true;     /* ★召回币：用掉 → 猫 1 小时内回家（item_recall 计数）*/
    return i == LOOT_I_DANDELION || i == LOOT_I_SLEEP_HERB;
}

void loot_consume(int i)
{
    if (i < 0 || i >= N_LOOT || !loot_is_owned(i)) return;
    if (loot_stack_owned(i)) {                        /* ★叠加制消耗品：数量-1，归零退出列表 */
        loot_stack_add(i, -1);
        if (!loot_stack_owned(i)) lb_set(&g_loot_own, i, false);
        loot_bits_save("loot_own", &g_loot_own);
        ESP_LOGW(TAG, "★★ 使用并消耗「%s」（余 %d）", g_loot[i].name,
                 loot_nvs_i32(i == 21 ? "item_dandelion" :
                              i == 22 ? "item_sleepherb" : "item_recall", 0));
        return;
    }
    lb_set(&g_loot_own, i, false);
    loot_bits_save("loot_own", &g_loot_own);   /* ★C1：统一 6 段 */
    lb_set(&g_loot_rd, i, false);              /* 已看位一并清 */
    loot_rd_save();
    ESP_LOGW(TAG, "★★ 使用并消耗收藏品「%s」", g_loot[i].name);
}

int loot_nvs_i32(const char *key, int32_t defv)
{
    nvs_handle_t h;
    int32_t v = defv;
    if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return (int)v;
}

void loot_nvs_set_i32(const char *key, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static bool loot_nvs_get_u64(const char *key, uint64_t *out)   /* ★C1 迁移用：存在性可判 */
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return false;
    uint64_t v = 0;
    const bool ok = nvs_get_u64(h, key, &v) == ESP_OK;
    nvs_close(h);
    if (ok && out) *out = v;
    return ok;
}

static uint64_t loot_nvs_u64(const char *key, uint64_t defv)
{
    char k[24];
    uint32_t lo = (uint32_t)loot_nvs_i32(key, (int32_t)(uint32_t)(defv & 0xFFFFFFFFu));
    snprintf(k, sizeof(k), "%s_hi", key);
    uint32_t hi = (uint32_t)loot_nvs_i32(k, (int32_t)(uint32_t)(defv >> 32));
    return ((uint64_t)hi << 32) | lo;
}

/* ★2026-10-26 方案第 4 步：`loot_nvs_set_u64` 全工程零调用（编译告警 -Wunused-function）→ 删除。
   ⚠ 对应的读函数 loot_nvs_get_u64 仍有调用者，不要一起删。 */

bool loot_nvs_get_blob(const char *key, void *buf, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    esp_err_t e = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    return e == ESP_OK && len == n;
}

void loot_nvs_set_blob(const char *key, const void *buf, size_t n)
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, buf, n);
    nvs_commit(h);
    nvs_close(h);
}

static bool loot_is_key(int i)
{
    if (i < 0 || i >= N_LOOT) return false;
    switch (i) {
    case LOOT_I_PASSPORT:                                            /* 护照 */
    case LOOT_I_TICKET: case LOOT_I_DINGHY: case LOOT_I_COMPASS:     /* ★通行档 3 件（机票/充气艇/指南针）= 远行必须 */
    case LOOT_I_POCKET_WATCH: case LOOT_I_ATLAS: case LOOT_I_TRAVEL_HAT:   /* 行囊三件 */
    case LOOT_I_SAKURA_GUIDE:  case LOOT_I_GONDOLA_TICKET:           /* 信物：京都 / 威尼斯 */
    case LOOT_I_ICELAND_TOKEN: case LOOT_I_DESERT_MAP:               /* 信物：冰岛 / 撒哈拉 */
    /* ★R115：环境件 4 件（老批）漏登记 —— 原来它们 carry=0，一直躺在「纪念品」堆里。
       现在升级成必须件（carry=1）后必须归「行装钥匙」，否则会掉进「随身物品」堆，
       10 件环境件会被劈成两堆（172..177 在钥匙、68..71 在随身）。 */
    case LOOT_I_WOOL_HAT: case LOOT_I_FOLDING_FAN:
    case LOOT_I_SUN_SCARF: case LOOT_I_RAIN_BOOTS:                   /* 环境件：冰岛/京都/撒哈拉/威尼斯 */
        return true;
    default:
        if (i >= 160 && i <= 165) return true;                       /* 信物：土耳其..印度（新批） */
        return (i >= 172 && i <= 177);                               /* ★环境件 6 件=钥匙类（机制口径 §一） */
    }
}

int loot_cat_of(int i)
{
    /* ★2026-10-24 D5 四类（机制口径 §一 顺序：消耗→钥匙→携带→拥有即生效）：
       0 消耗品 / 1 行装钥匙（护照+行囊+信物+环境件）/ 2 随身物品（carry）/ 3 纪念品（拥有即生效，用户令定名）。 */
    /* ★2026-10-26 修：虚拟件 6 件（好运铃/开饭铃/许愿签/猫薄荷/小鱼饼干/提神草）全是消耗品。
       原判只列前三件 → 猫薄荷/小鱼饼干/提神草被归进「纪念品」，消耗品页永远少 3 件。 */
    if (i >= N_LOOT) return 0;
    if (i < 0) return 3;
    if (!loot_alive(i)) return -1;   /* ★占位件：不属于任何类（-1 永不命中过滤器） */
    if (i == LOOT_I_DANDELION || i == LOOT_I_SLEEP_HERB || i == LOOT_I_RECALL) return 0;   /* 0 消耗品（用掉即耗） */
    if (loot_is_key(i))             return 1;                        /* 1 行装钥匙（先于 carry 判） */
    if (g_loot[i].carry)            return 2;                        /* 2 随身物品（背上才生效） */
    return 3;                                                        /* 3 纪念品（拥有即生效 + 观赏） */
}

int loot_stack_of(int i)
{
    /* ★2026-10-25 修（用户报「海滨堆只有 8 个」）：堆归属改回**按系列**（机制口径 §二：
       随身装备固有属性）——CITY→城市 WILD→山野 SEA→海滨 MILE/ABROAD→通用。
       旧 fx-scope 推导会把无效果行的件（如 海玻璃/螃蟹壳/渔网结）全挤进通用堆。 */
    if (i < 0 || i >= N_LOOT || !g_loot[i].carry) return -1;
    switch (g_loot[i].series) {
    case LOOT_SER_CITY: return 0;
    case LOOT_SER_WILD: return 1;
    case LOOT_SER_SEA:  return 2;
    default:            return 3;
    }
}

int loot_stack_cnt(int k)
{
    int n = 0;
    for (int i = 0; i < N_LOOT; ++i)
        if (loot_is_owned(i) && loot_cat_of(i) == 2 && loot_stack_of(i) == k) n++;   /* ★随身=cat2 */
    return n;
}

int loot_owned_cnt(void)
{
    int n = 0;
    /* ★★2026-10-19：取值域放宽到 N_LOOT+2 —— 两铃（虚拟件）计数 > 0 = 拥有。 */
    for (int i = 0; i < N_LOOT + LOOT_V_N; ++i)   /* ★虚拟消耗品 7 件（LOOT_V_N）*/
        if (loot_is_owned(i) && (g_loot_cat < 0 || loot_cat_of(i) == g_loot_cat) && (g_loot_cat != 2 || g_loot_stack < 0 || loot_stack_of(i) == g_loot_stack)) n++;
    return n;
}

/* ★R125：原 loot_owned_cnt_all()（空箱保底门槛）随羽毛改判据后无调用者，已删除。 */

static bool loot_stack_owned(int i)
{
    if (i == 21) return loot_nvs_i32("item_dandelion", 0) > 0;
    if (i == 22) return loot_nvs_i32("item_sleepherb", 0) > 0;
    if (i == LOOT_I_RECALL) return loot_nvs_i32("item_recall", 0) > 0;

    return false;
}

static void loot_stack_add(int i, int d)
{
    const char *key = (i == 21) ? "item_dandelion" : (i == 22) ? "item_sleepherb" :
                      (i == LOOT_I_RECALL) ? "item_recall" : NULL;
    if (!key) return;
    loot_nvs_set_i32(key, loot_nvs_i32(key, 0) + d);
}

bool loot_is_owned(int i)
{
    if (loot_stack_owned(i)) return true;   /* ★叠加制消耗品（计数 > 0 = 拥有） */
    /* ★★2026-10-19：两铃 = 虚拟件，拥有 = 计数 > 0（NVS），不走拥有位图；
       ★占位件（LOOT_GONE）恒 false —— 老存档里残留的位不再显示（稳定 ID 口径兜底） */
    if (i == LOOT_V_LUCK) return loot_nvs_i32("item_luckbell", 0) > 0;
    if (i == LOOT_V_CATNIP) return loot_nvs_i32("item_catnip", 0) > 0;
    if (i == LOOT_V_BISCUIT) return loot_nvs_i32("item_biscuit", 0) > 0;
    if (i == LOOT_V_HERB) return loot_nvs_i32("item_herb", 0) > 0;
    if (i == LOOT_V_MEAL) return loot_nvs_i32("item_mealbell", 0) > 0;
    if (i == LOOT_V_WISH) return loot_nvs_i32("item_wish", 0) > 0;
    if (i == LOOT_V_RENAME)  return loot_nvs_i32("item_rename", 0) > 0;
    return loot_alive(i) && lb_get(&g_loot_own, i);
}

const char *loot_name_of(int i)
{
    if (i == LOOT_V_LUCK) return "好运铃";
    if (i == LOOT_V_MEAL) return "开饭铃";
    if (i == LOOT_V_CATNIP) return "猫薄荷";
    if (i == LOOT_V_BISCUIT) return "小鱼饼干";
    if (i == LOOT_V_HERB) return "提神草";
    if (i == LOOT_V_WISH) return "许愿签";
    if (i == LOOT_V_RENAME)  return "改名卡";
    if (i < 0 || i >= N_LOOT) return "";     /* ★2026-10-26 防御：与 loot_note_of 同口径 */
    return g_loot[i].name;
}

const char *loot_note_of(int i)
{
    if (i == LOOT_V_LUCK) return T("下次出门必带回物品");
    if (i == LOOT_V_MEAL) return T("召唤猫猫过来吃饭");
    if (i == LOOT_V_WISH) return T("下次去最冷清的地方");
    /* ★★★ 2026-10-26 修（越界读·同 EXP_TBL[-1] 一类）：下面原来直接 `return g_loot[i].note`，
       而虚拟件 178..184 不在 g_loot[] 里 ⇒ 读表尾之后的内存当 const char* 交给 t_()，
       轻则显示成别人的文案、重则 LoadProhibited。阶段 7 新加的 4 件（猫薄荷/小鱼饼干/提神草/召回币）
       正好都会走到这里（详情页底栏）。 */
    if (i == LOOT_V_CATNIP)  return T("使用后中等增加亲密度");
    if (i == LOOT_V_BISCUIT) return T("使用后略微增加亲密度");
    if (i == LOOT_V_HERB)    return T("使用后大幅安抚猫的心情");
    if (i == LOOT_V_RENAME)  return T("用掉后给猫改个名字");
    if (i < 0 || i >= N_LOOT) return "";
    return g_loot[i].note;
}

static int loot_carry_cnt(void)
{
    int n = 0;
    for (int i = 0; i < N_LOOT; ++i) if (lb_get(&g_loot_carry, i)) n++;
    return n;
}

bool loot_is_carried(int i)
{
    return loot_alive(i) && lb_get(&g_loot_carry, i);   /* ★占位件恒 false（稳定 ID 口径） */
}

void loot_got_set(int i, int day, int place)
{
    if (i < 0 || i >= N_LOOT) return;
    uint8_t buf[LOOT_GOT_BYTES];
    if (!loot_nvs_get_blob("loot_got", buf, sizeof(buf))) memset(buf, 0, sizeof(buf));
    if (day < 0)        day = 0;
    if (day > 0xFFFF)   day = 0xFFFF;
    if (place < -1)     place = -1;
    if (place > 253)    place = 253;
    uint8_t *p = buf + i * LOOT_GOT_SZ;
    p[0] = (uint8_t)(day & 0xFF);
    p[1] = (uint8_t)((day >> 8) & 0xFF);
    p[2] = (uint8_t)(place + 1);            /* 0 = 家，1+ = 目的地序号 */
    p[3] = 0;
    loot_nvs_set_blob("loot_got", buf, sizeof(buf));
}

/* 读一条：返回 true = 有记录（day > 0）。place 回填 -1（家）或目的地序号。 */
bool loot_got_get(int i, int *day, int *place)
{
    if (day)   *day = 0;
    if (place) *place = -1;
    if (i < 0 || i >= N_LOOT) return false;
    uint8_t buf[LOOT_GOT_BYTES];
    if (!loot_nvs_get_blob("loot_got", buf, sizeof(buf))) return false;
    const uint8_t *p = buf + i * LOOT_GOT_SZ;
    const int d = (int)p[0] | ((int)p[1] << 8);
    if (day)   *day = d;
    if (place) *place = (int)p[2] - 1;
    return d > 0;
}

bool loot_grant(int i)
{
    /* ★R207 修（复查）：叠加制消耗品（蒲公英 21 / 安眠香草 22 / 召回币）原来只有
       `loot_stack_owned()` 为真（计数 > 0）才 +1；**计数为 0 时**走下面"首次获得"分支
       只置拥有位、不写计数 ⇒ 该件永远不出现（计数恒 0、拥有位孤立），下次再获得还走同一条路。
       这三件没有"拥有位"语义（拥有 = 计数 > 0），必须无条件 +1。 */
    if (i == 21 || i == 22 || i == LOOT_I_RECALL) {
        loot_stack_add(i, 1);
        ESP_LOGW(TAG, "★★ 获得消耗品「%s」×%d", g_loot[i].name,
                 loot_nvs_i32(i == 21 ? "item_dandelion" :
                              i == 22 ? "item_sleepherb" : "item_recall", 0));
        return true;
    }
    /* ★占位件拒收（稳定 ID 口径）—— 老掉落表漏摘引用时也不至于把空名件发进收藏箱 */
    if (!loot_alive(i) || loot_is_owned(i)) return false;
    lb_set(&g_loot_own, i, true);
    loot_bits_save("loot_own", &g_loot_own);   /* ★C1 */
    /* ★2026-10-22：落一条「获得日 + 所在地」记录（纪念品详情展示用）*/
    loot_got_set(i, (int)(g_app.st.now_local / 86400), s_grant_dest);
    ESP_LOGW(TAG, "★★ 获得收藏品「%s」（拥有 %d/%d · 未读 +1）",
             g_loot[i].name, loot_owned_cnt(), N_LOOT);
    exp_check_toast();                     /* ★B1：到手可能正好凑齐某套远行装备 */
    return true;
}

/* ★2026-10-25：虚拟消耗品图标统一映射（ITEM_ICON：0猫薄荷 1小鱼饼干 2提神草 3原晴天符 4顺风符 5原好运铃 6开饭铃） */
int loot_v_icon(int i)
{
    switch (i) {
    case LOOT_V_RENAME: return 3;      /* 改名卡 = 原晴天符图（用户令：就是要用那张图）*/
    case LOOT_V_MEAL:   return SHOP_ICON_MEAL;
    case LOOT_V_WISH:   return 4;      /* 许愿签 → 顺风符（ITEM_ICON 4） */
    case LOOT_V_LUCK:   return SHOP_ICON_LUCK;  /* 好运铃 = 原铃铛图（ITEM_ICON 5）*/
    case LOOT_V_CATNIP: return 0;
    case LOOT_V_BISCUIT:return 1;
    case LOOT_V_HERB:   return 2;
    default:            return 0;
    }
}

bool loot_carry_set(int i, bool on)
{
    if (i < 0 || i >= N_LOOT || !loot_is_owned(i) || !g_loot[i].carry) return false;
    if (on == loot_is_carried(i)) return false;
    /* ★2026-10-18：按动态容量 bag_cap() 把关（一件没带恒 6；三套装全携带最多 9）。
       ★2026-10-19 用户令：远征票的「免占格旁路」删除 —— 改套装 +1 后与别的件同口径，
       远征票两件也正常占格（集齐两件才 +1，第二件需要有个空槽才带上得去）。 */
    if (on && loot_carry_cnt() >= bag_cap()) {
        ESP_LOGW(TAG, "背包已满（%d/%d）→ 不能再背「%s」（走替换流程）",
                 loot_carry_cnt(), bag_cap(), g_loot[i].name);
        return false;
    }
    lb_set(&g_loot_carry, i, on);
    loot_bits_save("loot_carry", &g_loot_carry);   /* ★C1 */
    bag_slots_note(i, on);                           /* ★ 同步背包有序槽表（容量见 bag_cap()） */
    ESP_LOGW(TAG, "背包 → %s「%s」（%d/%d）",
             on ? "带上" : "放回", g_loot[i].name, loot_carry_cnt(), bag_cap());
    return true;
}

/* ★2026-10-25 C1 实现（放 NVS 助手之后，loot_load 之前）。 */
static int loot_bits_load(const char *base, loot_bits_t *b)
{
    nvs_handle_t h;
    int got = 0;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return 0;
    uint64_t seg[6] = {0, 0, 0, 0, 0, 0};
    static const char *suf[6] = { "", "_hi", "_2", "_3", "_4", "_5" };
    for (int k = 0; k < 6; ++k) {
        char key[24];
        snprintf(key, sizeof(key), "%s%s", base, suf[k]);
        int32_t v = 0;
        if (nvs_get_i32(h, key, &v) == ESP_OK) { seg[k] = (uint32_t)v; got++; }
    }
    nvs_close(h);
    if (got == 0) return 0;
    b->lo  = seg[0] | (seg[1] << 32);
    b->hi  = seg[2] | (seg[3] << 32);
    b->hi2 = seg[4] | (seg[5] << 32);
    return got;
}

void loot_bits_save(const char *base, const loot_bits_t *b)
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return;
    const uint32_t seg[6] = {
        (uint32_t)(b->lo & 0xFFFFFFFFull),  (uint32_t)(b->lo >> 32),
        (uint32_t)(b->hi & 0xFFFFFFFFull),  (uint32_t)(b->hi >> 32),
        (uint32_t)(b->hi2 & 0xFFFFFFFFull), (uint32_t)(b->hi2 >> 32),
    };
    static const char *suf[6] = { "", "_hi", "_2", "_3", "_4", "_5" };
    for (int k = 0; k < 6; ++k) {
        char key[24];
        snprintf(key, sizeof(key), "%s%s", base, suf[k]);
        nvs_set_i32(h, key, (int32_t)seg[k]);
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool loot_bits_migrate_u64(const char *base, loot_bits_t *b)
{
    uint64_t k0[2] = {0, 0};
    bool any = false;
    if (loot_nvs_get_u64(base, &k0[0])) any = true;
    { char kh[28]; snprintf(kh, sizeof(kh), "%s_hi", base);
      if (loot_nvs_get_u64(kh, &k0[1])) any = true; }
    if (!any) return false;
    memset(b, 0, sizeof(*b));
    b->lo = k0[0];
    b->hi = k0[1] & 0xFFFFFFFFFFFFFFFFull;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) == ESP_OK) {
        char kh[28]; snprintf(kh, sizeof(kh), "%s_hi", base);
        nvs_erase_key(h, base);                  /* 旧 u64 条目与新 i32 键同名不同型 → 必须先擦 */
        nvs_erase_key(h, kh);
        nvs_commit(h);
        nvs_close(h);
    }
    loot_bits_save(base, b);
    ESP_LOGW(TAG, "★ 位图迁移 %s：u64 双键 → 192 位 6 段（0x%016llX 0x%016llX）",
             base, (unsigned long long)b->lo, (unsigned long long)b->hi);
    return true;
}

void loot_load(void)
{
    /* ★2026-10-25 C1：三张位图统一 6 键读取 + 旧格式迁移（u64 双键 / 旧 i32 4 键兼容）。 */
    memset(&g_loot_rd, 0, sizeof(g_loot_rd));
    memset(&g_loot_own, 0, sizeof(g_loot_own));
    memset(&g_loot_carry, 0, sizeof(g_loot_carry));
    if (loot_bits_load("loot_rd", &g_loot_rd) == 0) {
        /* rd 旧 4 键与新前 4 键同名同位 → loot_bits_load 已兼容读到；全缺才走 u64 迁移兜底 */
        (void)loot_bits_migrate_u64("loot_rd", &g_loot_rd);
    }
    if (loot_bits_load("loot_own", &g_loot_own) == 0)
        (void)loot_bits_migrate_u64("loot_own", &g_loot_own);
    if (loot_bits_load("loot_carry", &g_loot_carry) == 0)
        (void)loot_bits_migrate_u64("loot_carry", &g_loot_carry);
#if LOOT_DEV_GIVE_ALL
    /* ★开发期：开局全拥有（用户 10-12 拍板：按键板满编）。**已看位也置满** ——
       否则 21 件全挂「新」、首页礼盒常驻、要逐件翻 21 次才清干净，开发态太吵；
       想看 ●/礼盒效果时把 petns:loot_rd 清零即可。 */
    memset(&g_loot_own, 0, sizeof(g_loot_own));
    for (int i = 0; i < N_LOOT; ++i) lb_set(&g_loot_own, i, true);   /* ★C1：192 位循环置位 */
    g_loot_rd = g_loot_own;
    /* ★2026-10-25：虚拟消耗品也全发放（否则列表看不到后 3 件） */
    {
        const char *vk[] = { "item_luckbell", "item_mealbell", "item_wish",
                             "item_catnip", "item_biscuit", "item_herb", "item_rename", "item_recall" };
        for (int q = 0; q < (int)(sizeof(vk) / sizeof(vk[0])); ++q)
            if (loot_nvs_i32(vk[q], 0) < 1) loot_nvs_set_i32(vk[q], 1);
    }
#endif
    /* ★★R117（一次性回滚 · 用户拍板「关掉按键满编」）：把 `loot_own` 里那批**全解锁时期落的盘**
       换回真实备份。背景：按键板以前每次开机强制满编 178 件（LOOT_DEV_GIVE_ALL=1），
       而测试菜单「全解锁」开一次就把这 178 件按了保存 —— 关开关时恢复的备份（loot_own_bak）
       才是真实进度，但**没人把它写回 loot_own**，所以关掉编译期满编后看到的仍旧是满编。
       这里做一次性回滚：bak 存在且与当前不同 → 覆盖 own（同时把「已看」位收进交集，
       免得 178 件全挂「新」），落 `loot_rev=3` 做闸。
       ⚠ 只回滚**不一致**的情况：触屏板 bak == own，走不到这里，进度零影响。
       ★R209（用户令 2026-11-13）：**删除「无备份 + 拥有 ≥90% → 清空」那条启发式**——
         用户原话「攒到 90% 为什么会把收藏清空？把这一段删掉」。
         它本意是清掉早期编译期满编留下的假进度，但没有备份时无法区分"假满编"和
         "真玩家攒到九成"，属于宁可误伤也可能误伤的逻辑，直接删掉；
         备份回滚这条保留（它只会把进度换回真实备份，不制造清空）。 */
    if (loot_nvs_i32("loot_rev", 0) < 3) {
        loot_bits_t bk; memset(&bk, 0, sizeof(bk));
        const int got = loot_bits_load("loot_own_bak", &bk);
        if (got > 0 && (bk.lo != g_loot_own.lo || bk.hi != g_loot_own.hi || bk.hi2 != g_loot_own.hi2)) {
            const int before = loot_owned_cnt();
            g_loot_own = bk;
            g_loot_rd.lo &= g_loot_own.lo;
            g_loot_rd.hi &= g_loot_own.hi;
            g_loot_rd.hi2 &= g_loot_own.hi2;
            loot_bits_save("loot_own", &g_loot_own);
            loot_bits_save("loot_rd", &g_loot_rd);
            ESP_LOGW(TAG, "★ 收藏品进度回滚到「全解锁」前的真实备份：%d → %d 件",
                     before, loot_owned_cnt());
        }
        loot_nvs_set_i32("loot_rev", 3);
    }
    /* ★2026-10-25 迁移：蒲公英/安眠香草 位图件 → 计数制（旧拥有位 → 数量 1，位图清位）。 */
    if (lb_get(&g_loot_own, 21) && loot_nvs_i32("item_dandelion", -1) == -1) {
        loot_nvs_set_i32("item_dandelion", 1);
        lb_set(&g_loot_own, 21, false);
        loot_bits_save("loot_own", &g_loot_own);
    }
    if (lb_get(&g_loot_own, 22) && loot_nvs_i32("item_sleepherb", -1) == -1) {
        loot_nvs_set_i32("item_sleepherb", 1);
        lb_set(&g_loot_own, 22, false);
        loot_bits_save("loot_own", &g_loot_own);
    }
    /* ★2026-10-26 迁移：旧「幸运币」位图件 → 「召回币」计数制（bit → item_recall=1，位图清位）。 */
    if (lb_get(&g_loot_own, LOOT_I_RECALL) && loot_nvs_i32("item_recall", -1) == -1) {
        loot_nvs_set_i32("item_recall", 1);
        lb_set(&g_loot_own, LOOT_I_RECALL, false);
        loot_bits_save("loot_own", &g_loot_own);
    }
    /* ★2026-10-26 修：开机时 cheat 仍=1 → 虚拟消耗品计数补齐 1（与「开机即满编」口径一致，
       否则烧完新固件重启后消耗品页仍只有 5 件，要再拨一次开关才齐） */
    if (loot_nvs_i32("loot_cheat", 0)) {
        const char *vk[] = { "item_luckbell", "item_mealbell", "item_wish",
                             "item_catnip", "item_biscuit", "item_herb", "item_rename", "item_recall" };
        for (int q = 0; q < (int)(sizeof(vk) / sizeof(vk[0])); ++q)
            if (loot_nvs_i32(vk[q], 0) < 1) loot_nvs_set_i32(vk[q], 1);
    }
    /* 携带位图自愈：位必须落在「已拥有 + 可携带」交集内（老数据/表序变更防脏） */
    for (int i = 0; i < N_LOOT; ++i)
        if (loot_is_carried(i) && (!loot_is_owned(i) || !g_loot[i].carry))
            lb_set(&g_loot_carry, i, false);
    while (loot_carry_cnt() > bag_cap()) {               /* 上限兜底（动态容量）：从高位往下一件件摘 */
        for (int i = N_LOOT - 1; i >= 0; --i)
            if (loot_is_carried(i)) {
                /* ★R115：容量 8→6 后，老存档超员的件会被静默摘掉 —— 必须打日志，
                   否则玩家只会发现「我明明带着的东西效果没了」，无从追查。 */
                ESP_LOGW(TAG, "背包超容（%d > %d）→ 自动放回「%s」",
                         loot_carry_cnt(), bag_cap(), g_loot[i].name);
                lb_set(&g_loot_carry, i, false);
                break;
            }
    }
    ESP_LOGI(TAG, "收藏品：%d 件 · 拥有 %d · 携带 %d · 已看位图 0x%016llX · 未读 %d · 全解锁开关 %s",
             N_LOOT, loot_owned_cnt(), loot_carry_cnt(),
             (unsigned long long)g_loot_rd.lo, loot_unread(), loot_nvs_i32("loot_cheat", 0) ? "开" : "关");
    /* ★R117 诊断（用户报「没解锁的成就也亮勾」）：把「当前拥有」与「全解锁前的真实备份」
       两个位图都打出来 —— 全解锁开关的快照是在**开机强制满编之后**拍的，
       所以按键板（LOOT_DEV_GIVE_ALL=1）关掉开关也回不到真实进度，只能从这里看出来。 */
    {
        loot_bits_t bk; memset(&bk, 0, sizeof(bk));
        const int got = loot_bits_load("loot_own_bak", &bk);
        if (got > 0)
            ESP_LOGI(TAG, "拥有位图：当前 %016llX%016llX%016llX · 全解锁备份 %016llX%016llX%016llX",
                     (unsigned long long)g_loot_own.hi2, (unsigned long long)g_loot_own.hi,
                     (unsigned long long)g_loot_own.lo,
                     (unsigned long long)bk.hi2, (unsigned long long)bk.hi,
                     (unsigned long long)bk.lo);
        else
            ESP_LOGI(TAG, "拥有位图：当前 %016llX%016llX%016llX · 全解锁备份 无（从未开过全解锁）",
                     (unsigned long long)g_loot_own.hi2, (unsigned long long)g_loot_own.hi,
                     (unsigned long long)g_loot_own.lo);
    }
}

/* ★★2026-11-12 用户令（测试菜单「全部清零」）：**明信片和收藏品一起清零**。
   与 loot_cheat_toggle()（开关式全解锁/还原）无关 —— 这是「真重开」：
   拥有位 / 已读位 / 携带位全部归零并落 NVS，收藏品从空箱重新收集；
   明信片由调用方（main.c 测试菜单那一行）一起清。
   顺手把「全解锁」开关关掉，否则界面上还显示「开」，对不上实际状态。 */
void loot_clear_all(void)
{
    memset(&g_loot_own,   0, sizeof(g_loot_own));
    memset(&g_loot_rd,    0, sizeof(g_loot_rd));
    memset(&g_loot_carry, 0, sizeof(g_loot_carry));
    loot_bits_save("loot_own",   &g_loot_own);
    loot_bits_save("loot_rd",    &g_loot_rd);
    loot_bits_save("loot_carry", &g_loot_carry);
    loot_nvs_set_i32("loot_cheat", 0);
    loot_nvs_set_i32("dry_n",   0);
    loot_nvs_set_i32("new_dry", 0);          /* 伪随机倾斜计数也归零，重开按新进度算 */
    /* ★★2026-11-12 用户口径：清零与「全解锁」是**对立**的 ——
       清完再点全解锁 = 全都有；再关掉全解锁 = 回到「全都没有」。
       所以「全解锁」的三份备份也要一起抹成空：否则「先开全解锁 → 再清零 → 关全解锁」
       会把清零前的旧进度恢复回来（那正好是用户不想看到的）。 */
    {
        loot_bits_t bk;
        memset(&bk, 0, sizeof(bk));
        loot_bits_save("loot_own_bak", &bk);
        loot_nvs_set_blob("post_rd_bak",  g_post_rd,  sizeof(g_post_rd));
        loot_nvs_set_blob("post_got_bak", g_post_got, sizeof(g_post_got));
    }
    ESP_LOGW(TAG, "★★ 全部清零（明信片 + 收藏品 · 拥有 0/%d · 全解锁备份已抹空）", N_LOOT);
}

void loot_cheat_toggle(void)
{
    const int on = loot_nvs_i32("loot_cheat", 0);
    if (!on) {
        loot_bits_save("loot_own_bak", &g_loot_own);           /* ★C1：快照真实进度 */
        for (int i = 0; i < N_LOOT; ++i) loot_grant(i);          /* 全解锁（幂等，逐件落 NVS） */
        /* ★2026-10-26 修：虚拟消耗品 6 件计数也发放（原只有编译期 LOOT_DEV_GIVE_ALL 才发 →
           运行时开关打开后消耗品页仍缺猫薄荷/小鱼饼干/提神草） */
        {
            const char *vk[] = { "item_luckbell", "item_mealbell", "item_wish",
                                 "item_catnip", "item_biscuit", "item_herb", "item_rename", "item_recall" };
            for (int q = 0; q < (int)(sizeof(vk) / sizeof(vk[0])); ++q)
                if (loot_nvs_i32(vk[q], 0) < 1) loot_nvs_set_i32(vk[q], 1);
        }
        loot_nvs_set_i32("loot_cheat", 1);
        /* ★★ 2026-10-16 口径改（用户拍板）：开 = 「所有东西全的状态」，方便查看 ——
           明信片**全部收到（post_got 全 1）+ 全部已读（post_rd 全 1）**，不是上批的全未读。
           两张位图都备份，关时一并还原。 */
        loot_nvs_set_blob("post_rd_bak", g_post_rd, sizeof(g_post_rd));
        loot_nvs_set_blob("post_got_bak", g_post_got, sizeof(g_post_got));
        memset(g_post_rd, 0xFF, sizeof(g_post_rd));
        memset(g_post_got, 0xFF, sizeof(g_post_got));
        post_save();
        g_post_unread = post_unread();
        ESP_LOGW(TAG, "★★ 测试开关：全解锁 = 开（原进度 0x%016llX 已备份 · 现拥有 %d/%d · 明信片 %d 张全部收到且已读）",
                 (unsigned long long)loot_nvs_u64("loot_own_bak", 0), loot_owned_cnt(), N_LOOT, N_POST);
    } else {
        memset(&g_loot_own, 0, sizeof(g_loot_own));
        (void)loot_bits_load("loot_own_bak", &g_loot_own);       /* ★C1：恢复真实进度 */
        loot_bits_save("loot_own", &g_loot_own);
        /* ★★ 2026-10-13 任务二 P1-4 修：自愈循环选出的件（carried && (!owned || !carry)）
           **必然命中** loot_carry_set 首条守卫 `!loot_is_owned(i) || !g_loot[i].carry →
           return false` ⇒ 原来这里是空操作：不清位图、不写 NVS、不走 bag_slots_note
           → 关开关后背包页出现「幽灵行+携带中」（bag_slots_rebuild 只查 carried 不过滤拥有），
           loot_carry_cnt() 虚高还可能假报「背包已满」堵死正常上背；只有重启后
           loot_load 的内联手捕（4554 行起）才修复。
           修法（报告口径「仿 loot_load 直接清位 + 落盘」）：绕过拥有性守卫强制摘除，
           一次落盘；槽表由下面既有的 `g_bag_slots_ready = false` 懒重建对齐。 */
        bool healed = false;
        for (int i = 0; i < N_LOOT; ++i)
            if (loot_is_carried(i) && (!loot_is_owned(i) || !g_loot[i].carry)) {
                lb_set(&g_loot_carry, i, false);
                healed = true;
                ESP_LOGW(TAG, "自愈：放回不再拥有的「%s」（强制清携带位）", g_loot[i].name);
            }
        if (healed) loot_bits_save("loot_carry", &g_loot_carry);   /* ★C1 */
        g_bag_slots_ready = false;                               /* 强制槽表下次懒重建（与位图重新对齐） */
        loot_nvs_set_i32("loot_cheat", 0);
        /* 明信片：从备份还原真实已读+收到位图（备份不存在则保持现状） */
        if (loot_nvs_get_blob("post_rd_bak", g_post_rd, sizeof(g_post_rd)) &&
            loot_nvs_get_blob("post_got_bak", g_post_got, sizeof(g_post_got)))
            post_save();
        g_post_unread = post_unread();
        ESP_LOGW(TAG, "★★ 测试开关：全解锁 = 关（恢复原进度 0x%016llX · 拥有 %d/%d · 明信片未读 %d）",
                 (unsigned long long)g_loot_own.lo, loot_owned_cnt(), N_LOOT, post_unread());
    }
}

static void loot_rd_save(void)
{
    loot_bits_save("loot_rd", &g_loot_rd);   /* ★C1：统一 6 段（>128 位的已看位也能落盘了） */
}

bool loot_is_read(int i)
{
    /* ★★2026-10-19：两铃虚拟件**常读**（无未读态，列表不挂 ●；位图容量也不管它们） */
    if (i >= N_LOOT) return true;
    return i >= 0 && i < N_LOOT && lb_get(&g_loot_rd, i);
}

void loot_mark_read(int i)
{
    if (i < 0 || i >= N_LOOT || loot_is_read(i)) return;
    lb_set(&g_loot_rd, i, true);
    loot_rd_save();
    ESP_LOGI(TAG, "收藏品「%s」标为已看（未读剩 %d）", g_loot[i].name, loot_unread());
}

int loot_unread(void)
{
    int n = 0;
    for (int i = 0; i < N_LOOT; ++i) if (loot_is_owned(i) && !loot_is_read(i)) n++;
    return n;
}

int loot_order(int k)
{
    /* ★★2026-10-19：取值域放宽到 N_LOOT+2（两铃虚拟件挂已读段尾 = 列表末尾） */
    int idx[N_LOOT + LOOT_V_N], n = 0;
    for (int i = 0; i < N_LOOT + LOOT_V_N; ++i)   /* ★虚拟消耗品 7 件（LOOT_V_N）*/
        if (loot_is_owned(i) && !loot_is_read(i) && (g_loot_cat < 0 || loot_cat_of(i) == g_loot_cat) && (g_loot_cat != 2 || g_loot_stack < 0 || loot_stack_of(i) == g_loot_stack))
            idx[n++] = i;
    for (int i = 0; i < N_LOOT + LOOT_V_N; ++i)   /* ★虚拟消耗品 7 件（LOOT_V_N）*/
        if (loot_is_owned(i) &&  loot_is_read(i) && (g_loot_cat < 0 || loot_cat_of(i) == g_loot_cat) && (g_loot_cat != 2 || g_loot_stack < 0 || loot_stack_of(i) == g_loot_stack))
            idx[n++] = i;
    if (k < 0 || k >= n) return -1;
    return idx[k];
}

int loot_disp_of(int item)
{
    const int n = loot_owned_cnt();
    for (int q = 0; q < n; ++q) if (loot_order(q) == item) return q;
    return 0;
}

int loot_cat_cnt(int c)
{
    int n = 0;
    for (int i = 0; i < N_LOOT + LOOT_V_N; ++i)   /* ★虚拟消耗品 7 件（LOOT_V_N）*/ if (loot_is_owned(i) && loot_cat_of(i) == c) n++;   /* ★2026-10-26 +6 */
    return n;
}

int loot_cat_unread(int c)
{
    int n = 0;
    for (int i = 0; i < N_LOOT; ++i)
        if (loot_is_owned(i) && !loot_is_read(i) && loot_cat_of(i) == c) n++;
    return n;
}

/* ★R204（用户报「随身物品的二级页没有红点了」）：四堆各自的未读数。
   随身(cat2) 的列表在堆过滤后显示，所以红点必须按堆统计 —— 否则二级页四行全是
   干名字，用户没法知道"新的在哪一堆"。口径与 loot_cat_unread 完全一致。 */
int loot_stack_unread(int k)
{
    int n = 0;
    for (int i = 0; i < N_LOOT; ++i)
        if (loot_is_owned(i) && !loot_is_read(i) && loot_cat_of(i) == 2 &&
            loot_stack_of(i) == k) n++;
    return n;
}

/* 选类 → 进该类列表（列表内顺序/计数自动按类过滤；清光标/视窗交给 go_page 的重置分支） */
void loot_cat_pick(int c)
{
    /* ★2026-10-24 D5：随身物品(1) → 先进四堆二级页；其余类直接进列表（堆过滤清零）。 */
    g_loot_cat = c;
    g_loot_stack = -1;
    go_page(c == 2 ? PG_LOOT_STACK : PG_LOOT_LIST);
}

/* ★D5 二级页：选堆 → 随身列表（堆过滤生效）。 */
void loot_stack_pick(int k) { g_loot_stack = k; go_page(PG_LOOT_LIST); }

/* （R26.1 的「BOOT 框选按钮」口径已废止——R28 用户实机三改：BOOT=切件 / POWER=带上放回，无框选态。） */

/* 纪念品底部两行：y168「获得 MM-DD」/ y186「地点 上海」。无记录 → 「获得日期未知」。 */
void loot_got_draw(int i)
{
    /* ★2026-10-22 用户令（原话）：「不要两行 / 不要那个标签，直接城市的那个，就是上海，
       也不要加『市』」⇒ 单行 = `2026-10-11 上海`；没有记录 = `未知 上海`。 */
    char d[40];                                        /* 容得下 int 极值的 %04d-%02d-%02d */
    int day = 0, place = -1;
    if (loot_got_get(i, &day, &place)) {
        const time_t t = (time_t)day * 86400 + 43200;      /* 取正午，避开时区边界 */
        struct tm tv;
        gmtime_r(&t, &tv);
        snprintf(d, sizeof(d), "%04d-%02d-%02d",
                 tv.tm_year + 1900, tv.tm_mon + 1, tv.tm_mday);
    } else {
        strlcpy(d, T("未知"), sizeof(d));
    }

    const char *pl;
    char pbuf[64];
    if (place >= 0 && place < EXP_N) {
        pl = t_(EXP_TBL[place].name);                      /* 远行目的地（冰岛/京都…，10 站）；
                                                              ★R105：此处与日期 snprintf 成整串后再上屏，
                                                              整串查表命不中 ⇒ 必须先在段内取译文 */
    } else if (g_app.cfg && g_app.cfg->city[0]) {
        strlcpy(pbuf, g_app.cfg->city, sizeof(pbuf));
        const size_t L = strlen(pbuf);                     /* 去掉末尾「市」（UTF-8 E5 B8 82）*/
        if (L >= 3 && (uint8_t)pbuf[L - 3] == 0xE5 &&
                      (uint8_t)pbuf[L - 2] == 0xB8 &&
                      (uint8_t)pbuf[L - 1] == 0x82) pbuf[L - 3] = 0;
        pl = pbuf;
    } else {
        pl = T("家");
    }

    char out[128];                                     /* d(≤39) + 空格 + 城市名(≤63) */
    snprintf(out, sizeof(out), "%s %s", d, pl);
    fb_text_c_fit(ZF_FONT_BODY, 100, 186, out, 184);
    ESP_LOGI(TAG, "   纪念品获得信息：%s", out);
}

const char *loot_desc_of(int i)
{
    return (i >= 0 && i < N_LOOT && g_loot_desc[i][0]) ? g_loot_desc[i] : NULL;
}

void loot_detail_carry_action(void)
{
    const int cnt = loot_owned_cnt();
    if (cnt <= 0) return;
    int k = g_loot_view % cnt;
    if (k < 0) k = 0;
    const int i = loot_order(k);
    if (i < 0) return;
    /* ★★2026-10-19 虚拟消耗品（用户拍板 B 方案）：详情页「用掉它」= 真实消耗 1 个。
       · 好运铃：用掉 → 置 luck_coin=2，下趟必带回一件；
       · 改名卡：用掉 → 名字清空 + 开热点让手机改名；
       · 开饭铃：盆里有食（BOWL_FULL）才可点 → 猫马上来吃。
       用完还有存货 → 留在详情刷新数量；用完归零 → 藏品从列表消失，回列表。 */
    if (i == LOOT_V_LUCK) {
        /* ★2026-10-26 用户令：好运铃回原槽（不再是改名卡）。
           luck_coin 三态：0 未装备 / 2 已用待生效 / 1 本趟生效中。 */
        bell_add("item_luckbell", -1);
        loot_nvs_set_i32("luck_coin", 2);
        ESP_LOGW(TAG, "★★ 好运铃用掉：已装备（待下趟生效 · 剩 %d）", loot_nvs_i32("item_luckbell", 0));
        if (loot_is_owned(i)) page_render(false); else go_page(PG_LOOT_LIST);
        return;
    }
    if (i == LOOT_V_RENAME) {
        /* ★2026-10-26 用户拍板：占原晴天符槽位的「改名卡」（原好运铃件已删，不再新增道具）：
           用掉 → 名字立刻变 "-" → 开热点让手机改名（复用配网 /name 页）。
           ★顺序：**先开热点成功才消耗**（开不起来就留着下次用，不白花 50 币）。
           不取名也安全：named_once 已清，下次配网会再弹取名。 */
        if (!ws_name_open()) {
            ESP_LOGW(TAG, "★ 改名卡：热点开不起来 → 本次不消耗，留着下次用");
            page_render(false);
            return;
        }
        bell_add("item_rename", -1);
        pet_name_clear();
        /* ★★ R94（用户令）：「取名卡使用了之后，应该立即跳那个页面」——
           热点开起来之后立刻上屏「给猫猫取名」引导屏（复用配网流程那张 scr_B），
           别让用户对着一个突然变成横杠的主页发愣，以为还得再走一遍配网。
           ⚠ 画完必须直接 return：下面那句 page_render/go_page 会立刻把引导屏盖掉。 */
        pet_draw_naming_wait();
        ESP_LOGW(TAG, "★★ 改名卡用掉（剩 %d）：手机连热点后自动打开取名页", loot_nvs_i32("item_rename", 0));
        return;
    }
    if (i == LOOT_V_CATNIP) {
        /* ★2026-10-26 用户口径：猫薄荷 = 中等增加亲密度（+3）；小鱼饼干 = 略微（+1） */
        bell_add("item_catnip", -1);
        if (!g_app.st.outside) pet_touch();
        (void)ws_intimacy_add(3);
        ESP_LOGW(TAG, "★★ 猫薄荷用掉：亲密度 +3（剩 %d）", loot_nvs_i32("item_catnip", 0));
        if (loot_is_owned(i)) page_render(false); else go_page(PG_LOOT_LIST);
        return;
    }
    if (i == LOOT_V_BISCUIT) {
        bell_add("item_biscuit", -1);
        (void)ws_intimacy_add(1);
        ESP_LOGW(TAG, "★★ 小鱼饼干用掉：亲密度 +1");
        if (loot_is_owned(i)) page_render(false); else go_page(PG_LOOT_LIST);
        return;
    }
    if (i == LOOT_V_HERB) {
        bell_add("item_herb", -1);
        /* ★2026-10-26 用户口径：提神草 = **大幅安抚**（降 3 档，不直接归零），
           与蒲公英（降 1 档）/ 安眠香草（降 2 档）形成 小/中/大 三档。 */
        g_rtc_mood_lvl = (g_rtc_mood_lvl > 3) ? g_rtc_mood_lvl - 3 : 0;
        ESP_LOGW(TAG, "★★ 提神草用掉：心情大幅安抚（降 3 档 → 现 %d）", g_rtc_mood_lvl);
        if (loot_is_owned(i)) page_render(false); else go_page(PG_LOOT_LIST);
        return;
    }
    if (i == LOOT_V_MEAL) {
        /* ★R125（用户令 2026-11-03）：猫不在家时**不扣铃**（原来扣了但猫回来才吃，
           食物还可能已过 24h 期 ⇒ 真白扣）。按钮侧同样判 outside（见 main.c 可用判据）。 */
        if (g_app.st.outside) {
            ESP_LOGW(TAG, "★★ 开饭铃：猫不在家 → 不消耗（回来再用）");
            page_render(false);
            return;
        }
        if (g_bowl.state != BOWL_FULL) return;      /* 盆里没食 = 无法使用 */
        bell_add("item_mealbell", -1);
        g_bowl.eat_at = g_bowl.ts;
        bowl_save();
        ESP_LOGW(TAG, "★★ 开饭铃用掉（剩 %d）：猫马上来吃", loot_nvs_i32("item_mealbell", 0));
        if (loot_is_owned(i)) page_render(false); else go_page(PG_LOOT_LIST);
        return;
    }
    if (loot_is_usable(i)) {
        /* ★★使用型（B 真消耗）：当场生效 + 消耗 → 回列表（藏品已从收藏箱消失） */
        if (i == LOOT_I_RECALL) {
            /* ★2026-10-26 用户令：112 号「召回币」——用了猫立刻回家（secs=0），
               并马上投递一次后台同步，让 step_life 当场结算（掉落/明信片/主页刷回）。
               猫不在外面 / NVS 写失败 → 不消耗，留着下次用（按钮显示「无法使用」）。 */
            if (!ws_out_recall(&g_app.st, 0)) {
                ESP_LOGW(TAG, "★★ 召回币：猫不在外面或写入失败 → 不消耗，留着下次用");
                page_render(false);
                return;
            }
            loot_consume(i);
            ESP_LOGW(TAG, "★★ 召回币用掉（剩 %d）：立即回家", loot_nvs_i32("item_recall", 0));
            if (g_app.cfg) ws_sync_begin(g_app.cfg, &g_app.st);   /* 立刻投递结算 */
            if (loot_is_owned(i)) page_render(false); else go_page(PG_LOOT_LIST);
            return;
        }
        if (i == LOOT_I_DANDELION) {
            if (g_rtc_mood_lvl > 0) g_rtc_mood_lvl -= 1;      /* 吹一吹：气立刻消一档 */
            ESP_LOGW(TAG, "★★ 蒲公英：气消一档（现 %d）", g_rtc_mood_lvl);
        } else if (i == LOOT_I_SLEEP_HERB) {
            g_rtc_mood_lvl = (g_rtc_mood_lvl > 1) ? g_rtc_mood_lvl - 2 : 0;
            ESP_LOGW(TAG, "★★ 安眠香草：气消两档（现 %d）", g_rtc_mood_lvl);
        }
        loot_consume(i);
        go_page(PG_LOOT_LIST);
        return;
    }
    if (i == LOOT_V_WISH) {
        /* ★★★ 2026-10-26 修（越界读 + 空按钮）：许愿签没有"当场用掉"分支 —— 它是**下次出门
           自动消耗**（loot_trip_start_cb 里 item_wish>0 时 -1 并改去向）。原来这个虚拟件会一路
           落到函数末尾的 `g_loot[i].carry`，而 180 号不在 g_loot[] 里 = 越界读；按钮点了也没反应。
           这里明确回一句"下次出门自动生效"，不消耗、不越界。 */
        ESP_LOGW(TAG, "★★ 许愿签：下次出门自动生效，不在此消耗");
        page_render(false);
        return;
    }
    /* ★R113 守卫：虚拟消耗品（LOOT_V_*，下标 ≥ N_LOOT）靠上面一串 if 逐支 return 兜住，
       一旦将来新增虚拟件忘了加分支，就会一路落到下面的 g_loot[i] 越界读。
       代价一行，换掉一类"加个新道具就随机死机"的隐患。 */
    if (i < 0 || i >= N_LOOT) return;
    if (!g_loot[i].carry) return;                /* 被动件没有按钮（理论进不来） */
    if (loot_is_carried(i)) {
        loot_carry_set(i, false);                /* 放回收藏 → 标签/按钮局刷重画 */
        page_render(false);
        return;
    }
    if (loot_carry_set(i, true)) {               /* 背上（未满 4） */
        page_render(false);
        return;
    }
    g_bag_from = 0;                              /* 来源 = 收藏详情（替换完成后回详情） */
    g_bag_new = i;                               /* 满 4 → 替换选择流程（§2.1） */
    g_bag_sel = -1;
    go_page(PG_BAG_SWAP);
}

int loot_cnt_get(const char *key)
{
    return loot_nvs_i32(key, 0);
}

/* 计数器读改写（feed_cnt / pet_cnt / trip_total / fd_cnt / trip_dest 共用） */
int loot_cnt_add(const char *key, int delta)
{
    const int v = loot_cnt_get(key) + delta;
    loot_nvs_set_i32(key, v);
    return v;
}

/* 定去向（出门那刻掷一次）：城市默认；山野/海滨要**拥有小背包**、异国要**携带旧邮票**。
 * ★2026-10-18 用户令：小背包是前期道具 → 改成「**拥有即解锁**」，不再要求背在身上
 *   （原来的 `loot_is_carried` 让玩家必须先占一个背包格才能出远门，太苛刻）。 */
static int loot_pick_dest(void)
{
    int cand[4], w[4], n = 0, tot = 0;
    cand[n] = LOOT_SER_CITY; w[n] = DEST_W_CITY; tot += w[n]; n++;
    if (loot_is_owned(LOOT_I_BAG)) {
        cand[n] = LOOT_SER_WILD; w[n] = DEST_W_WILD; tot += w[n]; n++;
        cand[n] = LOOT_SER_SEA;  w[n] = DEST_W_SEA;  tot += w[n]; n++;
    }
    if (loot_is_carried(LOOT_I_STAMP)) {
        cand[n] = LOOT_SER_ABROAD; w[n] = DEST_W_ABROAD; tot += w[n]; n++;
    }
    unsigned r = esp_random() % (unsigned)tot;
    for (int i = 0; i < n; ++i) {
        if (r < (unsigned)w[i]) return cand[i];
        r -= (unsigned)w[i];
    }
    return LOOT_SER_CITY;
}

static const loot_drop_t LOOT_DROP_CITY[] = {          /* 城市（空 130 → 实测空手 29%）原4 + 城市属性14 + 旅途属性13 + 远行前置6(通用4 + 信物2) */
    { LOOT_I_BUTTON, 20 }, { LOOT_I_KEY, 15 }, { LOOT_I_FLOWER, 10 }, { LOOT_I_STAR, 8 },
    { LOOT_I_EGG_STONE, 6 }, { LOOT_I_DOORMAT, 6 }, { LOOT_I_SILL_PAD, 6 }, { LOOT_I_PENCIL, 6 },
    { LOOT_I_PHOTO_TOGETHER, 6 }, { LOOT_I_SHARED_UMBRELLA, 6 }, { LOOT_I_WAND, 6 }, { LOOT_I_COIN_PURSE, 6 },
    { LOOT_I_MINI_UMBRELLA, 6 }, { LOOT_I_CUSHION, 6 }, { LOOT_I_CAT_BELL_DRUM, 6 }, { LOOT_I_OLD_PHOTO, 6 },
    { LOOT_I_LANTERN, 6 }, { LOOT_I_RATTAN_CHAIR, 6 },
    { LOOT_I_WING, 5 }, { LOOT_I_STAMP, 5 }, { LOOT_I_STARFISH, 5 }, { LOOT_I_TERU, 5 },
    { LOOT_I_HAND_WARMER, 5 }, { LOOT_I_STAMP_ALBUM, 5 }, { LOOT_I_WISH_COIN, 5 }, { LOOT_I_POCKET_WATCH, 5 },
    { LOOT_I_ATLAS, 5 }, { LOOT_I_TRAVEL_HAT, 5 }, { LOOT_I_TREASURE_MAP, 5 }, { LOOT_I_HORSESHOE, 5 },
    { LOOT_I_KNOT, 5 },
    /* ★★ 2026-10-20：远行前置件不再只靠商店 —— 近郊低概率也能掉（商店为保底路径）。
       通用旅行件（护照总闸+通行档）三表都放权重 4；信物按地标就近：冰岛指引物/沙漠地图=城市属性 权重 3 */
    { LOOT_I_PASSPORT, 4 }, { LOOT_I_DINGHY, 4 }, { LOOT_I_TICKET, 4 }, { LOOT_I_COMPASS, 4 },
    { LOOT_I_ICELAND_TOKEN, 3 }, { LOOT_I_DESERT_MAP, 3 },
    /* ★2026-10-25 阶段6：52 批城市件入池（此前 27 件接线效果却掉不出来）*/
    /* ★2026-10-26 用户令「20 件无渠道的补渠道」：76 折叠地图 / 98 旧车票根（城市停留）/ 
       90 旅行披风（前往各地，三表通用）/ 87 旧马灯（夜行）/ 88 折叠雨衣（雨天）/ 112 召回币 */
    { 76, 6 }, { 98, 6 }, { 87, 5 }, { 88, 5 }, { 90, 4 }, { LOOT_I_RECALL, 3 },
    { 77, 6 }, { 78, 6 }, { 79, 6 }, { 80, 5 }, { 91, 6 }, { 92, 6 },
    { 93, 6 }, { 102, 6 }, { 103, 6 }, { 126, 6 },
    /* ★2026-10-26 修（用户令「掉率回到设计值」）：此段原为**重复追加**（脚本二次插入事故），
        同一批 10 件权重被算了 2 次 ⇒ 实际掉率是设计值 2 倍。已删重复块。 */
    /* ★2026-10-27 B1：100 万用工具 / 101 回忆相册 原无获取通道（成就"万能与回忆"永不可达），
       进城市掉落表（与同批 102/103/126 同权重 6）。 */
    { 100, 6 }, { 101, 6 },
};

static const loot_drop_t LOOT_DROP_WILD[] = {          /* 山野（空 130 → 实测空手 32%）原4 + 山野属性12 + 旅途属性13 + 远行前置5(通用4 + 信物1) */
    { LOOT_I_PINE, 20 }, { LOOT_I_ACORN, 15 }, { LOOT_I_GINKGO, 10 }, { LOOT_I_STONE, 8 },
    { LOOT_I_DANDELION, 6 }, { LOOT_I_SLEEP_HERB, 6 }, { LOOT_I_SPYGLASS, 6 }, { LOOT_I_CLOVER, 6 },
    { LOOT_I_STAFF, 6 }, { LOOT_I_BARK, 6 }, { LOOT_I_MOSS, 6 }, { LOOT_I_BOOTS, 6 },
    { LOOT_I_SCARF, 6 }, { LOOT_I_SKETCHBOOK, 6 }, { LOOT_I_MOONSTONE, 6 }, { LOOT_I_IVY, 6 },
    { LOOT_I_WING, 5 }, { LOOT_I_STAMP, 5 }, { LOOT_I_STARFISH, 5 }, { LOOT_I_TERU, 5 },
    { LOOT_I_HAND_WARMER, 5 }, { LOOT_I_STAMP_ALBUM, 5 }, { LOOT_I_WISH_COIN, 5 }, { LOOT_I_POCKET_WATCH, 5 },
    { LOOT_I_ATLAS, 5 }, { LOOT_I_TRAVEL_HAT, 5 }, { LOOT_I_TREASURE_MAP, 5 }, { LOOT_I_HORSESHOE, 5 },
    { LOOT_I_KNOT, 5 },
    /* ★★ 2026-10-20：远行前置件近郊低概率掉落（商店为保底）。通用旅行件三表都放权重 4；
       信物按地标就近：樱花路引=山野属性 权重 3 */
    { LOOT_I_PASSPORT, 4 }, { LOOT_I_DINGHY, 4 }, { LOOT_I_TICKET, 4 }, { LOOT_I_COMPASS, 4 },
    { LOOT_I_SAKURA_GUIDE, 3 },
    /* ★2026-10-25 阶段6：52 批山野件入池 */
    /* ★2026-10-26 用户令：87 旧马灯 / 88 折叠雨衣 / 90 旅行披风（三表通用）+ 112 召回币 */
    { 87, 5 }, { 88, 5 }, { 90, 4 }, { LOOT_I_RECALL, 3 },
    { 86, 6 }, { 89, 6 }, { 114, 6 }, { 115, 5 }, { 116, 5 }, { 117, 5 },
    { 118, 6 }, { 119, 6 }, { 120, 5 }, { 121, 5 },
    /* ★2026-10-26 修：同上，重复追加块已删（掉率回设计值）*/
};

static const loot_drop_t LOOT_DROP_SEA[] = {           /* 海滨（空 110 → 实测空手 33%）原3 + 海滨属性4 + 旅途属性13 + 远行前置5(通用4 + 信物1) */
    { LOOT_I_SHELL, 20 }, { LOOT_I_CONCH, 15 }, { LOOT_I_BOTTLE, 8 },
    { LOOT_I_DRIFTWOOD, 6 }, { LOOT_I_SEAGLASS, 6 }, { LOOT_I_KITE_SPOOL, 6 }, { LOOT_I_SHELL_CHIME, 6 },
    { LOOT_I_WING, 5 }, { LOOT_I_STAMP, 5 }, { LOOT_I_STARFISH, 5 }, { LOOT_I_TERU, 5 },
    { LOOT_I_HAND_WARMER, 5 }, { LOOT_I_STAMP_ALBUM, 5 }, { LOOT_I_WISH_COIN, 5 }, { LOOT_I_POCKET_WATCH, 5 },
    { LOOT_I_ATLAS, 5 }, { LOOT_I_TRAVEL_HAT, 5 }, { LOOT_I_TREASURE_MAP, 5 }, { LOOT_I_HORSESHOE, 5 },
    { LOOT_I_KNOT, 5 },
    /* ★★ 2026-10-20：远行前置件近郊低概率掉落（商店为保底）。通用旅行件三表都放权重 4；
       信物按地标就近：贡多拉船票=海滨属性 权重 3 */
    { LOOT_I_PASSPORT, 4 }, { LOOT_I_DINGHY, 4 }, { LOOT_I_TICKET, 4 }, { LOOT_I_COMPASS, 4 },
    { LOOT_I_GONDOLA_TICKET, 3 },
    /* ★2026-10-25 阶段6：52 批海滨件入池（海滨 25→35 项，补齐与城市/山野的差距）*/
    /* ★2026-10-26 用户令：127 渔夫浮标（海滨带回率）+ 88 折叠雨衣 / 90 旅行披风 + 112 召回币 */
    { 127, 6 }, { 88, 4 }, { 90, 4 }, { LOOT_I_RECALL, 3 },
    { 81, 6 }, { 82, 6 }, { 83, 5 }, { 84, 6 }, { 85, 6 }, { 94, 6 },
    { 95, 5 }, { 96, 6 }, { 97, 5 }, { 99, 6 },
    /* ★2026-10-26 修：同上，重复追加块已删（掉率回设计值）*/
};

int loot_roll_drop(int dest)
{
    const loot_drop_t *tb;
    int n, empty_w;
    if      (dest == LOOT_SER_WILD) { tb = LOOT_DROP_WILD; n = (int)(sizeof(LOOT_DROP_WILD) / sizeof(loot_drop_t)); empty_w = LOOT_DROP_EMPTY_WILD; }
    else if (dest == LOOT_SER_SEA)  { tb = LOOT_DROP_SEA;  n = (int)(sizeof(LOOT_DROP_SEA)  / sizeof(loot_drop_t)); empty_w = LOOT_DROP_EMPTY_SEA;  }
    else if (dest == LOOT_SER_CITY) { tb = LOOT_DROP_CITY; n = (int)(sizeof(LOOT_DROP_CITY) / sizeof(loot_drop_t)); empty_w = LOOT_DROP_EMPTY_CITY; }
    else if (dest == LOOT_SER_ABROAD) {
        /* ★R125（用户令 2026-11-03）：异国不再「100% 空手」—— 旧邮票把去向拉到异国后
           整趟 0 件，玩家体感「带了邮票反而白出门」。改为从三张近郊表随机一张照常掷。 */
        return loot_roll_drop((int)(esp_random() % 3u));
    } else return -1;
    /* ★2026-10-21 新玩法③：**连续空手保底** —— 上一趟空手（dry_n≥1）时把"空"档权重清零，
       本趟必出一件。配合 loot_trip_return_cb 尾部的 dry_n 记账：
       空手 → +1，有件 → 归零 ⇒ 最多**连空一趟**，不会出现"连着三趟白等"的挫败段。 */
    if (loot_nvs_i32("dry_n", 0) >= 1) empty_w = 0;
    /* ★★2026-10-26 复查 A5 修：候选数组原写死 40 —— 而三张掉落表实测已长到
       CITY 57 / WILD 54 / SEA 45 行 ⇒ **第 41 行起整段被静默丢弃**
       （80/91/92/93/102/103/126 七件城市件、118..121 四件山野件永远掷不到）。
       改成尺寸驱动；DROP_MAX 由 _Static_assert 兜着，表再长了编不过而不是悄悄截断。 */
#define LOOT_DROP_MAX 64
    _Static_assert((int)(sizeof(LOOT_DROP_CITY) / sizeof(loot_drop_t)) <= LOOT_DROP_MAX &&
                   (int)(sizeof(LOOT_DROP_WILD) / sizeof(loot_drop_t)) <= LOOT_DROP_MAX &&
                   (int)(sizeof(LOOT_DROP_SEA)  / sizeof(loot_drop_t)) <= LOOT_DROP_MAX,
                   "掉落表超过 LOOT_DROP_MAX —— 抬高这个上限（别调回去让尾部被截断）");
    int idx[LOOT_DROP_MAX], wt[LOOT_DROP_MAX], m = 0, tot = empty_w;
    if (tot < 0) tot = 0;                          /* ★复查 C6：防御 —— tot 为 0 时 % 会直接崩 */
    const int dp = loot_fx_drop_pct(dest);          /* ★2026-10-27 B7：掉率倍率只取决于 dest，提出循环避免每轮全量重算 */
    /* ★★2026-11-12 用户令（实机报「后期一直只收到旧的，剩几件永远收不到」）：
       伪随机倾斜（PRD）。旧口径是「已拥有件照常入池、权重与新件完全一样」，
       后期剩 5 件未拥有时，单次命中率只有 5/175 上下 —— 抽不到才是常态。
       现在按「连续没抽到新件的趟数」逐级抬高未拥有件的权重，抽到就归零；
       已拥有件保持原权重（重复件折算贝币仍是收益，不是废件）。 */
    const int new_dry = loot_nvs_i32("new_dry", 0);
    for (int i = 0; i < n && m < LOOT_DROP_MAX; ++i) {
        /* ★2026-10-20：删掉 `if (loot_is_owned(...)) continue;` —— 详见本函数上方注记。
           已拥有件**照常入池**（D1 折算制：重复件由 loot_grant() 判失败 → 折算成贝币）。 */
        if (!loot_alive(tb[i].idx)) continue;      /* ★占位件不入池（稳定 ID 口径） */
        int w = tb[i].w;
        /* ★R119 修：观察家「稀有 ×2」必须按**原始权重**判定。原代码先乘掉率倍率再判
           w<=8 —— 在 ×1.5/×2 的目的地，原本 8 的稀有件被抬到 12 ⇒ 判定失败、反而**不翻倍**，
           效果与描述相反。 */
        const bool rare = (tb[i].w <= 8);
        /* ★A1 掉率倍率（机制文档 §4.1④）：树皮/苔藓 → 山野×1.5 / 松果 → 山野×2 /
           浮木 → 海滨×1.5 / 海玻璃 → 海滨×2 —— 查聚合核心 loot_fx_drop_pct()。 */
        w = w * dp / 100;
        if (g_persona == PERSONA_OBSERVER && rare) w *= 2;   /* 观察家：稀有 ×2 */
        /* ★2026-11-12 伪随机倾斜：未拥有件基础 ×4；连 3/6/10/15 趟没新件再逐级翻倍
           （累计 ×8 / ×16 / ×64 / ×256）。剩 5 件时，连 10 趟后命中率已到 ~85%，
           连 15 趟几乎必出 —— 但它是「权重」不是硬保底，不会显得像脚本发牌。 */
        if (!loot_is_owned(tb[i].idx)) {
            w *= 4;
            if (new_dry >= 3)  w *= 2;
            if (new_dry >= 6)  w *= 2;
            if (new_dry >= 10) w *= 4;
            if (new_dry >= 15) w *= 4;
        }
        idx[m] = tb[i].idx; wt[m] = w; tot += w; m++;
    }
    if (tot <= 0) return -1;                       /* ★复查 C6：池空 ⇒ 走「空」档，别 % 0 */
    unsigned r = esp_random() % (unsigned)tot;
    int pick = -1;
    for (int i = 0; i < m; ++i) {
        if (r < (unsigned)wt[i]) { pick = idx[i]; break; }
        r -= (unsigned)wt[i];
    }
    /* ★2026-11-12 用户令：伪随机倾斜的计数（每抽一次记一次）。
       抽到新件 → 归零；抽到重复件 → +1；空手不动；消耗品叠加不算重复挫败。
       loot_roll_drop() 是近郊与远行**共用**的唯一入口 ⇒ 在这里记一次就两版全覆盖，
       不用去改动调用方，也不会因为新增掉落路径而漏记账。 */
    if (pick >= 0 && !loot_stack_owned(pick)) {
        const int nd = loot_nvs_i32("new_dry", 0);
        if (!loot_is_owned(pick)) { if (nd) loot_nvs_set_i32("new_dry", 0); }
        else                      loot_nvs_set_i32("new_dry", nd + 1);
    }
    return pick;                                   /* -1 = 落「空」档 */
}

/* 里程碑复查（每次相关事件后调一次，每件只发一次）：小鱼干 喂食≥5 · 毛线球 抚摸≥100 ·
 * 铃铛 亲密度≥70 · 小背包 出门≥3（trip_start 里顺路查）。 */
void loot_milestone_check(const char *why)
{
    char via[64];
    if (loot_cnt_get("feed_cnt") >= 5 && !loot_is_owned(LOOT_I_FISH) && loot_grant(LOOT_I_FISH))
        snprintf(via, sizeof(via), T("%s（喂食累计 5 次）"), why);
    else if (loot_cnt_get("pet_cnt") >= 100 && !loot_is_owned(LOOT_I_YARN) && loot_grant(LOOT_I_YARN))
        snprintf(via, sizeof(via), T("%s（抚摸累计 100 次）"), why);
    else if (g_app.st.intimacy >= 70 && !loot_is_owned(LOOT_I_BELL) && loot_grant(LOOT_I_BELL))
        snprintf(via, sizeof(via), T("%s（亲密度 70）"), why);
    else return;
    ESP_LOGW(TAG, "★★ 里程碑收藏品到手：%s", via);
}

static int s_plan_ed = -1;      /* -1 = 无远行计划 */

static int s_plan_dest = -1;    /* -1 = 无近郊计划 */

int loot_trip_plan_cb(time_t out_at, int weather)
{
    (void)out_at; (void)weather;
    s_plan_ed = exp_pick_ready();
    if (s_plan_ed >= 0) return LOOT_SER_ABROAD;
    s_plan_dest = loot_pick_dest();
    return s_plan_dest;
}

/* ① 出门那一刻（net_sync step_life 经钩子回调）：★B2 先判远行套装（齐全→自动出发，
 *    返回覆盖小时数）；普通短途走原路：trip_total++ → 里程碑（小背包）→ 定去向落 NVS →
 *    雪天+山野必带雪花结晶（一次性）。
 *    ★2026-10-18：去向/远行**沿用 plan 钩子的预告**（不再重掷），保证场合加成与去向一致。 */
int loot_trip_start_cb(time_t out_at, int weather)
{
    loot_nvs_set_i32("exp_toast", 0);              /* 出门即清「套装集齐」提示（无论走不走远行） */
    const int ed = (s_plan_ed >= 0) ? s_plan_ed : exp_pick_ready();
    s_plan_ed = -1;
    if (ed >= 0) return exp_start_trip(ed, out_at);   /* ★B2：套装齐全 → 这次出门直接变远行 */
    const int total = loot_cnt_add("trip_total", 1);
    if (total >= 3 && !loot_is_owned(LOOT_I_BAG)) {
        loot_grant(LOOT_I_BAG);                    /* 出门累计 3 次必得（第 3 次出发时就到手）；
                                                      注意 loot_grant 只置**拥有位** —— 见下方 P2-6 订正 */
    }
    loot_milestone_check("出门");
    /* ★2026-10-13 任务二 P2-6 修（报告原文：「注释与实现不符」）：原注释写「小背包刚到手
       （第 3 次）→ 这趟就能去山野/海滨」是**假的** —— loot_pick_dest 判的是**携带位**
       （loot_is_carried，见该函数 10264 行），loot_grant 只置拥有位。刚到手但没在背包页
       带上 ⇒ 这趟去向池仍只有「城市」；要用户进背包页带上小背包，之后出门才扩大去向池。
       （★2026-10-18：小背包已按用户令改「拥有即生效」，判据在 loot_pick_dest 内。） */
    /* ★2026-10-21 新玩法⑥：持有许愿签 → 这趟去"最近最少去"的那一类，并消耗 1 个。
       ⚠ 排在 loot_pick_dest() 之前、但排在场合预告（s_plan_dest）之后 ——
         藏品/场合指定方向是"更强的心愿"，不该被一张签覆盖。 */
    int dest;
    /* ★R128：**许愿签优先** —— loot_trip_plan_cb 每次都会写 s_plan_dest(0..2)，
       原顺序下 `s_plan_dest >= 0` 恒真 ⇒ 商店卖的许愿签永远不消耗、也从未影响去向
       （审计实证：wish 分支不可达）。玩家主动用签的意愿强于随机预告。 */
    if (loot_nvs_i32("item_wish", 0) > 0) {
        char kc[16]; int cnt[3] = { 0, 0, 0 };
        for (int d = 0; d < 3; ++d) {
            snprintf(kc, sizeof(kc), "map_cnt%d", d);
            cnt[d] = loot_nvs_i32(kc, 0);
        }
        /* ★2026-10-23 机制审查修：许愿签原来会在三地里挑"最少去"，
           即使玩家**没有小背包**也可能被送去山野/海滨（绕过 loot_pick_dest 的解锁门控）。
           现在只在**已解锁**的目的地里挑：城市恒可选；山野/海滨要拥有小背包。 */
        int cand[3], nc = 0;
        cand[nc++] = LOOT_SER_CITY;
        if (loot_is_owned(LOOT_I_BAG)) {
            cand[nc++] = LOOT_SER_WILD;
            cand[nc++] = LOOT_SER_SEA;
        }
        dest = cand[0];
        for (int i = 1; i < nc; ++i)
            if (cnt[cand[i]] < cnt[dest]) dest = cand[i];
        loot_nvs_set_i32("item_wish", loot_nvs_i32("item_wish", 0) - 1);
        ESP_LOGW(TAG, "★★ 许愿签生效：这趟去「%s」（已解锁里最近去得最少，剩 %d 张）",
                 dest == LOOT_SER_WILD ? "山野" : dest == LOOT_SER_SEA ? "海滨" : "城市",
                 loot_nvs_i32("item_wish", 0));
    } else if (s_plan_dest >= 0) {
        dest = s_plan_dest;            /* ★R128：保留 plan 预告（场合加成按它算，不能重掷） */
    } else {
        dest = loot_pick_dest();
    }
    s_plan_dest = -1;
    loot_nvs_set_i32("trip_dest", dest);
    /* ★R75：为这趟锁一个「寄信城市」并给明信片预算（最多 3 张）。
       预算落 NVS：跨深睡/离线补算（step_life 的 elapsed-1 趟）共用同一份，
       不会因为补算多跑几次就把明信片发成 5 张。 */
    loot_nvs_set_i32("trip_pa", post_pick_base(dest));
    loot_nvs_set_i32("trip_pb", 3);
    loot_nvs_set_i32("trip_pc", 0);                 /* 本趟已寄张数（出发清零） */
    loot_nvs_set_i32("trip_pe", 0);                 /* ★2026-11-12：本趟「特快白送」额度 0/1（出发清零）*/
    loot_nvs_set_i32("trip_pl", 0);                 /* 上次寄信时刻（限间隔用） */
    if (dest != LOOT_SER_ABROAD) map_note_near(dest);  /* ★F 旅迹：近郊出门记账 */
    static const char *DEST_NAME[4] = { "城市", "山野", "海滨", "异国" };
    ESP_LOGW(TAG, "★ 出门去向 = %s（第 %d 次出门 · 天气 %d）", DEST_NAME[dest], total, weather);
    if (dest == LOOT_SER_WILD && weather == WX_SNOW) {
        if (loot_grant(LOOT_I_SNOW))
            ESP_LOGW(TAG, "★★ 雪天进山 → 必带「雪花结晶」（一次性，§0.1）");
    }
    return 0;                                      /* 普通短途：不覆盖 net_sync 掷的出门时长 */
}

/* ═══ R75b：短途明信片改「路途中寄」（用户口径）══════════════════════════
 * 原来只在回家结算时寄（补算还会一口气寄多张）。现在改为：
 *   · 猫在外面期间，每次唤醒/同步按行程进度寄（20% / 50% / 80% 三个节点，
 *     **每次唤醒最多 2 张**；两次唤醒之间至少间隔 ≈15% 行程，避免连续同步刷出多张）；
 *   · 回家那一刻：如果整趟一张都没寄出，最多再补 1 张（按同一命中线掷骰）；
 *   · 整趟硬上限 3 张（trip_pb），且全部锁同一座城（trip_pa）。
 * 远行（exp_active>=0）不走这里 —— 按天发信由 exp_day_mail 负责。
 * ═══════════════════════════════════════════════════════════════════════ */
static bool trip_post_send(int dest)
{
    int budget = loot_nvs_i32("trip_pb", 3);
    if (budget <= 0) return false;
    /* ★R157（用户口径）：日常也会混入 AI 生成的新明信片，通常 25%；预制片全收齐时 100%。
       接口不通 / 已熔断 → 全部失败后落到下面原来的预制片逻辑＝「当作没开」。
       ★R157b（用户「也要带 AI 图的」）：命中 AI 时按 50% 走**图片通道**
       （api_image_gen → 带 AI 图的持久 mypost 卡），其余走文字活卡；图片失败再回落文字活卡。
       图片卡经 sink 落 mypost 槽、没有稳定的预制片下标 ⇒ 不记 jtrip 行程流水，
       但预算/寄信数/亲密度照记（信箱里收到片才是验收点）。 */
    const int ai_pct = (post_ungot_count() == 0) ? 100 : 25;
    if (g_api_on && g_api_key[0] &&
        ((int)(esp_random() % 100u) < ai_pct)) {
        bool ai_ok = false, used_live = false;
        if ((esp_random() & 1u) && api_image_ready()) {
            /* ★R170：画面 + 配文 + 寄自哪 **一次性出炉**（post_brief_make）。
               用户口径（2026-11-10）：「22 个日常场景不还是全部固定吗？我要的是它
               随机生成，理想情况玩 100 年也能不停出新的图」⇒ 画面不再由固件从清单里
               挑，改由文字模型每张现编；固件只掷「寄自哪座城 / 地标还是日常」，
               并拼固定的铅笔速写画风尾巴。文字通道挂了 → post_brief_make 内部回落
               本地场景池/地标关键词，照旧出片。 */
            post_brief_t bf;
            char pl[64];
            const char *home = (g_app.cfg && g_app.cfg->city[0]) ? g_app.cfg->city : "家";
            post_brief_make(&bf);
            ai_place_line(bf.where, pl, (int)sizeof(pl));      /* ★R162：「上海 晴」 */
            ESP_LOGI(TAG, "明信片：%s（%s，本地 %s，画面=%s）", bf.where,
                     bf.landmark ? "城市地标" : "日常场景", home,
                     bf.ai_scene ? "模型现编" : "本地兜底");
            ai_ok = api_image_gen(bf.prompt, bf.body, bf.title, pl);
        }
        if (!ai_ok) {
            used_live = api_live_fill(-1, 1, g_app.st.now_local);
            ai_ok = used_live;
        }
        if (ai_ok) {
            if (used_live) jtrip_note_mail(-1 - dest, 0, API_POST_IDX, api_live_title());
            post_save();
            loot_nvs_set_i32("trip_pb", budget - 1);
            loot_nvs_set_i32("trip_pc", loot_nvs_i32("trip_pc", 0) + 1);
            loot_nvs_set_i32("trip_pl", (int32_t)g_app.st.now_local);
            { int ni = ws_intimacy_add_auto(2); if (ni >= 0) g_app.st.intimacy = ni; }
            ESP_LOGW(TAG, "★★ 路途中寄到 AI 明信片（%s）：%s", post_dest_name(dest),
                     used_live ? api_live_title() : "AI 图卡");
            return true;
        }
    }
    int a = -1;
    bool from_anchor = false;
    bool is_penpal = false;              /* ★R218：本次是否笔友信（决定要不要附赠道具） */
    /* ★★R218：笔友来信独立通道 —— 修「用户从来收不到笔友信」。
       根因：笔友信 mask=0x07 通配，post_pick_base() 第一轮 exact 只认 m==want(1/2/4)
       ⇒ 永远排不进近郊池，第二轮兜底又永远不会触发（专属片有几十张，第一轮不可能空）。
       修法：不把笔友信塞回近郊池（92 张会挤掉同城片），在这里给独立通道：
         · 从没收到过笔友信 → 强制寄第一封（新玩家必得，解锁"看小鸟/看蝴蝶"）；
         · 之后每趟 15% 概率，保持稀有。
       附赠道具在投递成功后发放（见下方 post_set_got 之后）：小鸟→羽毛，蝴蝶→蝶翼。 */
    {
        const int penpal_got = post_cat_cnt(1);
        if (penpal_got == 0 || (int)(esp_random() % 100u) < 15) {
            const int pi = post_pick_penpal();
            if (pi >= 0) {
                a = pi;
                is_penpal = true;
                from_anchor = true;      /* 借用"已锚定"语义：不让下面的同城/去重逻辑覆盖 */
            }
        }
    }
    const int anchor = loot_nvs_i32("trip_pa", -1);
    if (!is_penpal && anchor >= 0 && anchor < N_POST) {
        const post_t *pa = post_at(anchor);
        if (pa) { a = post_pick_same_city(pa->city); from_anchor = (a >= 0); }
    }
    if (a < 0) a = post_pick_base(dest);
    if (a < 0) a = (int)(esp_random() % (unsigned)N_POST_BASE);
    if (!from_anchor && post_is_got(a)) {          /* 未锚定时才偏开重复；锚定=同城优先 */
        const int b = post_pick_base(dest);
        if (b >= 0 && !post_is_got(b)) a = b;
    }
    if (!from_anchor && a >= 0) loot_nvs_set_i32("trip_pa", a);   /* 回填城市锚（用最终 a） */
    /* ★★2026-11-10 用户令：邮政等级（特快 15% / 普快 70% / 慢递 15%）——三档都改行为：
       · 特快：这张**不占本趟预算**（等于白送一张，整趟最多能多一张）；
       · 慢递：当场不进信箱，压 2 天走 post_slow_tick 投放（只挑没收到过的新片）；
       · 普快：与升级前逐字一致。 */
    const int today = exp_day_of(g_app.st.now_local);
    int cls = post_mail_cls_roll();
    /* ★2026-11-12 修：白送额度整趟只给一次。第二次掷到特快 → 降级普快（占额、不丢片），
       否则 trip_pc 一直不增会让 tick 的 sent>=target / 寄信间隔闸门失效，变成每次唤醒刷 2 张。 */
    if (cls == POST_CLS_EXP && loot_nvs_i32("trip_pe", 0) >= 1) cls = POST_CLS_STD;
    const bool is_exp = (cls == POST_CLS_EXP);
    bool in_transit = false;
    /* ★R218：笔友信强制普快 —— 附赠道具在投递成功处同步发放；
       若走慢递，2 天后才到而道具当场发会让"先有道具后收到信"，且慢递队列里
       换片逻辑（post_system.c:1984）会把笔友信换成普通片 ⇒ 附赠来源丢失。 */
    if (is_penpal && cls == POST_CLS_SLOW) cls = POST_CLS_STD;
    post_mail_cls_set(a, cls);
    if (cls == POST_CLS_SLOW && post_slow_enqueue(a, today + POST_SLOW_DELAY)) {
        /* 已入在途队列 → 此刻**不算收到**（未读/详情都看不到，等它到）*/
        in_transit = true;
    } else {
        if (cls == POST_CLS_SLOW) post_mail_cls_set(a, POST_CLS_STD);   /* 队列写不进 → 按普快当场投，绝不丢片 */
        post_set_got(a);
        post_arr_set(a, today);
        post_mark_unread(a);
        /* ★R218：笔友信附赠道具（原设计"本批不接"从未实现，与笔友信永不寄达叠加 ⇒
           羽毛/蝶翼两大初始道具几乎拿不到）。投递成功才发，慢递在 post_slow_tick 补发。 */
        if (is_penpal) {
            const int kind = post_sender_kind(a);
            const int gift = (kind == POST_SENDER_BIRD)      ? LOOT_I_FEATHER
                           : (kind == POST_SENDER_BUTTERFLY) ? LOOT_I_WING : -1;
            if (gift >= 0 && !loot_is_owned(gift) && loot_grant(gift))
                ESP_LOGW(TAG, "★★ 笔友来信附赠：%s 送了 %s",
                         (kind == POST_SENDER_BIRD) ? "小鸟" : "蝴蝶",
                         (gift == LOOT_I_FEATHER) ? "一根羽毛" : "蝶翼");
        }
    }
    jtrip_note_mail(-1 - dest, 0, a, NULL);        /* ★R131：近郊这趟寄了哪张
                                                      ★R132：-1-dest 区分城市/山野/海滨三个桶
                                                      （原来一律 -1 ⇒ 旅迹页三行共用一个桶，
                                                        点上海和黄山看到的是同一份记录） */
    post_save();
    if (!is_exp) loot_nvs_set_i32("trip_pb", budget - 1);   /* 特快不扣预算 */
    else         loot_nvs_set_i32("trip_pe", 1);            /* 白送额度已用（整趟一次）*/
    /* ★2026-11-12 修：特快白送的那张不占 3 张名额 ⇒ trip_pc 不自增，
       否则 loot_trip_mail_tick():1651 的 sent>=3 会把「白送一张」整个抵消。 */
    const int occupied = loot_nvs_i32("trip_pc", 0);
    if (!is_exp) loot_nvs_set_i32("trip_pc", occupied + 1);
    loot_nvs_set_i32("trip_pl", (int32_t)g_app.st.now_local);
    /* 日志序号 = 本趟总寄出数 = 占额数(trip_pc) + 白送数(trip_pe)。
       不能用 occupied+1：特快不占额时，紧接着那张普快会又打一次「第 1 张」。
       分母 3 是占额上限，第 4 张显示 4/3 即「白送多出来的一张」。 */
    const int sent = loot_nvs_i32("trip_pc", 0) + loot_nvs_i32("trip_pe", 0);
    /* ★2026-11-12 修：慢递入队时片还没到 → 亲密延到 post_slow_tick() 真正投放时
       一次性结 +3（基础 2 + 等得值 1），不再提前发放。 */
    if (!in_transit) { int ni = ws_intimacy_add_auto(2); if (ni >= 0) g_app.st.intimacy = ni; }   /* ★R125：回写 RAM，防深睡覆写 */
    ESP_LOGW(TAG, "★★ 路途中寄明信片（%s）：「%s」（本趟第 %d/3 张 · %s%s）",
             post_dest_name(dest), g_posts[a].title, sent,
             post_mail_cls_name(post_mail_cls(a)),
             cls == POST_CLS_SLOW ? "· 2 天后到" : "");
    return true;
}

static void loot_trip_mail_tick(void)
{
    if (exp_active() >= 0) return;                 /* 远行按天发信，走 exp_day_mail */
    const int out_at = loot_nvs_i32("out_at", 0);
    const int out_ts = loot_nvs_i32("out_ts", 0);
    const time_t now = g_app.st.now_local;
    if (out_at <= 0 || out_ts <= out_at || now < out_at) return;
    const int budget = loot_nvs_i32("trip_pb", 3);
    const int sent   = loot_nvs_i32("trip_pc", 0);
    if (budget <= 0 || sent >= 3) return;
    const int dest = loot_nvs_i32("trip_dest", LOOT_SER_CITY);
    const bool night = loot_fx_is_night(now);
    const bool rain  = loot_fx_is_rain(g_app.st.weather);
    int hitp   = 20 + mail_pp_total(night);
    /* ★★R113：积压未读梯度惩罚（与 exp_day_mail 同一把尺子，keep=100 时与改动前一致）。 */
    {
        const int keep = mail_backlog_keep_pct();
        const int raw  = hitp;
        hitp = hitp * keep / 100;
        /* 只在真被压过时打一行 —— U≤1 的正常状态不刷屏，但积压时一眼能看出机制在跑。 */
        if (keep < 100)
            ESP_LOGI(TAG, "积压未读 %d 封 → 寄信保留 %d%%（命中线 %d%% → %d%%）",
                     post_unread(), keep, raw, hitp);
    }
    if (hitp < 0) hitp = 0;
    const bool force = mail_force_hit(dest, night, rain, g_app.st.weather == WX_CLEAR);
    /* ★2026-10-26 用户令（实机报「猫没出门也收到常规明信片」）：**回家不再补寄**。
       寄信只发生在「猫确实在外面」的路途中；回家那一刻 step_life 已清 out_ts，
       本函数不会再被走到 —— 这样屏幕上是猫在家，就不会凭空收到一张片。 */
    if (now >= out_ts) return;                     /* 到点回家 → 交给 return 分支（不寄） */
    /* ★★ 2026-10-27 R91 用户令：**串门窗口内不寄明信片**（在好友家做客，不是旅行）。
       窗口前后照旧按进度寄；这里只挡窗口那一段。远行在上面的 exp_active() 已 return，
       走 exp_day_mail 按天寄，不受影响。 */
    {
        const int vt0 = loot_nvs_i32("visit_t0", 0);
        const int vh  = loot_nvs_i32("visit_h", 0);
        if (vt0 > 0 && vh > 0 && now >= (time_t)vt0 && now < (time_t)vt0 + (time_t)vh * 3600)
            return;
    }
    if (out_ts <= out_at) return;                 /* ★R144：脏存档下防除零/inf */
    const double prog = (double)(now - out_at) / (double)(out_ts - out_at);
    int target = 0;
    if      (prog >= 0.80) target = 3;
    else if (prog >= 0.50) target = 2;
    else if (prog >= 0.20) target = 1;
    if (sent >= target) return;
    /* 两次寄信至少间隔行程的 15%（且不少于 20 分钟），避免连续唤醒刷屏。 */
    const int last = loot_nvs_i32("trip_pl", 0);
    int gap = (int)((out_ts - out_at) * 15 / 100);
    if (gap < 20 * 60) gap = 20 * 60;
    if (sent > 0 && last > 0 && now - (time_t)last < gap) return;
    /* ★R76 用户令：一次醒来最多寄 2 张（仍同城、整趟 ≤3）。 */
    for (int k = 0; k < 2; ++k) {
        if (loot_nvs_i32("trip_pc", 0) >= target) break;
        if (loot_nvs_i32("trip_pb", 3) <= 0) break;
        if (!(force || (int)(esp_random() % 100u) < hitp)) break;
        if (!trip_post_send(dest)) break;
    }
}

/* ② 回家那一刻（net_sync step_life 经钩子回调）：按系列掉落表掷骰 → 掉落；
 *    重复件自动折算成贝币（★D1）；橡果/探险家 = 第二件概率；里程碑复查（铃铛按亲密度）。 */
void loot_trip_return_cb(time_t out_at, time_t out_ts)
{
    /* ★R123（用户令 2026-11-03）：召回币提前回家的这趟，贝币只按 **1/3** 结算
       （否则 12 币召回 = 白拿整趟 ~35~45 币并跳过 3~8 天冷却）。
       标志由 ws_out_recall 写下，这里**读一次即清** —— 不清的话下一趟正常回家会被误削。
       ★R213：清零挪到 exp_return_settle **之后** —— 那个函数自己也要读这个标志
       （召回不给首去专属/大包旧物），先清就永远读到 0。 */
    const bool recall_cut = (loot_nvs_i32("recall_cut", 0) != 0);
    /* ★2026-10-24 C4：本趟结束 → luck_coin 1（生效中）清 0。
       迁移：旧 item_luckbell(int32 计数) 若 >0 且 luck_coin 无记录 → 置 2（待生效）。
       （旧 luck_pend 语义 = 必带回一次，折算成 2 待下趟生效，观感一致。） */
    if (loot_nvs_i32("luck_coin", -1) == -1) {
        loot_nvs_set_i32("luck_coin",
                         (loot_nvs_i32("luck_pend", 0) || loot_nvs_i32("item_luckbell", 0) > 1) ? 2 : 0);
        loot_nvs_set_i32("luck_pend", 0);
    }
    if (loot_nvs_i32("luck_coin", 0) == 1) {
        loot_nvs_set_i32("luck_coin", 0);
        ESP_LOGW(TAG, "★★ 好运铃本趟生效结束 → 已装备清零");
    }
    (void)out_at; (void)out_ts;
    const int dest = loot_nvs_i32("trip_dest", LOOT_SER_CITY);
    const int ed = exp_active();                   /* ★B5：远行归来走专属/大包结算 */
    /* ★2026-10-26 用户令：回家不再补寄明信片（避免「猫在家却收到一张」）；只保留路途中寄。 */
    /* ★2026-10-26 修（消耗品承诺 vs 实装）：好运铃的文案是「必带回一件」，而下面
       extra=100 只保证「多一掷」，那一掷自己仍可能落空档（29%~33%）⇒ 每三次用一次会白花。
       这里先记下"本趟该不该触发保底"，全部掷完若一件都没带回，再补发一次非空掉落。 */
    const bool luck_this_trip = (ed < 0) &&
        (loot_nvs_i32("luck_coin", 0) == 2 || loot_nvs_i32("coin_pend", 0) != 0);
    /* ★2026-10-21：一趟的掷骰 2 → **3**（第 3 掷才吃"带回率"门槛）。
       理由：单趟产出只有 0.6~1.2 件时，把"更频繁出门"当产出杠杆 = 用陪伴感换掉落（方向错）。
       现在把陪伴时间还给用户（见 net_sync.c OUT_* 注释），产出改由**单趟厚一点**来补：
       默认 2 掷必发（1.2 件），带"带回"类藏品/性格再 +1 掷（最多 1.8 件）。 */
    int granted = 0, coin_gain = 0, rolls = 3;
    /* ★★R113（用户令 2026-10-31）：**把 TRIP_H 从负收益翻成正收益**。
       原来「出门时长 +小时」是纯惩罚 —— 猫在外面待得越久，回家结算的趟数越少、亲密衰减越多、
       寄信窗口被拉长，于是那 10 件带 trip_h 的藏品等于「带了反而亏」，玩家会主动不背。
       现在按「时长换厚度」补偿：每 6 小时多给一掷（基准约 8h/3 掷 ≈ 2.7h 一掷，这里给得更保守，
       只补掉一半损失），并夹断 8 掷，避免「满载一趟带一麻袋」把观感搞崩。
       ⚠ 只对**近郊/普通出门**生效：远行的时长由 EXP_TBL[ed].days 决定，trip_h 在那条路上
         本来就被 trip_start 的覆盖值顶掉（无效），所以不给远行加（ed >= 0 时跳过）。 */
    if (ed < 0) {
        const int th = loot_fx_trip_h(dest);
        if (th > 0) { rolls += th / 6; if (rolls > 8) rolls = 8; }
    }
    bool got_any = false;                          /* ★本趟是否至少有一次非空掉落（好运铃保底用） */
    /* ★2026-10-21 新玩法②：**满载而归**（8%）—— 这一趟额外 +2 掷（≈多带 1.2 件）。
       作用：给"方差"。现在一趟稳定 0~2 件，久了没惊喜；8% 的 4~5 件大包能制造
       "今天这趟赚了"的记忆点 —— 这是最低成本的情绪峰值来源（不需要任何新 UI）。 */
    const bool jackpot = ((int)(esp_random() % 100u) < 8);
    if (jackpot) rolls += 2;
    if (ed >= 0) {
        /* ★★★ 2026-10-19 修「远行期间一张明信片都没寄」（用户实机报）：
           ★病根：exp_day_mail（旅途逐日补掷发信）**只在 loot_sync_done_cb（同步收尾）里被调用**，
             而同一次同步里 step_life **先**处理归来 → 本回调把 exp_dest 置 -1（见 exp_return_settle
             末行）⇒ 收尾时 exp_active() 已是 -1 ⇒ 整趟按天发信**一次都没跑**。
             离线、或整趟只在回家那一刻发生一次同步时，必然全丢（用户这次就是）。
           ★修法：在**结算之前**（exp 态还在）先补掷到「今天」为止 —— exp_day_mail 的
             `for (d = exp_mail_day+1 .. today)` 本身幂等，之前同步已发过的天不会重发。
           ★R213：召回币立即回家这趟猫没到过目的地 ⇒ 不补发旅途明信片。 */
        if (!recall_cut) exp_day_mail(ed, g_app.st.now_local);
        exp_return_settle(ed, &granted, &coin_gain);
        rolls = 0;                                 /* 远行的旧物掷骰在 exp_return_settle 里做完了 */
    }
    if (recall_cut) loot_nvs_set_i32("recall_cut", 0);   /* ★R213：结算读完再清，避免削到下一趟 */
    /* ★★D1 折算制（2026-10-16，机制文档 §7）：掉落**允许掷到已拥有的件** ——
       新件 loot_grant 入收藏；重复件折算成贝币（近郊 5 / 里程碑 20）。
       （原「已拥有不再掉」的口径作废 —— 否则永远没有重复，折算无从谈起。） */
    for (int roll = 0; roll < rolls; ++roll) {
        if (roll == 2 && !jackpot) {               /* ★2026-10-21：门槛从第 2 掷挪到**第 3 掷**
                                                      （jackpot 的额外两掷不吃门槛 = 必发）
                                                      （前两掷必发，产出下限从 0.6 → 1.2 件/趟）
                                                      带回几率：四叶草/橡果/探路杖/藏宝图/马蹄铁 + 探险家 */
            /* ★2026-10-21：心情 + 今天喂中偏好食，两者都加成"带回率" */
            int extra = loot_fx_back_pp(dest) + mood_bonus_pp();
            if (loot_nvs_i32("fav_hit", 0) == (int)(g_app.st.now_local / 86400)) extra += 5;
                                                    /* ★scope 接线：四叶草/橡果/探路杖只在山野计入 */
            if (g_persona == PERSONA_EXPLORER) extra += TRIP_EXTRA_PCT_EXPLORER;
            /* ★E 联动：丰收（橡果+松果）+15（50→65）/ 山野之秋（松果+银杏）山野必带回 /
               漂流瓶（玻璃瓶+半片贝壳）海滨必带回 —— 「必带回」效果挂第二件骰 */
            if (loot_is_owned(LOOT_I_ACORN) && loot_is_owned(LOOT_I_PINE))    extra += 15;
            if (dest == LOOT_SER_WILD &&
                loot_is_owned(LOOT_I_PINE) && loot_is_owned(LOOT_I_GINKGO))   extra = 100;
            if (dest == LOOT_SER_SEA &&
                loot_is_owned(LOOT_I_BOTTLE) && loot_is_owned(LOOT_I_SHELL))  extra = 100;
            /* ★2026-10-24 C4 三态 luck_coin：2（已用待生效）→ 本趟首掷转 1（生效中）。
               生效中 = 掉落表「空」档权重清零 + 物品权重 ×2（≈ 必带回且更肥）；
               本趟结束（回家结算完）在 loot_trip_return_cb 侧清 0。 */
            {
                const int lc = loot_nvs_i32("luck_coin", 0);
                if (lc == 2) {
                    loot_nvs_set_i32("luck_coin", 1);
                    ESP_LOGW(TAG, "★★ 好运铃：待生效 → 本趟生效中（空档清零 + 物品权重×2）");
                }
                if (lc >= 1) extra = 100;      /* 生效中/刚转生效：第二件必带回（与旧 luck_pend 同观感） */
            }
            if (loot_nvs_i32("coin_pend", 0)) {
                loot_nvs_set_i32("coin_pend", 0);
                extra = 100;
                ESP_LOGW(TAG, "★★ 旧幸运币挂起生效：这趟必定带回（挂起已消费）");
            }
            /* ★R213（用户令）：门槛没过只跳过**这一掷**，不能 break ——
               长行程（trip_h）追加的第 4~8 掷都在第 3 掷之后，break 会把它们一起吞掉
               ⇒ 背了「出门时长」藏品反而少掷，与 R113 的正收益口径相反。 */
            if (extra <= 0 || (int)(esp_random() % 100u) >= extra) continue;
        }
        const int d = loot_roll_drop(dest);
        /* ★2026-10-21：落「空」档 → 给 1 币「路上捡到一枚旧硬币」。
           原来 40% 空手 = 屏上什么都不发生，用户体感"白等了两天"；1 币不破坏经济
           （≈ 一趟期望 +0.4 币），但保证**每次回家都有反馈**。 */
        /* ★R112：空掷兜底 1 → 2 币。原值 1 会让海星的「归来旧物 +20%」在
           小额结算上被整除抹平：一趟三掷全空 G=3 ⇒ floor(3*1.2)-3 = 0 ⇒ 收益精确为 0。 */
        if (d < 0) { coin_gain += 2; continue; }
        got_any = true;
        /* ★2026-10-21：新件也给"初次发现奖励" —— 收藏空集期不再是零收入。 */
        if (loot_grant(d)) { granted++; coin_gain += COIN_PRICE_FIND; continue; }
        coin_gain += coin_value_of(d) * loot_fx_coin_pct() / 100;   /* 重复 → 折算（★A2 加成） */
    }
    /* ★2026-10-26：好运铃「必带回一件」保底 —— 上面全空也补一发（最多试 10 次，
       掉落表非空时必中）。这样文案与实装一致，不再出现"用了却什么都没带回"。 */
    if (luck_this_trip && !got_any) {
        int d = -1;
        for (int t = 0; t < 10 && d < 0; ++t) d = loot_roll_drop(dest);
        if (d >= 0) {
            if (loot_grant(d)) { granted++; coin_gain += COIN_PRICE_FIND; }
            else coin_gain += coin_value_of(d) * loot_fx_coin_pct() / 100;
            ESP_LOGW(TAG, "★★ 好运铃保底：本趟至少带回一件「%s」", loot_name_of(d));
            got_any = true;
        }
    }
    /* ★2026-10-21 新玩法③：**连续空手保底**计数 —— 有件 → 归零；空手 → +1
       （下一趟 loot_roll_drop 会把"空"档权重清零 = 至少带回一件，见该函数尾部）。
       目的：消灭"连着三趟白等"的挫败段（★R120 实测空手率 29/32/33%，三连空约 3~4%，体验极差）。 */
    {
        const int dry = loot_nvs_i32("dry_n", 0);
        if (granted > 0) { if (dry) loot_nvs_set_i32("dry_n", 0); }
        else {
            loot_nvs_set_i32("dry_n", dry + 1);
            ESP_LOGW(TAG, "★ 这趟空手（连续 %d 趟）—— 下一趟保底至少带回一件", dry + 1);
        }
    }
    if (jackpot && rolls > 0) ESP_LOGW(TAG, "★★ 满载而归！这趟额外多带了两包旧物（共带回 %d 件）", granted);   /* ★2026-10-23 修：加 rolls>0 —— 远行归来 rolls=0 时 jackpot 也曾打印，件数与近郊口径混淆（误导排障） */
    if (coin_gain > 0) {
        /* ★R112：改**四舍五入**。原来是直接截断，G=1..4 时 floor(1.2G)-G 恒为 0
           ⇒ 海星（carry=1，占一个背包格）在多数实际结算里收益为 0，等于空头承诺。 */
        coin_gain = (coin_gain * (100 + loot_fx_back_bonus()) + 50) / 100; /* ★A9 海星：归来旧物 +20% */
        if (recall_cut) coin_gain = (coin_gain + 2) / 3;   /* ★R123：召回币提前回家 → 贝币只给 1/3 */
        coin_add(coin_gain);
        ESP_LOGW(TAG, "★★ 折算入账 +%d 贝币（现有 %d）%s", coin_gain, coin_get(),
                 recall_cut ? "（召回币提前回家：已按 1/3 结算）" : "");
    }
    /* ★R75b（用户口径）：短途明信片只走路途中寄（按行程进度），回家不再补发。
       发送实现见上方 loot_trip_mail_tick()。 */
    /* ★★2026-10-18 两铃「从外获得」：近郊回家 8% 带回一只（好运铃/开饭铃对半，封顶 99）。
       好运铃 = 计数 +1；开饭铃 = 计数 +1（盆里有食时用铃才生效）。 */
    if (ed < 0 && (int)(esp_random() % 100u) < 8) {
        const bool luck = (esp_random() & 1u) != 0;
        bell_add(luck ? "item_luckbell" : "item_mealbell", 1);
        ESP_LOGW(TAG, "★★C6 回家带回「%s」×1（现有 %d）", luck ? "好运铃" : "开饭铃",
                 loot_nvs_i32(luck ? "item_luckbell" : "item_mealbell", 0));
    }
    ESP_LOGW(TAG, "★ 回家结算：目的地 %d · 带回新件 %d · 折算 %d 贝币", dest, granted, coin_gain);
    if (granted > 0 || coin_gain > 0) {                /* ★D 结算框：带回明细挂到主页（看到后消失） */
        /* ★★2026-10-21 四轮：**多趟累加** —— 离线补算（net_sync.c step_life 的 elapsed-1 趟循环）
           会连续调多次本回调，旧代码是覆盖写 ⇒ 屏上只显最后一趟（币每趟都入账了没丢，
           丢的只是显示汇总）。口径：上一段结算已被看过（s_coin_rep 已清）→ 从 0 起算；
           还没看过 → 继续往上累，趟数记进 s_coin_rep_trips（主页文案「出门N趟·共M件」）。 */
        if (!s_coin_rep) { s_coin_rep_n = 0; s_coin_rep_m = 0; s_coin_rep_trips = 0; }
        s_coin_rep_n     += granted;
        s_coin_rep_m     += coin_gain;
        s_coin_rep_trips++;
        s_coin_rep = true;
        s_coin_rep_seen = false;                       /* ★2026-10-19：新一次的结算框重新计"未看过" */
    }
    loot_milestone_check("回家");
    /* ★A4 + 机制文档 §三：猫回家一次性 +2（★2026-10-23 用户拍板：3→2，
       当日有照顾它——盆里有食/残渣——才 +3）；暖手炉加成的叠加口径不变。
       只落 NVS（ws_intimacy_add 双键落账），内存 st 由下一轮同步收口 ——
       本回调跑在同步上下文，别和 step_life 的结算互相覆盖。 */
    {
        int home_add = 2 + loot_fx_home_pp();
        if (g_bowl.state != BOWL_EMPTY) home_add += 1;   /* 今天喂过 → +3 */
        { int ni = ws_intimacy_add_auto(home_add); if (ni >= 0) g_app.st.intimacy = ni; }   /* ★R125：回写 RAM */
    }
    /* ★B8：当地纪念池 —— 专属/纪念之外再送一件没收过的当地件；全齐 → 亲密度 +3 兜底。
       ★2026-10-26 修：这个兜底只属于**远行**（ed>=0）。原来无条件调 exp_souv_grant(ed)，
       近郊回家 ed=-1 必然返回 false ⇒ 每趟近郊白送 +3 亲密度（与 R63「只由互动推动」冲突）。
       ★R213（用户令）：召回币「立即回家」这趟猫**没到过目的地**，当地纪念件/亲密度兜底一并跳过。 */
    if (ed >= 0 && !recall_cut && !exp_souv_grant(ed)) { int ni = ws_intimacy_add_auto(3); if (ni >= 0) g_app.st.intimacy = ni; }   /* ★R125：回写 RAM */
    exp_souv_load();               /* 开机装载（幂等；settle 在开机后才会走到） */
    /* ★2026-10-25 基座接线⑤：远行归来送一张 AI 明信片进 mypost（静默生图）。
       阻塞 30~120s（任务+轮询+下载+解码）；没配图像 key / API 关 → 一行日志跳过。 */
    if (ed >= 0 && api_image_ready() && g_api_on) {   /* ★2026-10-26：ed 可能是 -1（近郊回家），
                                                        这一块原来没判 → EXP_TBL[-1].name 会炸 */
        /* ★R173 用户令：远行归来的第三处出图也要「每张现编」，不再用固定目的地提示词。
           地点仍锁死为本次远行目的地（不然画成普通巷口就不像冰岛/京都了），
           画面 + 标题 + 正文交给 post_brief_make_at / ws_api_scene 一次性现编；
           地点行仍用译文，保证非中文态不写汉字。 */
        post_brief_t bf;
        char pl[64];
        post_brief_make_at(&bf, EXP_TBL[ed].name);
        ai_place_line(t_(EXP_TBL[ed].name), pl, (int)sizeof(pl));   /* ★R162：「京都 晴」 */
        ESP_LOGI(TAG, "明信片（远行归来）：%s · 画面=%s", EXP_TBL[ed].name,
                 bf.ai_scene ? "模型现编" : "本地兜底");
        api_image_gen(bf.prompt, bf.body, bf.title, pl);
    }
}

/* ③ 每次联网同步收尾（net_sync ws_sync 经钩子回调）：空箱保底羽毛 ——
 *    收藏箱一件都没有时，每次同步 40%、第 3 次必得（§0.5 羽毛行）。 */
void loot_sync_done_cb(bool ok)
{
    /* ★R75b：猫在外面时，每次唤醒/同步按行程进度寄明信片（联网失败也按缓存时间结算）。 */
    loot_trip_mail_tick();
    if (!ok) return;                               /* 只在真同步成功时算次数 */
    /* ★R111(2026-10-30) 注释订正：此处原写「空箱保底加速：旧纽扣 = 第 2 次必得 / 许愿硬币
       = 第 1 次必得」。该口径已作废 —— 2026-10-18 用户令把这两件改挂**出门率**
       （main.c 的 g_loot_fx[]：LOOT_I_BUTTON / LOOT_I_WISH_COIN → LFX_OUT_PP 5/10），
       空箱保底已回到**纯基础线**（每 LOOT_FEATHER_SYNC_PCT%、第 LOOT_FEATHER_SYNC_MAX 次必得），
       不再有任何「加速件」。下面的判定因此等价于「已有任一藏品即停掷」。 */
    /* ★2026-10-27 B4：LFX_PITY 是死杠杆（g_loot_fx[] 零行使用，loot_fx_pity() 恒返回 0），
       原 (owned_cnt>0 && pity==0) 恒真 ⇒ 有藏品即重置 fd_cnt、need 恒 = LOOT_FEATHER_SYNC_MAX。
       删杠杆后保留与原行为一致的判定。 */
    /* ★R125（用户令 2026-11-03）：羽毛是**解锁小鸟**的前置，绝不能「已有其它藏品就绝版」。
       原表达式 `loot_is_owned(FEATHER) || loot_owned_cnt_all() > 0` 让玩家一旦有了任何一件
       藏品，空箱保底就永久停掷 ⇒ 羽毛再也拿不到。改为**只看是否已拥有羽毛**：
       其余玩家每次同步仍会掷（第 N 次必得），拿到后自动停。 */
    if (loot_is_owned(LOOT_I_FEATHER)) {
        loot_nvs_set_i32("fd_cnt", 0);
    } else {
        const int c = loot_cnt_add("fd_cnt", 1);
        const int need = LOOT_FEATHER_SYNC_MAX;
        const bool hit = (c >= need) ||
                         ((int)(esp_random() % 100u) < LOOT_FEATHER_SYNC_PCT);
        if (hit) {
            loot_grant(LOOT_I_FEATHER);
            loot_nvs_set_i32("fd_cnt", 0);
            ESP_LOGW(TAG, "★★ 空箱保底：羽毛到手（第 %d 次 · 必得线 %d）", c, need);
        } else {
            ESP_LOGI(TAG, "空箱保底掷骰未中（第 %d 次 · %d%% · 第 %d 次必得）",
                     c, LOOT_FEATHER_SYNC_PCT, need);
        }
    }
    /* ★B3 远行旅途推进：在途 → 跨天逐日补掷发信（45% 无 / 40% 一发 / 15% 两发）。
       now 读 g_app.st：本回调在同步上下文里跑，net_sync 刚把 fresh 时间写进 st。 */
    {
        const int ed = exp_active();
        if (ed >= 0) exp_day_mail(ed, g_app.st.now_local);
    }
    /* ★2026-10-26 修（实机：「转发成功但对方收不到」）：好友链同步原来只在开机那一次跑，
       手动「立即同步」/后台 30 分钟同步都不会去取好友信箱。收口到这里 = 每次 ws_sync 收尾都取一次。 */
    app_friend_sync();
}
