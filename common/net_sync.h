/*
 * net_sync.c — 真实世界同步（WiFi + 时间 + 天气 + 月相 + 出门状态）
 * ===========================================================================
 * 这一层回答四个问题，全部来自**真实世界**：
 *   ① 现在是哪个时段？  破晓5 / 清晨6-10 / 正午11-16 / 傍晚17-19 / 深夜20-4（本地时间）
 *   ② 外面什么天气？    晴 / 雨小 / 雨中 / 雨大 / 雾 / 雪（Open-Meteo，免 key）
 *   ③ 今晚月亮什么样？  8 档月相（纯计算，不依赖网络）
 *   ④ 猫在家吗？        亲密度 + 出门状态机（NVS 持久化，按「天」结算）
 *
 * 设计原则（用户明确要求）：
 *   · **永不空屏**：没网、没配网、时间没同步 —— 一律降级到「缓存的上次值」，
 *     缓存也没有就用出厂默认。任何情况下都要能画出画面。
 *   · **热点配网**（类似小智）：开机没有 WiFi 配置时自建热点，
 *     手机连上去填 WiFi 名/密码/城市，存 NVS 后重启联网。
 *
 * ── 时间来源 ────────────────────────────────────────────────────────────
 *   SNTP (pool.ntp.org) 取 UTC → 加时区偏移 → 本地时间。
 *   时区按城市经纬度粗算（见 CITY_TZ_OFFSET），中国 +8 无夏令时。
 *   同步成功后把「UTC 时间戳 + 开机后经过的秒数」算出的当前时间存 NVS，
 *   断网时用 RTC 继续走（`esp_timer` 单调时钟），精度足够（时段是小时级）。
 *
 * ── 天气来源 ────────────────────────────────────────────────────────────
 *   Open-Meteo 免 key：https://api.open-meteo.com/v1/forecast
 *     ?latitude=..&longitude=..&current=weather_code,temperature_2m
 *   返回 JSON，`current.weather_code` 是 WMO 码，映射到本项目 6 档天气。
 *
 * ── 月相 ────────────────────────────────────────────────────────────────
 *   月龄算法（纯计算）：以已知新月时刻为基准，除以朔望月长度取余。
 *   不需要网络，不需要天文库。
 * ===========================================================================
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 时段（与 build_统一出图.py 的 PERIODS 顺序一致）---- */
#define N_PERIOD    4
#define PERIOD_DAWN     0   /* 清晨 06:00-10:59 */
#define PERIOD_NOON     1   /* 正午 11:00-16:59 */
#define PERIOD_DUSK     2   /* 傍晚 17:00-19:59 */
#define PERIOD_NIGHT    3   /* 深夜 20:00-04:59 */
/* ★ 破晓 05:00-05:59（2026-09-25 用户新增）：只是**名字**独立，
   视觉（房间/窗贴片/网点）全部复用傍晚素材 —— 渲染侧用 period_visual()
   把它映射到 PERIOD_DUSK。注意 N_PERIOD 仍是 4（素材只有 4 套），破晓**没有**素材槽。 */
#define PERIOD_BREAK    4   /* 破晓 05:00-05:59（视觉=傍晚） */

/* ---- 天气（★2026-09-28 新增雷 = 6，N_WEATHER 6→7；win.bin 同步 48→56 片 7 列布局）---- */
#define N_WEATHER   7
#define WX_CLEAR    0       /* 晴 */
#define WX_RAIN_L   1       /* 雨小 */
#define WX_RAIN_M   2       /* 雨中 */
#define WX_RAIN_H   3       /* 雨大 */
#define WX_FOG      4       /* 雾 */
#define WX_SNOW     5       /* 雪 */
#define WX_THUNDER  6       /* 雷（09-28 新增：WMO 95/96/99 雷雨码映射；惊吓姿态仅此天气概率出现） */

/* ---- 月相 ---- */
#define N_PHASE     8       /* 8 档：新月/蛾眉/上弦/盈凸/满月/亏凸/下弦/残月 */

/* ---- 地标（与 build 一致）----
   ★R195：2 → 3，新增「星球」（用户给的第三张风景图，同一套分层/天气逻辑）。
   下标顺序必须与 scripts/_tools/gen_land_face.py 的 LANDS 完全一致。 */
#define N_LAND      3

/* ---- ★★ 2026-10-27 R91 串门彩蛋（长行程中途去好友家待一段）----
   对外语义（上报给好友的 vis）：0 在家 / 1 在好友家串门（只有这一段对方才显示）/ 2 正常在外（对方什么都不显示）。
   窗口在 net_sync step_life 出门时掷骰规划，写 NVS `visit_t0`（起点 unix 秒）+ `visit_h`（时长小时）；
   0 = 本趟不串门。main.c 上报与 loot_system 暂停寄信都读这两个键。 */
#define VISIT_P_PCT       20    /* 每趟长行程 20% 概率去串门（用户令：小概率彩蛋，「偶尔看到」） */
#define VISIT_MIN_H        2    /* 串门最短 2 小时（再短对方 30 分钟同步一次可能抓不到） */
#define VISIT_MAX_H        8    /* ★用户令：最长 8 小时；实际 2~8 随机（有时短有时长） */
#define VISIT_EDGE_H       2    /* 行程首尾各留 2 小时纯正常出行，避免刚出门/快回家时串门 */
#define VISIT_MIN_TRIP_H  10    /* 行程短于 10 小时不串门（短途就是纯正常出行） */
#define LAND_FUJI   0
#define LAND_PEARL  1
#define LAND_PLANET 2      /* ★R195：星球（第三张风景） */

/* 同步结果（一次联网把这几件事一起取回来） */
typedef struct {
    bool     time_ok;        /* 时间是否有效（true = 任何来源拿到了时间） */
    time_t   now_local;      /* 本地时间戳（epoch 秒，已加时区） */
    int      period;         /* 0..3 */
    int      weather;        /* 0..5 */
    int      temp_c;         /* 气温 ℃（仅日志/调试用） */
    int      moon_phase;     /* 0..7 */
    int      moon_age_pct;   /* 月龄百分比 0..100（调试用） */
    int      land;           /* 0=富士山 1=东方明珠 2=星球 */
    bool     outside;        /* 猫是否出门了 */
    time_t   out_ts;         /* ★ 2026-09-21：预计返家时刻（本地 epoch 秒）；0 = 没出门
                                门外时长是**小时级**的，所以不能再按「天」记 ——
                                首页立牌那句「预计 Nh 后回家」就是从这个时刻算出来的。 */
    time_t   out_at;         /* ★★ 2026-09-23：**出门那一刻**（本地 epoch 秒）；0 = 未知。
                                用户换稿后立牌变成两行：「猫猫已出门 Nh」+「预计 Nh 后回家」。
                                已出门时长 = now_local − out_at；两者之差 = 本次出门总时长。
                                ⚠ 只存 out_ts 反推不出出门时刻（总时长是 esp_random 抽的，
                                  存不回来）→ 必须单独记这一笔。老数据没有它时按「现在刚出门」
                                  处理（见 net_sync.c::step_life 的迁移注释）。 */
    int      intimacy;       /* 亲密度 0..100 */
    bool     from_cache;     /* true = 本次用的是缓存（没连上）*/
} ws_state_t;

/* WiFi 配置（NVS 读出来的）
 *
 * ★ 2026-09-19 用户订正（重要，别退回去）：
 *   配网里原本只有「城市」一项，且**由城市反推窗外风景**（日本城市→富士山，其余→东方明珠）。
 *   这是错的，现在是**两个完全独立的字段**：
 *     · city —— 使用者**居住的城市，精确到区**（如「上海市浦东新区」）。
 *                唯一用途 = 解析经纬度 → 同步天气。
 *     · land —— **窗外风景**，用户自己挑的画面元素（富士山 / 东方明珠）。
 *                与城市、与天气都无关，纯观感。
 */
typedef struct {
    char ssid[33];
    char pass[65];
    char city[64];           /* 居住城市，精确到区；只用于查天气 */
    int  land;               /* 窗外风景：0=富士山 1=东方明珠 2=星球（用户自选） */
    float lat, lon;          /* 由 city 经 Open-Meteo Geocoding 解析后回填 */
    int   tz_offset_sec;     /* 时区偏移（秒）；由天气接口 utc_offset_seconds 校准 */
    bool  valid;
} ws_config_t;

/* ---- 对外接口 ---- */

/* ★R195：地标名（中文，用于日志与 NVS 的人可读副本）· 越界归「东方明珠」。
   界面文案仍走 main.c 的 LAND_NAME[] + T()（可翻译）。 */
const char *ws_land_name(int land);

/* 初始化：挂载 NVS、读配置。不联网。返回 false = 无配置（需配网） */
bool ws_init(ws_config_t *cfg_out);

/* 连接 WiFi（阻塞，最多 timeout_ms）。true = 连上拿到 IP */
bool ws_wifi_connect(const ws_config_t *cfg, int timeout_ms);

/* ★★ 2026-09-23：运行时**切换**到另一条 WiFi（规格 §11.2「改 cur_wifi 索引 → 断开重连，
   全程零重新配网」）。与 ws_wifi_connect 的关键区别：可以安全地在同一次开机里重复调用
   （按需 start，不会撞 ESP_ERR_WIFI_NOT_STOPPED）；切换前会先断开旧连接。
   返回 true = 连上拿到 IP；调用方成功后再回写 cfg 并落 NVS。 */
bool ws_wifi_switch(const char *ssid, const char *pass, int timeout_ms);

/* ★ 一次完整同步：时间 + 天气 + 月相 + 出门状态。任何一步失败都自动降级。
   ★ cfg 变成非 const：同步过程会**回写**解析出的 lat/lon 与时区（并落 NVS）。 */
void ws_sync(ws_config_t *cfg, ws_state_t *st);

/* ══ 2026-10-14 R46：后台同步任务（方案 B）—— 同步链挪出主循环，触摸不再被阻塞 ══
 * ws_sync_task_start()      创建任务（幂等；在两版主循环入口调一次）。
 * ws_sync_begin(cfg, st)    投递一次后台同步（非阻塞）。任务未起时降级为原地阻塞
 *                           （启动/配网早期照旧，行为与 R45 全同）。单飞：上一轮
 *                           没跑完时重复投递被忽略。
 * ws_sync_async_busy()      后台同步进行中 —— ★主循环此刻**不得写回 g_app.st**
 *                           （否则 fresh 时间累加会覆盖 sync 刚写回的网络真值）。
 * ws_sync_async_done()      有一轮后台同步完成待收尾（clock_rebase/bookkeep/补刷
 *                           的决策全在主循环；epd 只许主循环碰）。
 * ws_sync_async_consume()   收尾完成取走 done 态。
 * ⚠ 契约细节见 net_sync.c 同名区块的注释（epd 分工/时间数学/竞态容忍三点）。 */
void ws_sync_task_start(void);
void ws_sync_begin(ws_config_t *cfg, ws_state_t *st);
bool ws_sync_async_busy(void);
bool ws_sync_async_done(void);
void ws_sync_async_consume(void);

/* ★★ 2026-09-24（唤醒慢修复）：调 ws_sync 内部「连 WiFi」这一步的预算。
   默认 20000ms（冷启动：有「正在连接」页兜着，等满不冤）；
   按键唤醒的「出画面后补同步」在调用前把它收紧到 8000ms —— 唤醒场景优先快，
   连不上就下次再试（重试计数器会兜底强制重连）。 */
void ws_set_wifi_budget_ms(int ms);

/* 城市名（精确到区）→ 经纬度。Open-Meteo Geocoding，需要已联网。
   ws_sync() 内部会自动调用；这里暴露出来是为了「设置 → 改城市」能用。
   返回 true = 拿到了坐标（或沿用上次同城结果）。 */
bool ws_resolve_city(ws_config_t *cfg);

/* ★★ 2026-09-23 新增：设置 →「窗外风景」页选定后调用。
   原来 land 只能在**配网时**选（net_prov.c 的内联写法）；用户在设置里加了二级页，
   屏上就要能换 —— 所以补一个「程序内改配置」的出口。
   它会做三件事：① cfg->land ② st->land（render_key 用它 → 回主页会重刷）
   ③ 落 NVS（`land` 整数 + `scenery` 人可读副本），**不联网、不重启**。 */
void ws_set_land(ws_config_t *cfg, ws_state_t *st, int land);

/* 只算月相（纯计算，不联网）。用于断网时仍能给出正确月相 */
int  ws_moon_phase(time_t utc, int *age_pct_out);

/* 时段判定（本地时间戳 → 0..3） */
int  ws_period_of(time_t local_epoch);

/* 保存/读取状态缓存（NVS），断网时用上次的值 */
void ws_save_state(const ws_state_t *st);
void ws_load_state(ws_state_t *st);

/* ★★ 2026-09-26（网络图标修复）：「上次成功联网」本地伪 epoch 的 NVS 落盘 / 读回。
   main.c 的 g_rtc_last_sync 只活在 RTC 保留内存，冷启动（掉电 / 烧录复位）清零；
   不落盘的话，配过网的设备冷启动后一旦联网失败就被误判。
   联网成功处调用 save；启动恢复处调用 load。没存过返回 0
   （flash19b 口径：cfg 有效时 0 也归「无wifi」，不再算「从未联网」）。 */
void   ws_save_last_sync(time_t now_local);
time_t ws_load_last_sync(void);

/* ★★ 2026-09-26（flash19c）：最近一次 ws_sync 的「wifi 是否连上」（连通性）。
   网络图标/上次联网时刻的更新用它 —— 旧判据 from_cache 的语义是「拿到网络数据」
   （还兼任重试计数器判据）：wifi 连上但 SNTP/接口失败时它仍是 true，
   用户视角就成了「联网成功页都跳了，标志怎么还在」。两个职责从此分开记账。 */
bool ws_last_net_ok(void);

/* ★ 2026-09-21 新增：由「返家时刻 + 当前时间」重算 st->outside。
   为什么必须有它：出门时长从「按天」改成「按小时」以后，**深睡醒来不联网**的那些帧
   手里只有 RTC 里的旧快照 —— 若不重算，猫明明到点回来了，屏上还一直贴着「外出立牌」。
   凡是在 main.c 里改过 st->now_local 的地方，改完就调一次这个。 */
void ws_refresh_outside(ws_state_t *st);

/* ★★ 2026-10-26：召回币 —— 用掉后猫立即回家（secs=0），或把返家时刻压到 now+secs。
   返回 false = 猫不在外面 / NVS 写失败（调用方不要消耗道具）。NVS 双键（out_ts/st_outts）一起写。 */
bool ws_out_recall(ws_state_t *st, int secs);

/* ★★★ R101（2026-10-28）：**清出门态的唯一出口** —— out_ts / out_at / st_outts / st_outat
   四个键一次 commit 同清，并同步把 st->outside / out_ts / out_at 归零（st 可为 NULL）。
   为什么必须是函数：单清 st_* 或单清 out_ts 都会让旧值在下一次 step_life 里复活
   （见 net_sync.c 里 ws_out_clear 的注释与 2026-10-28 的实机事故）。
   凡「让猫回家」的路径（测试页手动回家 / 召回币归零后收尾 / 一致性自愈）都调这里。 */
void ws_out_clear(ws_state_t *st);

/* ★★ flash24（2026-09-30）：互动加亲密度 + 落 NVS，返回新值。
   背景（用户报「猫猫很久没出过门」）：亲密度过去**只有每日 −2 的衰减、没有任何上升途径**
   （main.c 里只读不写），出厂 60 三天就跌破出门门槛，猫从此再也不出门。
   现在喂食 / 抚摸都会往上加，调用方拿到返回值直接写回 g_app.st.intimacy（内存与 NVS 同步）。
   约束与 step_life 一致：地板 20、天花板 100；NVS 双键（st_inti 给 load_state、inti 给 step_life）一起写。
   ★R175：内部过「梯度阻尼」——40 以下难降、80 以上难升（4 点输入才 +1），实际位移可能小于 delta。
   ★R176：本函数是**真互动通道**（喂食 / 抚摸 / 猫薄荷 / 小鱼饼干），上限 100。 */
int ws_intimacy_add(int delta);

/* ★★ R176（2026-11-10 用户拍板）：**自动来源通道** —— 回家 / 寄片 / 远行兜底 / 掉落 / 里程碑。
   与 ws_intimacy_add 唯一的差别 = 上限 INTI_AUTO_CAP(70)：
   这些「不用人管也会发生」的事件最多把亲密度推到 70（形影不离），
   70 以上只有真互动（上面的 ws_intimacy_add）能推。
   目的：用户实测「没怎么互动，好感度却一直 100 降不下来」—— 光靠梯度阻尼不够，
   因为自动行为一趟送 +5 点输入、衰减一趟只扣 0.84，迟早磨到顶。
   返回值同 ws_intimacy_add（新值；NVS 失败 -1；已在上限时返回现值、不落盘）。 */
int ws_intimacy_add_auto(int delta);

/* ══ ★G 远行 API（2026-10-16）：OpenAI 兼容 chat/completions 生成「猫的远行日记」══
 * dst=目的地名（EXP_TBL 名）· day/total=旅途第 N/共 N 天 · weather=0..6（WX_*）。
 * 成功 = true 且 out 填好「标题 正文」（空格断句、无标点、UTF-8 完整截断）；
 * 失败（没配 Key / 请求挂 / 解析挂）= false，调用方兜底预制片。
 * 阻塞至多 10s（HTTPS POST），跑在调用方上下文；Key/URL/模型读 NVS petns:api_key /
 * api_url / api_model（缺省 api.openai.com/v1/chat/completions + gpt-4o-mini）。 */
bool ws_api_diary(const char *dst, int day, int total, int weather, char *out, int cap);
/* ★R170：`ws_api_nearby()` 已删（配文并入 ws_api_scene，见下）。 */
/* ★R170：AI 明信片「现编画面」—— 一次调用同时拿到 画面(英文) + 标题 + 正文。
   用户口径（2026-11-10）：「22 个日常场景不还是全部固定吗？我要的是随机生成，
   理想情况玩 100 年也能不停出新图」⇒ 具体画面不再取自固件清单，由模型每张现想；
   固件只掷「寄自哪 / 地标还是日常」并给一句光线引子，画风尾巴由调用方拼。
   place=中文地名（可能已辐射到邻城）· landmark=true 允许带一点该城味道。
   en/title/body 均为原始模型输出（未过字库清洗，调用方负责 api_font_fix）；
   失败（没配/限流/格式不对/画面不给英文）= false，调用方回落到本地旧路。 */
bool ws_api_scene(const char *place, bool landmark, int weather,
                  char *en, int encap, char *title, int tcap, char *body, int bcap);

/* ---- 热点配网（类似小智）---- */
/* 自建热点 + HTTP 配置页 + DNS 劫持。阻塞直到用户提交或长摁 BOOT 跳过。
   ★ 2026-09-19 用户订正：**不会因超时自动跳过** —— 没配置就一直等，
   出路只有两条：手机提交成功（返回 true）/ 长摁 BOOT 1.5s 跳过（返回 false，
   之后用随机天气+随机地点跑，想改到设置里重新配网）。
   成功返回 true 并把新配置写进 cfg（同时写 NVS）。 */
bool ws_provision(ws_config_t *cfg);

/* ★★2026-10-19（API 前端批次）：**API 配置模式**（阻塞，与 ws_provision 同口径）。
   设备端「功能页 → API接口」的开关（未配置时）/ 底部「去配置/重新配置」按钮 →
   prov_again_set_goal(PROV_AGAIN_API) + esp_restart() → 冷启动时走本函数：
   开热点（无密码）→ 手机连上**直接出 API 配置页**（h_root 按 NVS prov_again==2 分派）
   → 等手机保存（/api POST）或长按 BOOT 跳过 → 收热点（**保留设备原有 STA**）。
   返回 true = 已保存配置；false = 用户跳过/超时。设备原 WiFi 配置**不受影响**。 */
bool ws_api_provision(void);

/* ★★ 2026-10-05（触屏版，用户订正）：配网页「跳过」改 **按住底部按钮 3 秒**。
   ── 为什么要改（用户原话）：「点击跳过的话，可能会导致误操作 —— 你下面给他一个按钮，
      必须按这个按钮 3 秒才能跳过」。
      配网是设备**唯一**能连上网的窗口，手指一碰就跳过去 = 用户不知道自己做了什么，
      而且后果不可见（之后永远随机天气/地点 —— 他会以为是设备坏了）。
   ── 为什么是「钩子」而不是直接塞标志：
      `ws_provision()` 是**阻塞**的（跑在配网等待循环里，主循环此时还没起来），
      触摸只能由它自己每 50ms 轮询。所以把「按住多久」的探测交给 main.c
      （只有它握着 tp_poll 和屏幕反馈），这里只认返回值：
        · 返回 ≥3000（ms）→ 立刻跳过（与长按 BOOT **同一条出口**）；
        · 返回 0（没按住 / 松手）→ 计时清零。
      没注册（= NULL，按键版）→ 恒为 0，行为与旧版完全一致。 */
typedef int (*prov_touch_hold_fn)(void);   /* 返回「底部跳过按钮已连续按住」的毫秒数 */
void ws_prov_set_touch_probe(prov_touch_hold_fn fn);

/* ★R206（用户令 2026-11-13）：「关机应该在任何页面都能用」——
   配网引导是**阻塞**在 ws_provision() 里的（主循环/按键任务都不跑），
   原来 POWER 在这页完全没反应。这里由 main.c 注册一个「关机执行体」，
   net_prov 的等待循环里检测到 POWER 按住 ≥2s 就调用它（内部不返回）。 */
typedef void (*prov_poweroff_fn)(void);
void ws_prov_set_poweroff_fn(prov_poweroff_fn fn);

/* ★ 2026-10-05（用户 bug②「配网引导左上角有返回按钮但是点不回来」）：
   配网页**阻塞在 ws_provision() 里**，主循环的 touch_tap 压根不会被调用 ⇒
   标题栏那个返回箭头是**死键**。修法：探针函数（唯一能在阻塞期摸到触摸的地方）
   顺便认「点左上角返回箭头」，返回这个**假毫秒数**冒充"按住够久"，
   于是复用同一条跳过出口 —— 语义正好：**返回 = 我不配网了，进主页**。
   为什么点一下就生效（不像跳过要按住 3 秒）：返回是**有意的即时操作**，
   用户看见箭头就是要走；3 秒门槛是给"跳过配网"防蹭的，两者意图不同。 */
#define PROV_BACK_TAP_MS    3000    /* ≥ PROV_TOUCH_SKIP_HOLD_MS 即可（放在 net_prov.c） */

/* ★★ 2026-09-22：**按键优先** —— 联网等待期间用户一按键就立刻放弃等待。
   为什么必须做：`ws_sync()` 是同步调用，内部 WiFi 20s + 城市 10s + 天气 12s + 对时 15s
   ≈ **最长 57 秒**，这期间主循环完全不扫按键 → 用户看到的现像是「设备不响应/像没电了」。
   本函数由 `main.c` 实现（只有它知道按键引脚），net_sync.c 里每个等待循环都要查它。 */
bool ws_key_pressed(void);

/* ★★ 2026-10-07（用户需求）：**自动转连** —— 已存 A/B/C 多个 WiFi 时，当前网（A）没信号
   而别的（B）在场 → 同步流程里自动转连 B，并把「当前网」切到 B（写 cfg + NVS）。
   ── 为什么做成两个钩子：WiFi 已存列表（g_wifi_names/pass）与「当前网标记」都归 main.c
      管，net_sync 只负责「连不上时怎么找备选、怎么连」—— 数据与落账各归其主
      （与上面 prov_touch_hold_fn 同款风格：钩子没注册 = 行为同旧，只连当前那条）。
   ── 耗电口径（用户问过）：深睡待机**零变化**；只在「当前条连不上」那次同步里多一次
      扫描（~2s）+ 至多 3 条 × 6s 的备选试连，最坏 +26s；正常（当前条在场）路径
      **零增量**，反而省掉原来 20s 干等超时的白耗。 */
typedef int (*ws_roam_list_fn)(char (*ssids)[33], char (*passes)[65], int max);
/* 返回已存条数并填充 ssids/passes（最多 max 条）。 */
void ws_set_roam_list_get(ws_roam_list_fn fn);

typedef void (*ws_roam_commit_fn)(const char *ssid);
/* 漫游转连成功后回调：main.c 把「当前网」的内存标记（g_wifi_cur）对齐到新网
   （NVS 的 ssid/pass 由 net_sync 自己经 ws_save_config 落盘，不经这里）。 */
void ws_set_roam_commit(ws_roam_commit_fn fn);

/* ★★ 2026-10-11 R26（收藏品/性格系统 · 三个事件钩子）────────────────────────
 * 出门/回家/同步是**事件**；掉落与里程碑结算归 main.c（收藏品状态都在那边），
 * net_sync 只负责在正确的时刻发信号 —— 与上面 roam 钩子同款风格：
 * 没注册（NULL）= 行为与旧版完全一致。 */
typedef int (*ws_trip_start_fn)(time_t out_at, int weather);
/* 猫出门那一刻（step_life 掷骰命中后）：main.c 记 trip_total、掷去向落 NVS、
   雪天+山野发雪花结晶。
   ★★ 2026-10-16 远行（机制文档 §6）：返回**覆盖用的出门小时数**（0 = 不覆盖）——
   套装齐全时 main.c 自动出发远行（4~7 天），返回 N*24 让 step_life 把返家时刻
   拉到 N 天后；普通短途返回 0，行为与旧版完全一致。 */
void ws_set_trip_start(ws_trip_start_fn fn);

typedef int (*ws_trip_plan_fn)(time_t out_at, int weather);
/* ★★ 2026-10-18 方案甲（场合接线）：出门掷骰**前**先问「这次去哪」——
   main.c 只掷不改状态：远行可去返回 LOOT_SER_ABROAD，否则普通去向系列（0..2）。
   step_life 用它把场合类效果（城里/山野/海滨的出门率、出门时长）按场合计入；
   真出发时 trip_start 沿用同一预告，不再重掷。没注册（NULL）= 场合效果只在
   trip_h 上按"无去向"口径计（NONE 件生效）。 */
void ws_set_trip_plan(ws_trip_plan_fn fn);

typedef void (*ws_trip_return_fn)(time_t out_at, time_t out_ts);
/* 猫回家那一刻（step_life 检测到返家）：main.c 按去向掉落表掷骰、里程碑复查。 */
void ws_set_trip_return(ws_trip_return_fn fn);

typedef bool (*ws_trip_alive_fn)(void);
/* ★★★ R101（2026-10-28）：step_life 的「幽灵出门态」自愈要区分两种在外面：
   · 远行在途（4~10 天 = 96~240h）→ **合法**，不能碰；
   · 普通出行（上限 OUT_LONG_MAX_H = 32h）→ 超过上界还在外面 = 双键不同步的脏态。
   而 net_sync 不认识 exp_*（远行状态在 main.c 侧），所以用钩子问一句「当前是否在远行」。
   没注册（NULL）= 视为**不在远行** ⇒ 超过 34h 的出门一律判脏态强制归家。 */
void ws_set_trip_alive(ws_trip_alive_fn fn);

typedef void (*ws_sync_done_fn)(bool net_ok);
/* 每次联网同步收尾（成功失败都通知；net_ok = 本次 WiFi 是否连通）——
   main.c 拿它做「空箱保底羽毛：每次同步 40%、第 3 次必得」。 */
void ws_set_sync_done(ws_sync_done_fn fn);

/* ══ 添加朋友热点（SoftAP + 门户页，实现在 net_prov.c）════════════════
 * ★2026-10-26：原先只在 main.c 里写成本地 extern（friend.c 搬进来后编译失败），
 *   收进头文件 —— 谁都能声明一次，别再各自写 extern 副本。 */
bool ws_friend_open(void);
void ws_friend_close(void);
void ws_friend_ap_ssid(char *out, int cap);
void ws_friend_set_add_cb(int (*cb)(const char *code, const char *name));
/* ★★ R92：编辑目标（>=0 = 修改第 i 位好友）。门户网页据此预填码与名、改标题/按钮文案；
   提交仍走同一个 add_cb（friend.c 内部按编辑目标分派到 friend_update）。-1 = 新增。 */
void ws_friend_set_edit(int idx);

/* ══ 改名卡门户（SoftAP + /name，实现在 net_prov.c）════════════════════
 * ★2026-10-26 用户令：改名卡用掉 → 名字先变 "-" → 手机连热点自动打开取名页；
 *   取名成功由 h_namesave 直写 NVS；3 分钟无人操作自动收热点。 */
bool ws_name_open(void);
void ws_name_close(void);
bool ws_name_portal_open(void);       /* ★R153：离页收口判断门户是否还开着 */

#ifdef __cplusplus
}
#endif
