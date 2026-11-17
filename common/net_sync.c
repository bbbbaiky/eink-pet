/*
 * net_sync.c — 真实世界同步实现（WiFi + SNTP 时间 + Open-Meteo 天气 + 月相 + 出门）
 * ===========================================================================
 * 见 net_sync.h 的总体说明。本文件只做「数据获取与计算」，不做 UI。
 *
 * ★ 设计上的两个硬约束（都是用户明确要求的）：
 *   ① **永不空屏**：任何一步失败都必须降级到缓存/默认值，绝不出现空画面。
 *   ② **不阻塞太久**：联网有超时上限，超了就带着缓存继续跑（墨水屏用户不会
 *      盯着屏幕看进度条，慢一点没关系，但绝不能卡住不刷新）。
 * ===========================================================================
 */
#include <esp_attr.h>       /* ★R198：EXT_RAM_BSS_ATTR（BSS 外置 PSRAM） */
#include "net_sync.h"
#include "loot_fx.h"      /* ★ 2026-10-16 收藏品效果聚合核心 */
#include "city_coords.h"   /* ★R217 冻结资产：离线烘焙的省+市→经纬度表，见本文件头注释 */
#include "world_cities.h"        /* 2026-10-14 海外城市解耦：CC:City 键 → 坐标/名/国家码 */
#include "city_place.h"          /* ★R158：city_place_en 原型 */
#include "world_cities_data.h"   /* 生成表 WORLD_CITIES[] / N_WORLD_CITIES（scripts/gen_world_cities.py） */
#include "i18n.h"                /* ★G（2026-10-16）：远行 API 提示词按设备语言四语化（g_lang） */

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>   /* ★G：远行 API 走 HTTPS，证书束（两版 sdkconfig 均已 CONFIG_..._BUNDLE=y） */
#include <esp_timer.h>
#include <esp_random.h>    /* ★ 2026-09-21：出门时长用 esp_random() 抽签 */
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>

static const char *TAG = "net_sync";

#define NVS_NS      "petns"
#define WIFI_BIT_OK BIT0
#define WIFI_BIT_FAIL BIT1

/* ★R175：亲密度硬边界（地板/天花板）—— 全文件唯一真源。
   原来 20/100 在 ws_load_state / step_life / ws_intimacy_add 三处各写一遍字面量，
   改一处漏两处就会出现「读的时候夹 100、写的时候能写 105」。 */
#define INTI_FLOOR       20
#define INTI_CEIL       100

/* 城市经纬度表 —— ★ 2026-09-19 起**只作兜底**。
 *
 * 用户订正后的口径：「城市」= 使用者居住的城市、**精确到区**（如「上海市浦东新区」），
 * 只用来同步天气。精确到区就不可能靠一张表覆盖，所以主路径改成联网做**地理编码**
 * （见下面 ws_resolve_city）；这张省会级小表只在「地理编码全失败」时兜一下，
 * 保证天气至少是同一省会的、画面永不空。 */
typedef struct { const char *name; float lat, lon; int tz_sec; } city_t;
static const city_t CITIES[] = {
    { "上海",   31.23f, 121.47f,  8 * 3600 },
    { "北京",   39.90f, 116.40f,  8 * 3600 },
    { "深圳",   22.54f, 114.06f,  8 * 3600 },
    { "广州",   23.13f, 113.26f,  8 * 3600 },
    { "杭州",   30.27f, 120.16f,  8 * 3600 },
    { "成都",   30.57f, 104.07f,  8 * 3600 },
    { "东京",   35.68f, 139.69f,  9 * 3600 },
    { "富士山", 35.36f, 138.73f,  9 * 3600 },
    { "香港",   22.30f, 114.17f,  8 * 3600 },
    { "台北",   25.03f, 121.57f,  8 * 3600 },
    { "新加坡",  1.35f, 103.82f,  8 * 3600 },
    { "伦敦",   51.51f,  -0.13f,  0 * 3600 },
    { "纽约",   40.71f, -74.01f, -5 * 3600 },
    { "洛杉矶", 34.05f, -118.24f, -8 * 3600 },
    { "悉尼",  -33.87f, 151.21f, 10 * 3600 },
};
#define N_CITY (sizeof(CITIES) / sizeof(CITIES[0]))

/* ★ 已删除 city_land()（2026-09-19）：「由城市反推窗外风景」是错的耦合。
   现在风景 = 配网时用户在下拉里选的那一项（cfg->land），与城市/天气无关。 */

/* ★R195：窗外风景的地标名 / 合法化。顺序必须与 net_sync.h 的 LAND_* 以及
   scripts/_tools/gen_land_face.py 的 LANDS 一致（0 富士山 · 1 东方明珠 · 2 星球）。
   只用于**日志**和 NVS 里那份人可读副本（scenery）；界面文案走 main.c 的 LAND_NAME + T()。 */
const char *ws_land_name(int land)
{
    static const char *const NM[N_LAND] = { "富士山", "东方明珠", "星球" };
    return (land >= 0 && land < N_LAND) ? NM[land] : NM[LAND_PEARL];
}

static int land_clamp(int land)
{
    return (land >= 0 && land < N_LAND) ? land : LAND_PEARL;
}

/* ---------------------------------------------------------------- NVS */

static bool nvs_get_str_into(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) return false;
    return true;
}

bool ws_init(ws_config_t *cfg_out)
{
    memset(cfg_out, 0, sizeof(*cfg_out));
    /* ★★★ 2026-10-10 修（隐患 · 「NVS 初始化失败就整片擦除」）：
       原来对**任何**失败都 `nvs_flash_erase()` —— 那一刀下去是**用户的全部数据**
       （WiFi 密码 / 城市 / 窗景 / 已读明信片 / 收藏品 / 食盆 / 引导已看标志）。
       而 `nvs_flash_init()` 只在**两种**错误下才该重建：
         · ESP_ERR_NVS_NO_FREE_PAGES    —— 分区页耗尽，官方口径就是擦；
         · ESP_ERR_NVS_NEW_VERSION_FOUND —— 分区格式升级（换 IDF 大版本）。
       其它失败（ESP_ERR_NO_MEM / 分区表里没配 NVS / 底层 SPI 故障…）擦了**也修不好**，
       白白把用户数据赔进去 —— 而且是在"只要开机就发生"的路径上（ws_init 每次冷启动都跑）。
       ⇒ 只对这两码重建；其余**保留数据**并明确报错。
       ★ 与 main.c 的 app_main 里那两处 `nvs_flash_init` 已经是一个口径（那里本来就
         按这两码判），此处属于**补齐不一致**。 */
    esp_err_t ne = nvs_flash_init();
    if (ne == ESP_ERR_NVS_NO_FREE_PAGES || ne == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建（%s）→ 擦除后重新初始化（用户数据本就要丢）",
                 esp_err_to_name(ne));
        /* ⚠ 故意**不**用 ESP_ERROR_CHECK：擦除失败时 panic 会把设备卡在重启循环里，
           而这里只是"配置读不出来"—— 交给下面的 nvs_flash_init 再试一次，
           真不行就让 ws_init 返回 false（上层会走配网引导），至少设备还能用。 */
        esp_err_t ee = nvs_flash_erase();
        if (ee != ESP_OK) ESP_LOGE(TAG, "  NVS 擦除失败（%s）→ 仍继续尝试重新初始化",
                                   esp_err_to_name(ee));
        ne = nvs_flash_init();
    }
    if (ne != ESP_OK) {
        ESP_LOGE(TAG, "★★ NVS 初始化失败（%s）→ 本次不用 NVS；**不擦除**，保住用户配置",
                 esp_err_to_name(ne));
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    bool ok = nvs_get_str_into(h, "ssid", cfg_out->ssid, sizeof(cfg_out->ssid))
           && cfg_out->ssid[0] != 0;
    nvs_get_str_into(h, "pass", cfg_out->pass, sizeof(cfg_out->pass));
    nvs_get_str_into(h, "city", cfg_out->city, sizeof(cfg_out->city));
    int32_t lat_i = 0, lon_i = 0, tz_i = 0, tz_ok_i = 0;
    nvs_get_i32(h, "lat", &lat_i);
    nvs_get_i32(h, "lon", &lon_i);
    nvs_get_i32(h, "tz",  &tz_i);
    /* ★★ 2026-10-07 修：「有没有配过时区」必须用**另一个键**表达，不能拿 tz==0 当哨兵。
       ── 0 是**合法时区**（UTC±0：伦敦 / 里斯本 / 加纳…），拿它当哨兵 = 这些用户被
          永久错判成 +8（时间、时段、月相全跟着错）。
       ── 更糟的是回写死循环：fetch_weather 校准拿到 utc_offset_seconds=0 → 写进 NVS
          → 下次开机这里读成 0 → 又判「没配过」→ 又给 +8 → 下次同步又写 0 …… 永远改不回来。
       ── 老设备 NVS 里没有 tzok（读出 0）→ 仍走 +8 默认，行为与改前完全一致，安全。 */
    nvs_get_i32(h, "tzok", &tz_ok_i);
    /* ★★ 2026-10-04 体检修：land 必须在 nvs_close 之前读。
       原来这行在 nvs_close(h) 之后 —— nvs_close 会把句柄槽清成 NVS_INVALID_HANDLE，
       再用 h 读直接返回 ESP_ERR_INVALID_HANDLE，而返回码没接 → land_i 停在初值
       LAND_PEARL → **用户在设置里选的富士山每次重启都被强制回东方明珠**。 */
    int32_t land_i = LAND_PEARL;
    if (nvs_get_i32(h, "land", &land_i) != ESP_OK) land_i = LAND_PEARL;
    nvs_close(h);

    cfg_out->lat = lat_i / 10000.0f;      /* 存的是万分之一度，避免 NVS 浮点 */
    cfg_out->lon = lon_i / 10000.0f;
    /* ★2026-10-07：判据从「tz_i != 0」改成「tzok 已置位」—— 见上面 tzok 的注释。 */
    cfg_out->tz_offset_sec = (tz_ok_i == 1) ? tz_i : 8 * 3600;
    cfg_out->land = land_clamp(land_i);
    cfg_out->valid = ok;
    if (ok) {
        ESP_LOGI(TAG, "配置：SSID=%s 城市=%s（精确到区）窗外风景=%s (%.2f,%.2f) TZ=%+d 小时",
                 cfg_out->ssid, cfg_out->city, ws_land_name(cfg_out->land),
                 cfg_out->lat, cfg_out->lon,
                 cfg_out->tz_offset_sec / 3600);
    } else {
        ESP_LOGW(TAG, "NVS 里没有 WiFi 配置 → 需要配网");
    }
    return ok;
}

/* 把配置写进 NVS。
 * ⚠️ 配网路径（net_prov.c）里有一份内联实现，因为它在另一个文件里、
 *    需要立刻落盘。这里保留一份供「程序内改配置」用（比如以后加
 *    「长按 BOOT 重置配网」）。
 * ★ 2026-10-04 体检清理：原来这段注释下方还有一段自相矛盾的补充
 *   「现在真的会被调用了…所以去掉 __attribute__((unused))」，
 *   两段描述同一件事的相反状态、且中间夹着函数定义，读者要读完才知道哪个作废。
 *   —— 只保留本段（结论：这个函数确实在用）。
 * ★★★ 2026-10-10 新增 `tz_known` 参数（隐患修 · 见下方 tzok 那段注释）：
 *   tz/tzok **只在时区真值已知时**才落盘；只是"换了当前网"的调用（漫游转连）
 *   传 false → 这两个键**保持 NVS 原值不动**。 */
static void ws_save_config(const ws_config_t *cfg, bool tz_known)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", cfg->ssid);
    nvs_set_str(h, "pass", cfg->pass);
    nvs_set_str(h, "city", cfg->city);
    nvs_set_i32(h, "land", cfg->land);
    /* 人可读副本，方便用 nvs 工具直接看（真源仍是 land 整数） */
    nvs_set_str(h, "scenery", ws_land_name(cfg->land));
    nvs_set_i32(h, "lat", (int32_t)(cfg->lat * 10000.0f));
    nvs_set_i32(h, "lon", (int32_t)(cfg->lon * 10000.0f));
    /* ★★★ 2026-10-10 修（隐患 · 把"临时兜底 +8"晋升成"用户既成事实"）：
       `tzok` 的语义是「**这个 tz 值是可信来源给的**」（见 ws_load_config 的注释）。
       所以它只能在**真的拿到真值**时置位 —— 即 `fetch_weather` 用 `timezone=auto`
       拿到 `utc_offset_seconds` 之后（本文件 784-791，那里传 tz_known=true）。
       反面案例（原来就是这样）：**自动漫游转连**到另一条记忆网络时也调本函数，
       而那时 `cfg->tz_offset_sec` 只是 `ws_load_config()` 给的 +8 兜底值
       —— 这一笔写下去就把 +8 钉死了，之后 `ws_load_config` 永久按 `tzok==1` 采纳它，
       **非 +8 用户的时钟再也不会自愈**（时间/时段/月相全错）。
       ⇒ 非可信来源一律**不碰**这两个键（保留 NVS 原值：原来可信就还是可信，
         原来未知就仍然未知，等第一次联网自动回填）。 */
    if (tz_known) {
        nvs_set_i32(h, "tz",  cfg->tz_offset_sec);
        nvs_set_i32(h, "tzok", 1);   /* ★2026-10-07：配过就置位（见 ws_load_config 的 tzok 注释） */
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "配置已写入 NVS（城市=%s 风景=%s %.4f,%.4f TZ=%+d%s）",
             cfg->city, ws_land_name(cfg->land),
             cfg->lat, cfg->lon, cfg->tz_offset_sec,
             tz_known ? "" : " · 时区未定（本次不写 tz/tzok，保留原值）");
}

/* ★★ 2026-09-23 新增：设置 →「窗外风景」页选定地标后调用（声明见 net_sync.h）。
   ★ 只动 land，**不碰 city / lat / lon / tz** —— 风景只决定画面里的地标，
     与「住哪、查哪的天气」完全解耦（这是 2026-09-19 用户订正过的口径）。 */
void ws_set_land(ws_config_t *cfg, ws_state_t *st, int land)
{
    int v = land_clamp(land);
    cfg->land = v;
    if (st) st->land = v;          /* render_key 拌了 st->land → 回主页必然重刷 */

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "land", v);
        nvs_set_str(h, "scenery", ws_land_name(v));
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "★ 窗外风景已切换 → %s（land=%d 已落 NVS，城市/天气不受影响）",
             ws_land_name(v), v);
}

/* ---------------------------------------------------------------- WiFi */
static EventGroupHandle_t s_wifi_eg = NULL;

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_BIT_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_BIT_OK);
    }
}

static bool wifi_stack_once(void)
{
    static bool inited = false;
    if (inited) return true;
    ESP_ERROR_CHECK(esp_netif_init());     /* 幂等：已初始化时直接返回 ESP_OK */
    /* ★★ 2026-09-21 修复：同一次开机里可能**配网已经先把这套栈建好了**
       （配网提交成功 → 紧接着就要试连一次），此时再调：
         · esp_event_loop_create_default() → ESP_ERR_INVALID_STATE
         · esp_netif_create_default_wifi_sta() → 内部 ESP_ERROR_CHECK 直接 abort
       旧代码两个都硬 ESP_ERROR_CHECK 包着 → 「提交配网」那一下会 abort 重启。
       （NVS 已落盘，重启后又能连上，所以台面上只表现为「配网成功页一闪而过」。）
       esp_wifi_init() 不用管：它内部 `if (s_wifi_inited) return ESP_OK;`，本来就是幂等的。 */
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    inited = true;
    return true;
}

/* ★R186：当前是否**已经关联上**某个 AP（GOT_IP 之前的那一步）。
   ⚠ 本 IDF（v5.5.3）的 wifi_ap_record_t 没有 sta_connected 字段，只能靠
   esp_wifi_sta_get_ap_info() 的返回值判断（未关联 → ESP_ERR_WIFI_STA_NOT_CONNECTED）。 */
static bool ws_sta_associated(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

bool ws_wifi_connect(const ws_config_t *cfg, int timeout_ms)
{
    if (!cfg->valid || !cfg->ssid[0]) return false;
    wifi_stack_once();

    /* ★★★★★ 2026-10-05 用户 bug⑱「点了配网成功页，屏幕 22 秒不动」
     *   —— 真凶就是这个函数里 20 秒的"断开重连 + 超时"，不是触摸。
     *
     *   配网成功的瞬间，STA **已经连上** cfg->ssid 并拿到 IP 了
     *   （net_prov 那边是拿现成连接去试连的，log 有
     *   `wifi:sta is connected, disconnect before connecting to new ap`
     *   —— 这句正是"驱动要先断开才肯换配置"的内核打印）。
     *   ⇒ 紧接着调本函数时会：esp_wifi_set_config 触发断开 → esp_wifi_connect
     *     → 而此时 xEventGroupClearBits 刚清过 WIFI_BIT_OK，
     *     GOT_IP 事件可能**在 clear 之前就已经来过**了 ⇒ 一个 bit 都等不到
     *     → 干等满 timeout_ms（配网路径给的是 20000）→ 用户看着"点了没反应"。
     *
     *   修法：**先问驱动"现在连着谁"**。已经是目标 SSID 且拿到了 IP 就直接
     *   返回 true，一步都不等。这不是优化，是消除一个 20 秒的假死窗口。 */
    {
        wifi_ap_record_t ap;
        /* ⚠ 本 IDF 版本（v5.5.3）的 wifi_ap_record_t **没有 sta_connected 字段**，
         *   判"连上了"只能靠 esp_wifi_sta_get_ap_info 返回 ESP_OK —— 它在 STA 没连上时
         *   返 ESP_ERR_WIFI_STA_NOT_CONNECTED。**别自己加字段判断，会编译不过。 */
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
            strncmp((const char *)ap.ssid, cfg->ssid, sizeof(ap.ssid)) == 0) {
            /* ★★ 2026-10-13 任务二 P1-3 修（根因）：光「关联着目标 AP」**不等于**持有效 IP。
             *   僵尸关联实证：问卷阻塞 60s+ 期间 AP 把空闲关联踢掉，STA 侧关联状态在发流量
             *   前一直存活 → 早退返回 true → 天气空等 12s + 对时 15s 才降级兜底
             *   （用户 10-11 实报「做完题不会自动连、时间天气还是错」的另一半根因）。
             *   触屏版 10-11 只在调用点打了「问卷后 disconnect」补丁；这里补上根因侧判据，
             *   两条路共同收口。拿不到 IP → 不早退，照常走下面的重连流程。 */
            esp_netif_ip_info_t ip;
            bool has_ip = false;
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0)
                has_ip = true;
            if (has_ip) {
                ESP_LOGI(TAG, "WiFi 仍连在目标「%s」且持有效 IP → 直接返回，不重连",
                         cfg->ssid);
                fflush(stdout);
                return true;
            }
            ESP_LOGW(TAG, "★ 关联着目标「%s」但**无有效 IP**（僵尸关联）→ 不早退，走真实重连",
                     cfg->ssid);
            fflush(stdout);
        }
    }

    if (!s_wifi_eg) s_wifi_eg = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_eg, WIFI_BIT_OK | WIFI_BIT_FAIL);

    wifi_config_t wc = {0};
    /* ★R207 修（复查）：原来是 strncpy(size-1) —— SSID 恰为 32 字节 / 密码恰为 64 字节时
       会被截掉最后 1 个字符（wifi_config 的 ssid 是定长 32、password 定长 64，本来就
       允许存满、不需要 NUL），结果试连必失败、配网永远显示「连接失败」。
       改成按定长 memcpy（wc 已清零，余下字节自然为 0）。 */
    memcpy(wc.sta.ssid, cfg->ssid, strnlen(cfg->ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, cfg->pass, strnlen(cfg->pass, sizeof(wc.sta.password)));
    /* 开放网络（无密码）要允许空密码 */
    wc.sta.threshold.authmode = cfg->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* ★R185 实测记录：曾试过「把上次的信道写进 wc.sta.channel 跳过全信道扫描」，
       本机第二次启动直接锁在 11 信道反复关联失败 14s+（AP 实际信道变了），
       而且**连接中改 config 不生效**，清提示那一步也救不回来 ⇒ 已回滚。
       结论：信道锁定只在「AP 固定信道」的环境成立，跨环境是负优化，别再试。 */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    /* ★★ 2026-10-04 体检修：原来这里是裸的 `ESP_ERROR_CHECK(esp_wifi_start())`，
       假设「WiFi 栈是首次启动」。但**配网成功那条路恰好违反这个假设**：
       ws_provision 成功 → prov_portal_stop(srv, keep_sta=true) 只把模式从 APSTA 降到
       STA，**驱动仍处于 started** → 同一次开机里紧接着的第一次 ws_sync 走到这里
       → esp_wifi_start() 返回 ESP_ERR_WIFI_NOT_STOPPED → ESP_ERROR_CHECK 直接 abort 重启。
       这就是 188 行注释描述的「配网成功页一闪而过」（NVS 已落，重启后又能连上，
       所以台面上不容易看出来）。上一轮修了 event loop 和 netif，唯独这一处是裸的。
       —— 改成与同文件 ws_wifi_switch（288-297 行）同款的「按需启动」。 */
    esp_err_t wifi_start_err = ESP_OK;
    {
        esp_err_t e = esp_wifi_start();
        wifi_start_err = e;
        if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_STOPPED) {
            ESP_LOGE(TAG, "★ esp_wifi_start 失败（%s）", esp_err_to_name(e));
            fflush(stdout);
            return false;                 /* 失败就返回 false，让调用方走缓存兜底，不 abort */
        }
    }
    ESP_LOGI(TAG, "连接 WiFi「%s」…", cfg->ssid);
    fflush(stdout);

    /* ★★ 2026-10-07 修 + ★R171 全量检查：
       STA_START 事件回调（wifi_evt）里**已经会自动 connect**；只有「栈本来就是 started」
       （esp_wifi_start 返回 NOT_STOPPED，不会再发 STA_START）才需要显式补一次。
       原来无条件补 → 首次启动同一瞬间发两次 connect，IDF 报
       `E wifi:sta is connecting, return error`（串口里每次开机三条红字）。
       定时重试（2.5s）仍在，万一事件没来也有兜底。 */
    if (wifi_start_err == ESP_ERR_WIFI_NOT_STOPPED) esp_wifi_connect();

    /* 分片等待：每片失败就重试，总共不超过 timeout_ms
       ★★ 2026-09-22：分片从 8s 改小到 **1s**，且每片都查一次按键 ——
       原来的 8s 片意味着「用户按了键，最多 8 秒后才有反应」，仍然像卡死。
       重试节奏没变（仍是每 8s 一次 `esp_wifi_connect()`），只是把「查按键」的粒度调细。 */
    int waited = 0;
    /* ★R156：原 8000 配上 8s 预算 = 一次定时重试都发不出去（waited 到 8000 时
       while 已退出），弱网第一次关联抖动就整轮失败。改 2.5s 节奏。 */
    int next_retry = 2500;
    const int SLICE = 1000;
    while (waited < timeout_ms) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_eg, WIFI_BIT_OK | WIFI_BIT_FAIL, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(SLICE));
        if (bits & WIFI_BIT_OK) {
            ESP_LOGI(TAG, "WiFi 已连接（%d ms）", waited);
            fflush(stdout);
            return true;
        }
        /* ★R156：DISCONNECTED 是确凿的关联失败，wifi_evt 已置 WIFI_BIT_FAIL；
           原写法把这一位清掉后继续干等满预算（同文件 wifi_wait_ip 早已修成
           「FAIL 立即返回」）。这里立即重发 connect，别白烧剩余预算。 */
        if (bits & WIFI_BIT_FAIL) {
            ESP_LOGW(TAG, "WiFi 关联失败（%d ms）→ 立即重试", waited);
            /* ★R186：先问驱动「现在是不是又关联上了」。事件是异步的，FAIL 到达时
               上一轮 connect 可能已经生效 —— 这时再 esp_wifi_connect() 会被驱动
               「先断开再重连」（串口实证 `sta is connected, disconnect before
               connecting to new ap`），白丢一次 IP。 */
            if (!ws_sta_associated()) esp_wifi_connect();
            next_retry = waited + 2500;
        }
        waited += SLICE;
        /* ★ 按键优先：用户一按键就放弃这次等待，把控制权立刻交回主循环 */
        if (ws_key_pressed()) {
            ESP_LOGW(TAG, "★ 用户按键 → 放弃 WiFi 连接等待（已等 %d ms）", waited);
            fflush(stdout);
            return false;
        }
        /* ★2026-10-07：原来是 `waited % 8000 == 0` —— 两个毛病：
             ① 依赖 waited 恰好整除 8000（SLICE=1000 时确实整除，但换成别的 SLICE 就失效）；
             ② timeout_ms 正好 =8000 时，唯一一次成立发生在 waited 刚变 8000 那轮：
                发完 connect 回头 while(8000<8000) 不成立 → 立刻判超时
                ⇒ **连接请求在最后一刻发出、然后马上认输**，永远连不上。
           —— 改成独立的「下次重试时刻」，与超时长度无关。 */
        /* ★★ 2026-10-13 任务二 P2-14 修（补上 10-07 那笔的漏）：重试判定加
         *   `waited < timeout_ms` —— 否则 timeout_ms 恰为 8000 的整边界上，最后一次
         *   循环 waited 刚加到 8000：重试 connect 照发，回头 while(8000<8000) 不成立
         *   立刻 return false ⇒ 连接请求本函数内永远等不到结果，上层误判「连不上」
         *   触发不必要的漫游。加上条件后：没预算等结果的 connect 不发。 */
        if (waited >= next_retry && waited < timeout_ms) {
            /* ★R186：已关联、只是在等 DHCP（GOT_IP）⇒ **不要再 connect**。
               串口实证：连接成功 6.3s 后定时重试仍触发，驱动先断开再重连，
               白花 1~2s（用户报的「连 WiFi 有点久」的另一半）。 */
            if (!ws_sta_associated()) {
                ESP_LOGW(TAG, "WiFi 定时重试（%d ms）…", waited);
                fflush(stdout);
                esp_wifi_connect();
            }
            next_retry = waited + 2500;      /* ★R156：2.5s 一次，直到预算用完 */
        }
    }
    ESP_LOGE(TAG, "WiFi 连接超时（%d ms）", timeout_ms);
    fflush(stdout);
    return false;
}

/* ★★ 2026-09-23：**运行时切换 WiFi**（规格 §11.2「切换：改 cur_wifi 索引 → 断开重连，
   全程零重新配网」）—— 不重启、不重新配网。
   与 ws_wifi_connect 的区别：那个假设 WiFi 栈是**首次启动**（无条件 esp_wifi_start，
   且用 ESP_ERROR_CHECK 包着）—— 同一次开机里第二次调它会撞 ESP_ERR_WIFI_NOT_STOPPED
   直接 abort。本函数**按需启动**，并且切换前先断开旧连接。
   返回 true = 拿到了 IP（凭据有效、信号可用）；调用方成功后再回写 cfg + NVS。 */
bool ws_wifi_switch(const char *ssid, const char *pass, int timeout_ms)
{
    if (!ssid || !ssid[0]) return false;
    wifi_stack_once();

    if (!s_wifi_eg) s_wifi_eg = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_eg, WIFI_BIT_OK | WIFI_BIT_FAIL);

    wifi_config_t wc = {0};
    /* ★R207：同 ws_wifi_connect —— 用定长 memcpy，别把 32 字节 SSID / 64 字节密码截掉 1 位。 */
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, pass ? pass : "", strnlen(pass ? pass : "", sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* ★ 按需启动：WiFi 栈可能已经起过（同一次开机里先同步过 / 刚配过网）。
       注意 WIFI_EVENT_STA_START 的处理函数里会自动 esp_wifi_connect()，
       所以下面先 disconnect 再 set_config，保证最终用的是新配置。 */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        esp_err_t e = esp_wifi_start();
        if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_STOPPED) {
            ESP_LOGE(TAG, "★ 切换 WiFi：esp_wifi_start 失败（%s）", esp_err_to_name(e));
            fflush(stdout);
            return false;
        }
    }
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(110));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "切换 WiFi →「%s」…", ssid);
    fflush(stdout);
    esp_wifi_connect();

    int waited = 0;
    int next_retry = 8000;        /* ★2026-10-07：同 ws_wifi_connect，取代取模判据 */
    const int SLICE = 1000;
    while (waited < timeout_ms) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_eg, WIFI_BIT_OK | WIFI_BIT_FAIL, pdTRUE, pdFALSE, pdMS_TO_TICKS(SLICE));
        if (bits & WIFI_BIT_OK) {
            ESP_LOGI(TAG, "★ 已切到「%s」（%d ms）", ssid, waited);
            fflush(stdout);
            return true;
        }
        waited += SLICE;
        if (ws_key_pressed()) {                  /* 按键优先，同 ws_wifi_connect */
            ESP_LOGW(TAG, "★ 用户按键 → 放弃切换等待（已等 %d ms）", waited);
            fflush(stdout);
            return false;
        }
        if (waited >= next_retry) {              /* ★2026-10-07：同 ws_wifi_connect 的理由 */
            /* ★R186：同 ws_wifi_connect —— 已关联就不要再 connect（会先断开再重连） */
            if (!ws_sta_associated()) {
                ESP_LOGW(TAG, "切换重试第 %d 次…", next_retry / 8000);
                fflush(stdout);
                esp_wifi_connect();
            }
            next_retry += 8000;
        }
    }
    ESP_LOGE(TAG, "★ 切换到「%s」超时（%d ms）→ 判定连接失败", ssid, timeout_ms);
    fflush(stdout);
    return false;
}

/* ---------------------------------------------------------------- 时间 */

static volatile bool s_sntp_done = false;
static void sntp_cb(struct timeval *tv)
{
    s_sntp_done = true;
    ESP_LOGI(TAG, "SNTP 已同步");
    fflush(stdout);
}

/* 返回 true = 拿到有效 UTC 时间 */
static bool fetch_utc_time(int timeout_ms)
{
    s_sntp_done = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");     /* 国内兜底，更快 */
    esp_sntp_init();
    sntp_set_time_sync_notification_cb(sntp_cb);

    int waited = 0;
    while (!s_sntp_done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        /* ★ 按键优先：没对上时也别干等（这段时间主循环同样扫不到按键） */
        if (ws_key_pressed()) {
            ESP_LOGW(TAG, "★ 用户按键 → 放弃 SNTP 等待（已等 %d ms）", waited);
            fflush(stdout);
            break;
        }
    }
    esp_sntp_stop();
    if (!s_sntp_done) {
        ESP_LOGW(TAG, "SNTP 超时（%d ms）", timeout_ms);
        return false;
    }
    /* 校验：年份合理（>2024）才算真的同步上了 */
    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    if (tm_utc.tm_year + 1900 < 2024) {
        ESP_LOGW(TAG, "SNTP 给的年份 %d 不合理", tm_utc.tm_year + 1900);
        return false;
    }
    return true;
}

int ws_period_of(time_t local_epoch)
{
    struct tm t;
    gmtime_r(&local_epoch, &t);
    int h = t.tm_hour;
    if (h >= 5  && h < 6)  return PERIOD_BREAK;   /* 05:00-05:59 破晓（视觉复用傍晚） */
    if (h >= 6  && h < 11) return PERIOD_DAWN;    /* 06:00-10:59 */
    if (h >= 11 && h < 17) return PERIOD_NOON;    /* 11:00-16:59 */
    if (h >= 17 && h < 20) return PERIOD_DUSK;    /* 17:00-19:59 */
    return PERIOD_NIGHT;                  /* 20:00-04:59 */
}

/* ---------------------------------------------------------------- 月相 */
/*
 * ★ 月龄算法（纯计算，不依赖网络）：
 *   以已知新月时刻 JD 2451550.1（2000-01-06 18:14 UTC）为基准，
 *   朔望月平均长度 29.530588853 天。
 *     age_days = fmod(now - NEW_MOON_EPOCH, SYNODIC)
 *     age_frac = age_days / SYNODIC          → 0..1
 *     phase    = round(age_frac * 8) % 8     → 0..7
 *   实测（2026-09-16）：age_frac ≈ 0.33 → phase 3（盈凸）左右。
 *   ⚠️ 简单平均周期会累积误差（每年约 ±0.25 天），但月相是 8 档、
 *      每档跨 3.7 天，误差远小于档宽 → 肉眼完全看不出，够用。
 */
#define SYNODIC_DAYS   29.530588853
#define NEW_MOON_EPOCH 947182440.0      /* 2000-01-06 18:14 UTC 的 epoch 秒 */

int ws_moon_phase(time_t utc, int *age_pct_out)
{
    double days = (double)utc - NEW_MOON_EPOCH;
    double syn = SYNODIC_DAYS * 86400.0;
    double age = fmod(days, syn);
    if (age < 0) age += syn;
    double frac = age / syn;                 /* 0 = 新月，0.5 = 满月 */
    if (age_pct_out) *age_pct_out = (int)(frac * 100.0 + 0.5);
    int ph = (int)floor(frac * N_PHASE + 0.5) % N_PHASE;
    return ph;
}

/* ---------------------------------------------------------------- 天气 */

/* WMO 天气码 → 本项目 6 档（见 net_sync.h 的 WX_*）
 *   0 晴           → 晴
 *   1/2/3 多云      → 晴（1-bit 上多云与晴难分，归晴）
 *   45/48 雾        → 雾
 *   51-57 毛毛雨    → 雨小
 *   61/63 小雨中雨  → 雨小 / 雨中
 *   65 大雨         → 雨大
 *   66/67 冻雨      → 雨中
 *   71/73/75 雪     → 雪
 *   77 雪粒         → 雪
 *   80/81 阵雨      → 雨中
 *   82 强阵雨       → 雨大
 *   85/86 阵雪      → 雪
 *   ★ 09-28：95/96/99 雷雨 从「雨大」改映射「雷」（WX_THUNDER，惊吓姿态仅此天气触发）
 */
static int wmo_to_weather(int code)
{
    if (code == 0 || code == 1 || code == 2 || code == 3) return WX_CLEAR;
    if (code == 45 || code == 48) return WX_FOG;
    if (code >= 51 && code <= 57) return WX_RAIN_L;
    if (code == 61) return WX_RAIN_L;
    if (code == 63) return WX_RAIN_M;
    if (code == 65) return WX_RAIN_H;
    if (code == 66 || code == 67) return WX_RAIN_M;
    if (code >= 71 && code <= 77) return WX_SNOW;
    if (code == 80) return WX_RAIN_M;
    if (code == 81) return WX_RAIN_M;
    if (code == 82) return WX_RAIN_H;
    if (code == 85 || code == 86) return WX_SNOW;
    if (code >= 95) return WX_THUNDER;   /* 95/96/99 雷雨 → 雷 */
    return WX_CLEAR;
}

typedef struct { char buf[1024]; int len; } http_acc_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        http_acc_t *a = (http_acc_t *)e->user_data;
        if (a && e->data_len >= 0 && e->data_len < (int)sizeof(a->buf) - 1 - a->len) {
            memcpy(a->buf + a->len, e->data, e->data_len);
            a->len += e->data_len;
            a->buf[a->len] = 0;
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------- 城市名 → 经纬度（地理编码） */
/*
 * ★ 2026-09-19 用户订正：配网里的「城市」是**使用者居住的城市，精确到区**
 *   （「上海市浦东新区」这种），所以不能再靠一张写死的城市表。
 *
 * 用 Open-Meteo 的免费 Geocoding 接口（免 key）：
 *   http://geocoding-api.open-meteo.com/v1/search
 *       ?name=<城市>&count=1&language=zh&format=json
 *   返回 results[0].latitude / longitude。
 *
 * ★ 时区**不**从这里取：Geocoding 只给时区**名字**（"Asia/Shanghai"），还得维护
 *   名字→偏移的表；天气接口带 `timezone=auto` 会直接回整数 `utc_offset_seconds`，
 *   白拿。所以时区在 fetch_weather() 里校准。
 *
 * 「精确到区」的输入接口不一定一次就认，所以按「由具体到笼统」给几个候选逐个试，
 * 第一个有结果的就用：
 *     ① 原样             上海市浦东新区
 *     ② 最后一个「市」之后   浦东新区
 *     ③ ② 再去掉尾部区/县    浦东
 * 全失败 → 回落到省会级 CITY 表 → 再不行就保留原坐标并打警告（**永不空屏**）。
 */

static void url_enc(const char *s, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15];
        }
    }
    out[o] = 0;
}

/* 走一个 UTF-8 字符（中文字 3 字节） */
static const char *u8_next(const char *s)
{
    if (!s || !*s) return s;
    unsigned char c = (unsigned char)*s;
    int n = (c < 0x80) ? 1 : (((c >> 5) == 0x6) ? 2 : (((c >> 4) == 0xE) ? 3 : 4));
    return s + n;
}

/* 一次地理编码请求。成功返回 true 并回填 lat/lon。 */
static bool geocode_once(const char *name, float *lat, float *lon)
{
    char enc[256];
    url_enc(name, enc, sizeof(enc));
    /* ★ 缓冲要算够：enc 最坏情况是每个字节都被转义成 %XX（255→ 最大 ~252 字节），
       再加前缀 51 字节与后缀 32 字节 ≈ 336。给 512 免得 -Werror=format-truncation 拦下来。 */
    char url[512];
    snprintf(url, sizeof(url),
             "http://geocoding-api.open-meteo.com/v1/search"
             "?name=%s&count=1&language=zh&format=json", enc);

    /* ★★ 2026-09-21：acc 不再放栈上 —— 它 1KB，叠在 url[512]+enc[256] 之上，
       再进 esp_http_client_perform() 就把「task main」（4KB）爆了（实测 panic：
       "A stack overflow in task main has been detected"）。
       本函数不重入（只由主任务顺序调用），static 足够；每次进来手动清 len。 */
    static EXT_RAM_BSS_ATTR http_acc_t acc;
    acc.len = 0;
    esp_http_client_config_t hc = {
        .url = url,
        .event_handler = http_evt,
        .user_data = &acc,
        .timeout_ms = 10000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) return false;
    esp_err_t err = esp_http_client_perform(cli);
    int code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK || code != 200 || acc.len == 0) {
        ESP_LOGW(TAG, "地理编码「%s」没结果：err=%d http=%d", name, (int)err, code);
        return false;
    }
    cJSON *root = cJSON_Parse(acc.buf);
    if (!root) return false;
    cJSON *res = cJSON_GetObjectItem(root, "results");
    cJSON *r0  = res ? cJSON_GetArrayItem(res, 0) : NULL;
    bool ok = false;
    if (r0) {
        cJSON *la = cJSON_GetObjectItem(r0, "latitude");
        cJSON *lo = cJSON_GetObjectItem(r0, "longitude");
        cJSON *nm = cJSON_GetObjectItem(r0, "name");
        if (cJSON_IsNumber(la) && cJSON_IsNumber(lo)) {
            *lat = (float)la->valuedouble;
            *lon = (float)lo->valuedouble;
            ok = true;
            ESP_LOGI(TAG, "★ 地理编码：「%s」→「%s」 (%.4f, %.4f)",
                     name, cJSON_IsString(nm) ? nm->valuestring : "?", *lat, *lon);
            fflush(stdout);
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ---- 2026-10-14 海外城市查找（world_cities_data.h 生成表；调用极少，线性扫即可）---- */
const wc_entry_t *wc_find(const char *key)
{
    if (!key) return NULL;
    for (int i = 0; i < N_WORLD_CITIES; ++i)
        if (strcmp(key, WORLD_CITIES[i].key) == 0)
            return &WORLD_CITIES[i];
    return NULL;
}

/* ★R158：按中文名找海外城市（出图提示词要把「东京」变成 "Tokyo"） */
static const wc_entry_t *wc_find_by_zh(const char *zh)
{
    if (!zh || !zh[0]) return NULL;
    for (int i = 0; i < N_WORLD_CITIES; ++i)
        if (strcmp(zh, WORLD_CITIES[i].zh) == 0 || strcmp(zh, WORLD_CITIES[i].zht) == 0)
            return &WORLD_CITIES[i];
    return NULL;
}

/* ── ★R158：中国城市 → 英文出图关键词 ──────────────────────────────────────
   只列**有辨识度**的城市与地貌；没列到的走 "China, quiet landscape"。
   实测依据：flux 对 "Shanghai"/"Guilin" 这类英文地名反应极准（外滩天际线、
   漓江峰林都一次到位），对中文地名基本无视 —— 所以这张表是必须的。
   匹配用子串双向，因为配网存的是「河北省石家庄市」这种带省前缀的全名。 */
/* ★R168：加 region 字段，供「明信片辐射到周边城市」用（见 ai_visit_city）。
   区域：0 华东 / 1 华北 / 2 东北 / 3 华中 / 4 华南 / 5 西南 / 6 西北 / 7 远行目的地（不参与辐射）。
   表本身按地理顺序排列，同区域条目天然相邻。 */
/* ★R168：zh/en + 近似经纬度（度×100 的整数，省 flash）+ 区域。
   坐标只用来**排序远近**（决定明信片辐射到哪），不做导航，误差几十公里无影响。
   reg 7 = 远行目的地，不参与辐射。 */
typedef struct { const char *zh; const char *en; int16_t lat; int16_t lon; uint8_t reg; } city_en_t;
static const city_en_t CITY_EN[] = {
    { "上海", "Shanghai Bund skyline, Oriental Pearl Tower, Huangpu river", 3120, 12150, 0 },
    { "北京", "Beijing Forbidden City, Great Wall, old hutong alleys", 3990, 11640, 1 },
    { "天津", "Tianjin Hai river, old European style buildings", 3910, 11720, 1 },
    { "重庆", "Chongqing mountain city, Yangtze river, stacked houses", 2960, 10650, 5 },
    { "哈尔滨", "Harbin snow and ice, Russian style architecture", 4580, 12660, 2 },
    { "沈阳", "Shenyang imperial palace, northern city", 4180, 12340, 2 },
    { "长春", "Changchun northern city, wide streets", 4390, 12530, 2 },
    { "大连", "Dalian coastal cliffs, sea, red roofed houses", 3890, 12160, 2 },
    { "石家庄", "north China plain, endless wheat fields, poplar trees", 3800, 11450, 1 },
    { "太原", "Taiyuan Fen river, loess plateau hills", 3790, 11250, 1 },
    { "呼和浩特", "Hohhot vast grassland, Mongolian steppe, blue sky", 4080, 11170, 1 },
    { "济南", "Jinan springs, Daming lake, willow trees", 3670, 11700, 0 },
    { "青岛", "Qingdao seaside, red roofs, pier, blue sea", 3610, 12040, 0 },
    { "烟台", "Yantai coast, fishing boats, sea cliffs", 3750, 12140, 0 },
    { "郑州", "Zhengzhou Yellow river, flat plain", 3470, 11360, 1 },
    { "洛阳", "Luoyang Longmen grottoes, peony flowers", 3460, 11240, 1 },
    { "开封", "Kaifeng ancient capital, old city wall", 3480, 11430, 1 },
    { "西安", "Xi'an ancient city wall, bell tower, Tang dynasty", 3430, 10890, 6 },
    { "兰州", "Lanzhou Yellow river, desert edge, mountains", 3610, 10380, 6 },
    { "西宁", "Xining Qinghai plateau, distant snow mountains", 3660, 10180, 6 },
    { "银川", "Yinchuan desert oasis, Helan mountain", 3850, 10620, 6 },
    { "乌鲁木齐", "Urumqi Tianshan snow mountains, vast dry land", 4380, 8760, 6 },
    { "喀什", "Kashgar old town, bazaar, central Asia", 3950, 7600, 6 },
    { "吐鲁番", "Turpan desert, grape vines, dry red earth", 4290, 8920, 6 },
    { "敦煌", "Dunhuang desert dunes, Mogao caves, camel", 4010, 9470, 6 },
    { "拉萨", "Lhasa Potala Palace, Tibetan plateau, prayer flags", 2970, 9110, 6 },
    { "成都", "Chengdu teahouse, bamboo forest, panda", 3060, 10410, 5 },
    { "昆明", "Kunming spring city, flowers, Dian lake", 2500, 10270, 5 },
    { "丽江", "Lijiang old town, canals, Jade Dragon snow mountain", 2690, 10020, 5 },
    { "大理", "Dali Erhai lake, Cangshan mountain, white houses", 2560, 10020, 5 },
    { "香格里拉", "Shangri-La Tibetan meadow, monastery, snow peaks", 2780, 9970, 5 },
    { "西双版纳", "Xishuangbanna tropical rainforest, palm trees", 2200, 10080, 5 },
    { "贵阳", "Guiyang karst hills, green mountains", 2660, 10660, 5 },
    { "桂林", "Guilin karst mountains, Li river, bamboo raft", 2530, 11030, 5 },
    { "南宁", "Nanning green tropical city, palm trees", 2280, 10840, 4 },
    { "海口", "Haikou seaside, coconut trees, tropical", 2000, 11030, 4 },
    { "三亚", "Sanya tropical beach, palm trees, turquoise sea", 1830, 10950, 4 },
    { "广州", "Guangzhou Canton Tower, Pearl river, banyan trees", 2310, 11330, 4 },
    { "深圳", "Shenzhen modern skyline, sea, skyscrapers", 2250, 11410, 4 },
    { "珠海", "Zhuhai coastal road, sea, islands", 2230, 11360, 4 },
    { "厦门", "Xiamen island, Gulangyu, sea, red roofs", 2450, 11810, 0 },
    { "福州", "Fuzhou Min river, banyan trees, old lanes", 2610, 11930, 0 },
    { "泉州", "Quanzhou old port, temples, stone streets", 2490, 11860, 0 },
    { "南昌", "Nanchang Tengwang pavilion, Gan river", 2870, 11590, 0 },
    { "长沙", "Changsha Orange isle, Xiang river, hills", 2820, 11300, 3 },
    { "武汉", "Wuhan Yangtze river bridge, Yellow Crane tower", 3060, 11430, 3 },
    { "合肥", "Hefei city, Chaohu lake, flat land", 3180, 11730, 0 },
    { "南京", "Nanjing city wall, Qinhuai river, plane trees", 3210, 11880, 0 },
    { "苏州", "Suzhou classical garden, water town, white walls black tiles", 3130, 12060, 0 },
    { "无锡", "Wuxi Taihu lake, gardens, misty water", 3160, 12030, 0 },
    { "扬州", "Yangzhou Slender West lake, old town, willows", 3240, 11940, 0 },
    { "杭州", "Hangzhou West lake, pagoda, willow trees, mist", 3030, 12020, 0 },
    { "宁波", "Ningbo port, coastal hills, sea", 2990, 12150, 0 },
    { "温州", "Wenzhou river, green hills, coastal", 2800, 12070, 0 },
    { "绍兴", "Shaoxing water town, stone bridges, wine jars", 3000, 12060, 0 },
    { "嘉兴", "Jiaxing water town, canals, old houses", 3080, 12080, 0 },
    { "舟山", "Zhoushan islands, fishing boats, sea", 3000, 12220, 0 },
    { "黄山", "Huangshan yellow mountain, pine trees, sea of clouds", 2970, 11830, 0 },
    { "张家界", "Zhangjiajie sandstone pillars, misty peaks", 2910, 11050, 3 },
    { "九寨沟", "Jiuzhaigou colorful lakes, waterfall, forest", 3330, 10390, 5 },
    { "峨眉山", "Mount Emei temple, cloud sea, green peaks", 2960, 10350, 5 },
    { "承德", "Chengde mountain resort, temples, pine hills", 4090, 11790, 1 },
    { "秦皇岛", "Qinhuangdao coast, Great Wall meets the sea", 3990, 11960, 1 },
    { "呼伦贝尔", "Hulunbuir grassland, horses, vast sky", 4920, 11980, 1 },
    /* ── 远行的 10 个目的地（EXP_TBL）：是地区不是城市，单独给地貌关键词 ── */
    { "冰岛", "Iceland glacier lagoon, black sand, snow field, aurora", 6500, -1800, 7 },
    { "京都", "Kyoto old temple, torii gate, bamboo grove, autumn maple", 3500, 13580, 7 },
    { "撒哈拉", "Sahara desert dunes, endless sand, camel caravan", 2300, 1300, 7 },
    { "威尼斯", "Venice canal, gondola, old stone bridge, water reflection", 4540, 1230, 7 },
    { "土耳其", "Cappadocia fairy chimneys, hot air balloons, dry valley", 3890, 3520, 7 },
    { "秘鲁", "Machu Picchu, Andean stone terraces, alpaca, cloud", -1350, -7200, 7 },
    { "埃及", "Egypt pyramids, Nile river, desert horizon", 2680, 3080, 7 },
    { "摩洛哥", "Morocco blue painted town, kasbah, spice market", 3180, -710, 7 },
    { "新西兰", "New Zealand green hills, fjord, sheep, snow mountain", -4100, 17400, 7 },
    { "印度", "India Taj Mahal, Ganges river, ancient temple", 2700, 7800, 7 },
};
#define N_CITY_EN ((int)(sizeof(CITY_EN) / sizeof(CITY_EN[0])))

void city_place_en(const char *zh, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!zh || !zh[0]) { snprintf(out, cap, "China, quiet landscape"); return; }
    for (int i = 0; i < N_CITY_EN; ++i) {
        if (strstr(zh, CITY_EN[i].zh) || strstr(CITY_EN[i].zh, zh)) {
            snprintf(out, cap, "%s", CITY_EN[i].en);
            return;
        }
    }
    const wc_entry_t *w = wc_find_by_zh(zh);
    if (w && w->en && w->en[0]) { snprintf(out, cap, "%s", w->en); return; }
    snprintf(out, cap, "China, quiet landscape");
}
/* ★R169：日常通用场景池 —— 用户报「你这边所有生成跟上海生成的全是东方明珠，
   你要生成多少东方明珠」。
   ⇒ 明信片不能张张是地标：多数应该是**生活里的普通一角**（巷口/早点摊/阳台/小卖部），
     这种画面本来就看不出城市，地点行就老老实实标**用户所在城市**，
     不硬塞一个它根本画不出来的地标。地标片只占少数（见 loot_system.c 的 35%）。 */
static const char *const SC_GROUND[] = {
    "a narrow alley", "a riverside path", "a stone bridge", "an old residential compound",
    "a small vegetable market", "a rooftop", "a bus stop", "a stairwell",
    "a small courtyard", "a noodle shop", "a fish market", "a school gate",
    "a parking lot behind a building", "an edge of a rice field", "a temple yard",
    "a rooftop laundry area",
};
static const char *const SC_THING[] = {
    "a rusted bicycle", "a wooden stool", "a row of flower pots", "a red plastic basin",
    "a bamboo steamer", "a milk crate", "a coiled garden hose", "a bare light bulb",
    "a folding table", "a green rubbish bin", "a bird cage", "a sack of rice",
    "a hand cart", "a pile of bricks", "an old radio", "a tea kettle",
    "a pair of rubber boots", "a bundle of firewood",
};
static const char *const SC_LIGHT[] = {
    "low evening sun", "hazy morning light", "flat overcast light", "hard noon shadow",
    "blue hour dusk", "one warm street lamp", "light through wet leaves", "soft window light",
};
static const char *const SC_DETAIL[] = {
    "long shadows", "thin mist", "wet ground reflections", "dust in a sunbeam",
    "laundry moving in the wind", "steam from a vent", "peeling paint",
    "moss on the step", "rain dripping from the eaves", "a puddle with the sky in it",
};
static const char *const SC_FRAME[] = {
    "seen from a low angle", "wide quiet composition", "close to the ground",
    "one third sky", "seen through a doorway", "from across the street",
};

#define SC_N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define SC_PICK(a) ((a)[(int)(esp_random() % (uint32_t)SC_N(a))])

/* ★R170：**兜底画面用组合式生成，不再从 22 条清单里挑**。
   用户口径（2026-11-10）：「22 个日常场景不还是全部固定吗？我玩 100 年也会重样」。
   组合数 = 16 场所 × 18 物件 × 8 光线 × 10 细节 × 6 取景 = **138240 种**，
   按一天一张算够 300 多年不重样；而这只是**模型不通时的兜底**，
   通了就是模型现编（理论无穷）。拼出来的仍是纯英文 prompt，flux 直吃。 */
void ai_daily_scene_build(char *out, int cap)
{
    if (!out || cap <= 0) return;
    snprintf(out, (size_t)cap, "%s with %s, %s, %s, %s",
             SC_PICK(SC_GROUND), SC_PICK(SC_THING), SC_PICK(SC_LIGHT),
             SC_PICK(SC_DETAIL), SC_PICK(SC_FRAME));
}

/* ★R168：这一次「去哪」——以用户城市为中心向外辐射。
   用户口径（2026-11-10）：「推的其实也跟用户所选的城市比较相关，但不会所有的都是
   用户所在城市，也是可以辐射出去的」。
   做法：
     · 以 local_pct 的概率留在 home（地标片传 50：一半本地、一半外地）；
     · 其余按**经纬度距离分档**抽别的城市（上海 ↔ 嘉兴/苏州/杭州…），
       近城权重高、远城权重低但非零 —— 像「从家往外辐射」，不是乱跳；
     · 海外城市（不在表里）与远行目的地（reg=7）不参与辐射，原样返回；
     · 输出**中文短名**（"杭州"），调用方直接喂 city_place_en() 与 ai_place_line()，
       保证「图 / 地点行 / 配文」说的是同一座城，不会图是杭州、字写上海。 */
void ai_visit_city(const char *home, char *out, int cap, int local_pct)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!home || !home[0]) { snprintf(out, cap, "%s", home ? home : ""); return; }

    int hi = -1, reg = -1;
    for (int i = 0; i < N_CITY_EN; ++i) {
        if (strstr(home, CITY_EN[i].zh) || strstr(CITY_EN[i].zh, home)) {
            hi = i; reg = CITY_EN[i].reg; break;
        }
    }
    if (local_pct < 0) local_pct = 68;
    if (local_pct > 100) local_pct = 100;
    if (hi < 0 || reg == 7 || (int)(esp_random() % 100u) < local_pct) {
        snprintf(out, cap, "%s", home);                 /* 留在本地 */
        return;
    }

    /* 按**经纬度距离分档**加权（不是按表内序号 —— 那会让上海优先抽到济南/青岛，
       因为它们恰好排在华东段的开头，实测偏差很明显）。
       用户口径（2026-11-10）：「远的城市稍微概率小一点，但以近的城市为主，远的也可以」
       ⇒ 不用平方反比（远处会低到几乎抽不到），改成显式档位：
           <220km 1000 / 220-550km 300 / 550-1100km 110 / 1100-2200km 45 / 更远 18
       这样近城仍是绝对主力，但偶尔能抽到西安、成都、厦门这种「跑远了」的片。 */
    const int hlat = CITY_EN[hi].lat, hlon = CITY_EN[hi].lon;
    int idx[N_CITY_EN], nc = 0;
    long wt[N_CITY_EN], tot = 0;
    for (int i = 0; i < N_CITY_EN; ++i) {
        if (i == hi || CITY_EN[i].reg == 7) continue;
        const long dla = (long)CITY_EN[i].lat - hlat;
        const long dlo = (long)CITY_EN[i].lon - hlon;
        const long d2 = dla * dla + dlo * dlo;          /* 单位 = 0.0001 度^2 */
        long w;
        if      (d2 <  40000L) w = 1000;                /* < 2 度    ≈ 220km（沪杭/沪苏同档） */
        else if (d2 < 250000L) w =  300;                /* < 5 度    ≈ 550km */
        else if (d2 < 1000000L) w = 110;                /* < 10 度   ≈ 1100km */
        else if (d2 < 4000000L) w =  45;                /* < 20 度   ≈ 2200km */
        else                    w =  18;                /* 更远：偶尔来一张 */
        idx[nc] = i; wt[nc] = w; tot += w; ++nc;
    }
    if (nc == 0) { snprintf(out, cap, "%s", home); return; }

    long r = (long)(esp_random() % (uint32_t)tot);
    for (int k = 0; k < nc; ++k) {
        r -= wt[k];
        if (r < 0) { snprintf(out, cap, "%s", CITY_EN[idx[k]].zh); return; }
    }
    snprintf(out, cap, "%s", CITY_EN[idx[nc - 1]].zh);
}

bool ws_resolve_city(ws_config_t *cfg)
{
    if (!cfg->city[0]) return false;

    /* 同一个城市已经解析过 → 不重复发请求（省时省流量，也不给接口添麻烦） */
    char geo_name[64] = {0};
    nvs_handle_t h;
    bool have_h = (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK);
    if (have_h) {
        size_t cap = sizeof(geo_name);
        nvs_get_str(h, "geo_city", geo_name, &cap);
        nvs_close(h);
    }
    if (strcmp(geo_name, cfg->city) == 0 && cfg->lat != 0.0f) {
        ESP_LOGI(TAG, "城市未变（%s）→ 沿用已解析坐标 (%.4f, %.4f)",
                 cfg->city, cfg->lat, cfg->lon);
        return true;
    }

    /* ★★ 2026-10-14 海外城市：CC:City 键先查 world_cities（GeoNames 权威坐标）。
       命中即用；未命中（中国城 = 中文名键）落到下面的 CITY_COORDS 旧路 —— 中国零回归。 */
    {
        const wc_entry_t *w = wc_find(cfg->city);
        if (w) {
            cfg->lat = w->lat;
            cfg->lon = w->lon;
            ESP_LOGI(TAG, "★ 海外城市查表命中：「%s」→ (%.4f, %.4f) [%s]",
                     cfg->city, cfg->lat, cfg->lon, w->cc);
            fflush(stdout);
            goto geo_done;
        }
    }

    /* ★★ 2026-09-19：居住城市改成「省→市」两级下拉后，候选项是**封闭且已知**的
       （下拉里那 300 多个市），全部在电脑上离线解析好了（city_coords.h）。
       所以主路径改成**直接查表**，不再依赖 Open-Meteo 地理编码 ——
       实测它根本不认中文城市名（上海 / 浦东新区 / 广州 / 天河区 … 全返回 null），
       而设备上没有 pypinyin，中文→拼音这条补救路走不通。
       下面那段旧的地理编码链保留着，只作为「表里没有」时的兜底。 */
    for (size_t i = 0; i < N_CITY_COORDS; ++i) {
        if (strcmp(cfg->city, CITY_COORDS[i].key) == 0) {
            cfg->lat = CITY_COORDS[i].lat;
            cfg->lon = CITY_COORDS[i].lon;
            ESP_LOGI(TAG, "★ 城市查表命中：「%s」→ (%.4f, %.4f)",
                     cfg->city, cfg->lat, cfg->lon);
            fflush(stdout);
            goto geo_done;
        }
    }
    ESP_LOGW(TAG, "城市「%s」不在下拉表（%d 条）里 → 退回旧的地理编码路径",
             cfg->city, (int)N_CITY_COORDS);

    /* 生成候选 */
    char cand[3][64];
    int  nc = 0;
    snprintf(cand[nc++], sizeof(cand[0]), "%s", cfg->city);

    const char *tail = NULL;
    for (const char *p = cfg->city; *p; ) {
        if (strncmp(p, "市", 3) == 0) tail = p + 3;      /* 记住最后一个「市」之后 */
        p = u8_next(p);
    }
    if (tail && *tail) {
        snprintf(cand[nc++], sizeof(cand[0]), "%s", tail);
        char t2[64];
        snprintf(t2, sizeof(t2), "%s", tail);
        size_t L = strlen(t2);
        if (L > 3 && (strcmp(t2 + L - 3, "区") == 0 || strcmp(t2 + L - 3, "县") == 0)) {
            t2[L - 3] = 0;                                /* 去掉尾部「区/县」再试一次 */
            if (t2[0]) snprintf(cand[nc++], sizeof(cand[0]), "%s", t2);
        }
    }

    float la = 0.0f, lo = 0.0f;
    for (int i = 0; i < nc; ++i) {
        if (geocode_once(cand[i], &la, &lo)) {
            cfg->lat = la;
            cfg->lon = lo;
            goto geo_done;
        }
    }
    /* 地理编码全失败 → 省会级兜底 */
    for (size_t i = 0; i < N_CITY; ++i) {
        if (strstr(cfg->city, CITIES[i].name)) {
            cfg->lat = CITIES[i].lat;
            cfg->lon = CITIES[i].lon;
            ESP_LOGW(TAG, "地理编码全失败 → 用「%s」的近似坐标 (%.2f, %.2f)",
                     CITIES[i].name, cfg->lat, cfg->lon);
            goto geo_done;
        }
    }
    ESP_LOGW(TAG, "地理编码全失败且无近似可兜 → 保留原坐标 (%.4f, %.4f)",
             cfg->lat, cfg->lon);
    return false;

geo_done:
    /* 落 NVS：下次同城不再请求；同时把 lat/lon 持久化（掉电也不丢） */
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "geo_city", cfg->city);
        nvs_set_i32(h, "lat", (int32_t)(cfg->lat * 10000.0f));
        nvs_set_i32(h, "lon", (int32_t)(cfg->lon * 10000.0f));
        nvs_commit(h);
        nvs_close(h);
    }
    return true;
}

/* 返回 true = 取到天气。
 * ★ 顺带干一件事：`timezone=auto` 会让接口回一个 `utc_offset_seconds`，
 *   于是**时区不用再维护「时区名 → 偏移」的表**，也不用手写城市→时区的映射。
 *   所以 cfg 由 const 改成可写 —— 校准时区后直接落 NVS。 */
static bool fetch_weather(ws_config_t *cfg, int *weather_out, int *temp_out)
{
    char url[288];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f&current=weather_code,temperature_2m"
             "&timezone=auto",
             cfg->lat, cfg->lon);

    /* ★★ 2026-09-21：同上 —— 1KB 缓冲从栈上挪到 static，避免「task main」栈溢出。 */
    static EXT_RAM_BSS_ATTR http_acc_t acc;
    acc.len = 0;
    esp_http_client_config_t hc = {
        .url = url,
        .event_handler = http_evt,
        .user_data = &acc,
        .timeout_ms = 8000,              /* ★R156：12s→8s，弱网别为天气干等 */
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) return false;
    esp_err_t err = esp_http_client_perform(cli);
    int code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || code != 200 || acc.len == 0) {
        ESP_LOGW(TAG, "取天气失败：err=%d http=%d len=%d", (int)err, code, acc.len);
        fflush(stdout);
        return false;
    }
    cJSON *root = cJSON_Parse(acc.buf);
    if (!root) {
        ESP_LOGW(TAG, "天气 JSON 解析失败");
        return false;
    }
    bool ok = false;
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *wc = cJSON_GetObjectItem(cur, "weather_code");
        cJSON *tp = cJSON_GetObjectItem(cur, "temperature_2m");
        if (cJSON_IsNumber(wc)) {
            *weather_out = wmo_to_weather((int)wc->valuedouble);
            ok = true;
        }
        if (cJSON_IsNumber(tp)) *temp_out = (int)lround(tp->valuedouble);
    }
    /* ★ 时区校准：跟经纬度天然一致，比名字表可靠 */
    {
        cJSON *off = cJSON_GetObjectItem(root, "utc_offset_seconds");
        if (cJSON_IsNumber(off)) {
            int tz = (int)off->valuedouble;
            if (tz != cfg->tz_offset_sec) {
                ESP_LOGW(TAG, "★ 时区校准：%+d → %+d 小时（来自 utc_offset_seconds）",
                         cfg->tz_offset_sec / 3600, tz / 3600);
                cfg->tz_offset_sec = tz;
                ws_save_config(cfg, true);   /* ★2026-10-10：真值来自 timezone=auto → 置 tzok */
            }
        }
    }
    cJSON_Delete(root);
    if (ok) {
        ESP_LOGI(TAG, "天气：WMO→档 %d，气温 %d℃", *weather_out, *temp_out);
        fflush(stdout);
    }
    return ok;
}

/* ═══ ★G 远行 API（2026-10-16）：OpenAI 兼容 chat/completions 生成「猫的远行日记」 ═══
 * main.c 的 api_live_fill() 调用：把生成的一段话填进「活」明信片（专用槽 API_POST_IDX）。
 * · 端点/模型 = OpenAI 兼容（缺省宏见下）；NVS petns:api_url / petns:api_model 可覆盖，
 *   Key = petns:api_key（配网页第 5 项写入）。没 Key 直接 false（调用方有预制片兜底）。
 * · 跑在**调用方上下文**（主循环测试按钮 / 同步任务每日发信），可阻塞 —— 超时 20s（★R157 免费端点实测）；
 *   大缓冲一律 static（同 geocode_once/fetch_weather 的 acc 口径，防 task main 栈爆）。
 * · HTTPS 证书束校验（CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y）。
 * · 输出清洗：换行/Tab → 空格（项目口径「空格断句」，屏上无标点字形）；按 UTF-8 字符
 *   步进截断，绝不切出半个字。 */
#define API_URL_DEF    "https://api.openai.com/v1/chat/completions"
#define API_MODEL_DEF  "gpt-4o-mini"
/* ★R157：免费端点实测 16.4s（Pollinations 匿名层慢），原 10s 必定超时判失败。
   ★R164：收到 12s + 失败立刻重试一次（见 api_card_send）—— r163 抓到「握手成功证书
   通过、之后一个字节都不回」的排队态，20s×1 次等于白等；12s×2 次最坏 24s，
   命中一次就出真文案，两次都不行才落到本机随机兜底池（不再是同一句模板）。 */
#define API_TIMEOUT_MS 12000

/* ★R157：响应缓冲 2KB→4KB。免费端点实测返回的 message 里带 reasoning 字段，
   完整 JSON 达 2919 B；2KB 会被 api_evt 静默截断 ⇒ cJSON_Parse 失败 ⇒ 整轮判失败。
   —— static + 本函数非重入（与 acc 同口径）；api_acc_t 与 http_acc_t 同布局但更大，
   事件回调要用它自己的宽度收数据（http_evt 里 sizeof 是 http_acc_t 的 1024）。 */
typedef struct { char buf[4096]; int len; } api_acc_t;

static esp_err_t api_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        api_acc_t *a = (api_acc_t *)e->user_data;
        if (a && e->data_len >= 0 && e->data_len < (int)sizeof(a->buf) - 1 - a->len) {
            memcpy(a->buf + a->len, e->data, e->data_len);
            a->len += e->data_len;
            a->buf[a->len] = 0;
        }
    }
    return ESP_OK;
}

/* ★R157 失败熔断（文字通道）：连续 3 次生成失败 → 本次开机不再试，直接回落预制片。
   普通 static（非 RTC 域）⇒ 开机/深睡唤醒自动清零，符合用户「重新开关机后按原样来」。
   图片通道在 api_image.c 里各有一份，互不拖累（一个通一个不通时不会互相停掉）。 */
#define API_FUSE_MAX 3
static int s_api_fail_txt = 0;
static bool api_txt_fail(void)
{
    if (s_api_fail_txt < API_FUSE_MAX) ++s_api_fail_txt;
    ESP_LOGW(TAG, "★G API 文字通道失败计数 %d/%d", s_api_fail_txt, API_FUSE_MAX);
    return false;
}

/* ── 明信片文案：一次 chat/completions → 返回「标题 空格 正文」单串 ──────────
   ★R159：原 ws_api_diary 把「读 NVS + 拼 body + HTTP + 解析」全揉在一个函数里，只有
   远行按天寄信 / 日常**纯文字活卡**能用。AI **图片**卡想要同一套文案就只能再抄一遍；
   实际它连抄都没抄 —— 文案写死成「这是我在XX附近走走」，于是每张图卡文字一模一样
   （用户实机报「两张片子的文字都是这句，我明明接了文字通道」）。
   这里把发送层抽出来共用，提示词交给调用方自己组装：
     · ws_api_diary()  远行按天寄信 / 日常纯文字活卡（行为与原版逐字一致）
     · R170 起：AI 图片卡的画面+配文改走 ws_api_scene()（见下方），不再单发配文
   ⚠ 非重入：acc/key/url/body 都是 static，同一时刻只有一条链路在用。 */
/* ★R170：加 max_tokens / timeout_ms / attempts 三个参数
   —— 三行输出（画面+标题+正文）120 token 不够，会截断在正文处（传 260）；
   —— 现编画面这一跳在**真机实测**（pollinations 免费档，本机同一出口 IP）要 18.5s / 31s / 38.5s，
      而日记/配文那一跳的 12s 超时会把它一刀切死 ⇒ 单独放宽到 28s、只试一次
      （再乘 2 次就是 56s，比图还慢，用户要等死）；
   —— 远行日记仍是 12s × 2 次，行为逐字不变。 */
static bool api_card_send(const char *sys, const char *usr, char *out, int cap,
                          int max_tokens, int timeout_ms, int attempts)
{
    if (timeout_ms <= 0) timeout_ms = API_TIMEOUT_MS;
    if (attempts <= 0) attempts = 1;
    if (out && cap > 0) out[0] = 0;
    if (s_api_fail_txt >= API_FUSE_MAX) {
        ESP_LOGW(TAG, "★G API 文字通道已熔断 → 本次开机不再试");
        return false;
    }
    static EXT_RAM_BSS_ATTR char key[128], url[192], model[48], auth[168], body[1536];  /* ★R170 全检：sys+usr 最坏约 1KB */
    key[0] = url[0] = model[0] = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l1 = sizeof(key), l2 = sizeof(url), l3 = sizeof(model);
        nvs_get_str(h, "api_key",   key,   &l1);       /* 缺键保持空串 */
        nvs_get_str(h, "api_url",   url,   &l2);
        nvs_get_str(h, "api_model", model, &l3);
        nvs_close(h);
    }
    if (!key[0]) { ESP_LOGW(TAG, "★G API：没配 Key → 不生成（调用方兜底预制片）"); return false; }
    if (!url[0])   snprintf(url,   sizeof(url),   "%s", API_URL_DEF);
    if (!model[0]) snprintf(model, sizeof(model), "%s", API_MODEL_DEF);

    /* JSON 体：model/提示词均为固件可控常量（无引号），dst 来自 EXP_TBL —— 无转义需求 */
    snprintf(body, sizeof(body),
             "{\"model\":\"%s\",\"messages\":["
             "{\"role\":\"system\",\"content\":\"%s\"},"
             "{\"role\":\"user\",\"content\":\"%s\"}],"
             "\"max_tokens\":%d,\"temperature\":1.0,\"seed\":%u}",
             model, sys, usr, (max_tokens > 0 ? max_tokens : 120),
             (unsigned)(esp_random() % 1000000u));   /* ★R163：随机 seed */

    /* 响应缓冲 4KB —— static api_acc_t（见上方说明），本函数非重入 */
    static EXT_RAM_BSS_ATTR api_acc_t acc;
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    /* ★R164：一次不通就**立刻再试一次**。r163 实证：pollinations text 排队时
       TLS 握手成功、证书校验通过，然后 20s 一个字节都不回（`err=28679 http=-1`
       "Connection timed out before data was ready"），一次就判死 → 明信片只能吃兜底模板。
       超时从 20s 收到 12s：两次最坏 24s（与旧版一次 20s 同量级），但多一次机会。 */
    esp_err_t err = ESP_FAIL;
    int code = 0;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        acc.len = 0;
        acc.buf[0] = 0;
        esp_http_client_config_t hc = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .event_handler = api_evt,
            .user_data = &acc,
            .timeout_ms = timeout_ms,
            .disable_auto_redirect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t cli = esp_http_client_init(&hc);
        if (!cli) { ESP_LOGW(TAG, "★G API：http client 初始化失败"); return api_txt_fail(); }
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_header(cli, "Authorization", auth);
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
        err = esp_http_client_perform(cli);
        code = esp_http_client_get_status_code(cli);
        esp_http_client_cleanup(cli);
        if (err == ESP_OK && code == 200 && acc.len > 0) break;
        if (code == 402 || code == 429) break;          /* 限流：重试也是同一个名额 */
        if (attempt + 1 < attempts) {
            ESP_LOGW(TAG, "★G API 文字通道没回数据（err=%d http=%d len=%d）→ 立刻重试",
                     (int)err, code, acc.len);
            fflush(stdout);
        }
    }
    if (err != ESP_OK || code != 200 || acc.len == 0) {
        /* ★R159：402/429 = 免费源限流（每 IP 30 秒一次），不是"配错了"。
           与熔断分开记：限流直接返回 false，不计连续失败，免得点两次就废一天。 */
        if (code == 402 || code == 429) {
            ESP_LOGW(TAG, "★G API 文字通道被限流 http=%d（不计熔断）", code);
            return false;
        }
        ESP_LOGW(TAG, "★G API 请求失败：err=%d http=%d len=%d（尝试 %d 次）",
                 (int)err, code, acc.len, attempts);
        fflush(stdout);
        return api_txt_fail();
    }
    cJSON *root = cJSON_Parse(acc.buf);
    if (!root) { ESP_LOGW(TAG, "★G API 响应不是 JSON"); return api_txt_fail(); }
    bool ok = false;
    cJSON *chs  = cJSON_GetObjectItem(root, "choices");
    cJSON *c0   = chs ? cJSON_GetArrayItem(chs, 0) : NULL;
    cJSON *msg  = c0 ? cJSON_GetObjectItem(c0, "message") : NULL;
    cJSON *ct   = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
    if (cJSON_IsString(ct) && ct->valuestring && ct->valuestring[0]) {
        int o = 0;
        const char *s = ct->valuestring;
        while (*s && o < cap - 4) {
            /* ★R170 全检：模型换行可能是 CRLF 或纯 CR。原来把 '\r' 一律变空格，
               三行格式会被拼成一行 → 解析失败白等 28 秒。CRLF 只保留 '\n'，
               纯 CR 当换行；Tab 仍是空格（日记口径不变）。 */
            if (*s == '\r') {
                if (s[1] == '\n') { ++s; continue; }
                out[o++] = '\n'; ++s; continue;
            }
            if (*s == '\t') { out[o++] = ' '; ++s; continue; }
            if (*s == '\n') { out[o++] = '\n'; ++s; continue; }   /* ★R163：保留硬换行 */
            const char *nx = u8_next(s);           /* 一次走一个 UTF-8 字符 → 不会切半字 */
            const int cl = (int)(nx - s);
            if (cl <= 0 || o + cl >= cap) break;
            memcpy(out + o, s, (size_t)cl);
            o += cl;
            s = nx;
        }
        out[o] = 0;
        ok = (o > 0);
        ESP_LOGI(TAG, "★G API 文案：%s", out);
        fflush(stdout);
    }
    cJSON_Delete(root);
    if (!ok) { ESP_LOGW(TAG, "★G API 响应里没有可用正文"); return api_txt_fail(); }
    s_api_fail_txt = 0;                  /* ★R157：成功即清零，熔断按「连续失败」计 */
    return true;
}

/* ★R163：每次换一个「注意点」。
   用户实机报「推的第二张，文案跟上一张一模一样」。真因是**请求完全一样**：
   城市 + 天气 + 固定系统提示词，模型（或被缓存）就回同一句。加一句随机侧重，
   请求变了，文案才会变 —— 只加 seed 不够稳，有些免费端点会忽略 seed。 */
static const char *walk_hint(void)
{
    static const char *const H_ZH[] = { "留意光线的角度", "留意听到的声音", "留意闻到的气味",
                                        "留意路边的小东西", "留意自己的影子", "留意风和温度" };
    static const char *const H_EN[] = { "Notice the light", "Notice the sounds", "Notice the smells",
                                        "Notice small things by the road", "Notice your own shadow",
                                        "Notice the wind" };
    static const char *const H_JA[] = { "光の角度", "聞こえた音", "におい", "道ばたの小さなもの",
                                        "自分の影", "風と温度" };
    static const char *const H_ZHT[] = { "留意光線的角度", "留意聽到的聲音", "留意聞到的氣味",
                                         "留意路邊的小東西", "留意自己的影子", "留意風和溫度" };
    const char *const *tab = H_ZH;
    if      (g_lang == LANG_EN)  tab = H_EN;
    else if (g_lang == LANG_JA)  tab = H_JA;
    else if (g_lang == LANG_ZHT) tab = H_ZHT;
    return tab[esp_random() % 6u];
}

/* 四语系统提示词：**远行日记**（ws_api_diary 专用）。
   ★R170：原来这里还有一整段 nearby=true 的「家门口散步」四语文案，只服务
     ws_api_nearby()；该函数在 R170 被 ws_api_scene 取代并删除后，这段成了
     零调用死码（R148 体检口径：死码留着会误导下一轮 Agent），整段删除。
   格式约束同口径：标题≤8字 + 1空格 + 正文≤30字一句，全程空格断句、零标点零 emoji
   —— 屏上字库没有标点字形。 */
static const char *card_sys(void)
{
    switch (g_lang) {
    case LANG_EN:
        return "You are a little cat traveling far away. Write a postcard to your beloved owner. "
               "Reply strictly in this format: a title of at most 8 words, then ONE space, "
               "then one sentence of at most 30 characters. Use spaces instead of any punctuation. "
               "No punctuation marks, no emoji. Sound like an affectionate cat missing its owner.";
    case LANG_JA:
        return "あなたは遠くへ旅している子猫 大好きな飼い主に絵手紙を書きます "
               "必ずこの形式で答えてください 8語以内のタイトル 空白を一つ その後に30文字以内の本文を一文だけ "
               "句読点や記号や絵文字は一切使わない 飼い主が恋しい甘えん坊な子猫の口調で";
    case LANG_ZHT:
        return "你是一隻出門遠行的小貓 給最愛的主人寫明信片 嚴格按這個格式回答 "
               "一個不超過8字的標題 然後一個空格 然後正文 正文不超過30字 只有一句 "
               "全程用空格代替所有標點 不許出現任何標點符號和表情 語氣像一隻想念主人的貓 親暱口語化";
    default:
        return "你是一只出门远行的小猫 给最爱的主人写明信片 严格按这个格式回答 "
               "一个不超过8字的标题 然后一个空格 然后正文 正文不超过30字 只有一句 "
               "全程用空格代替所有标点 不许出现任何标点符号和表情 语气像一只想念主人的猫 亲昵口语化";
    }
}

/* 天气词**按设备语言**出（提示词侧随语言走 → 模型生成的内容同语言；
   屏显侧由 main.c 用 T(WEATHER_NAME[...]) 走 i18n 表）。
   ⚠ 这些词只进提示词，不上屏 → 不受 zfont 字集约束。 */
static const char *card_weather(int weather)
{
    static const char *const WX_ZH[N_WEATHER]  = { "晴", "下小雨", "下雨", "下大雨", "有雾", "下雪", "打雷" };
    static const char *const WX_EN[N_WEATHER]  = { "clear", "drizzling", "raining", "pouring", "foggy", "snowing", "thunderstorm" };
    static const char *const WX_JA[N_WEATHER]  = { "晴れ", "小雨", "雨", "大雨", "霧", "雪", "雷" };
    static const char *const WX_ZHT[N_WEATHER] = { "晴", "下小雨", "下雨", "下大雨", "有霧", "下雪", "打雷" };
    const char *const *tab = WX_ZH;
    if      (g_lang == LANG_EN)  tab = WX_EN;
    else if (g_lang == LANG_JA)  tab = WX_JA;
    else if (g_lang == LANG_ZHT) tab = WX_ZHT;
    return (weather >= 0 && weather < N_WEATHER) ? tab[weather] : tab[0];
}

bool ws_api_diary(const char *dst, int day, int total, int weather, char *out, int cap)
{
    char home[64] = {0};
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l4 = sizeof(home);
        nvs_get_str(h, "city", home, &l4);             /* ★R157：居住城市 = 出发地 */
        nvs_close(h);
    }
    const char *from = home[0] ? home : "家";
    const char *w = card_weather(weather);
    /* ★R157：提示词带上「从哪出发」—— 原先只写目的地，所有用户生成的片都像从同一个地方出发。
       设备没有 GPS，起点 = 配网页选的居住城市 cfg->city（NVS city 键）。 */
    char usr[192];
    const char *hint = walk_hint();                    /* ★R163：换个说法，不然每次同一句 */
    switch (g_lang) {
    case LANG_EN:  snprintf(usr, sizeof(usr), "Leaving from %s for %s. Day %d of %d. Today: %s. %s.", from, dst, day, total, w, hint); break;
    case LANG_JA:  snprintf(usr, sizeof(usr), "%sから%sへ %d日目 共%d日間 今日は%s 今回は%s", from, dst, day, total, w, hint); break;
    case LANG_ZHT: snprintf(usr, sizeof(usr), "從%s出發去%s 現在是第%d天 一共%d天 今天%s 這次%s", from, dst, day, total, w, hint); break;
    default:       snprintf(usr, sizeof(usr), "从%s出发去%s 现在是第%d天 一共%d天 今天%s 这次%s", from, dst, day, total, w, hint); break;
    }
    return api_card_send(card_sys(), usr, out, cap, 120, API_TIMEOUT_MS, 2);
}

/* ★R170：`ws_api_nearby()` 已删除 —— 它只被「图卡配文」调用，而配文这一跳在 R170
   并进了 ws_api_scene（画面/标题/正文一次出，不再多打一次网络、也不会图字错配）。
   留着一个零调用的导出函数＝下次改配文口径时又冒出第二个真源，按项目规矩删掉。 */

/* ══ ★R170：一张 AI 明信片 = 画面 + 标题 + 正文，**一次调用同时拿到** ═══════════
   用户口径（2026-11-10）：「新增 22 个日常场景，那你不又全部固定了吗？我想要的
   是它随机生成 —— 理想情况我玩 100 年，它也能不停出新的图。要只有 22 个场景，
   那我为什么不提前弄好？提前弄好我掌控性还高一点。」
   ⇒ 结论：**画面不准再从固件清单里挑**。地标/日常这个分类、以及「寄自哪座城」
     由固件掷（保证近城为主、远的偶发）；但**具体画面**交给文字模型每次现想，
     同一座城这次是巷口、下次是天桥下的报亭，固件里没有清单能穷举。
     固件只做三件不自由的事，保证画风统一、上屏不炸：
       ① 给一句极短的引子（时段 / 天气 / 视线落在什么小东西上）把模型往不同角落推；
       ② 画面**必须英文**（flux 只吃英文），且不带人、不带猫；
       ③ 风格尾巴（铅笔速写 / 大面积留白）由调用方拼，模型别自己乱改画风。
   输出格式（严格三行）：
     第 1 行  英文画面 ≤ 40 词
     第 2 行  标题 ≤ 8 字
     第 3 行  正文 ≤ 30 字（一句）
   失败返回 false → 调用方回落到**本地**旧路（场景池 + 城市地标关键词 + 本地配文池），
   断网/限流也照样出片，只是那一张回到「清单里的画面」。 */
static const char *scene_hint(int i)
{
    /* 引子池：只给「往哪看」，不给「画什么」。6×6×4 组合已足够把模型推散，
       真正的画面仍由模型临场编 —— 组合数是有限的，成品数不是。 */
    static const char *const T_ZH[] = { "清晨", "上午", "正午", "下午", "傍晚", "深夜" };
    static const char *const M_ZH[] = { "落在一扇旧窗户上", "落在一段没人的台阶上", "落在一辆停着的旧车上",
                                        "落在一小块积水上", "落在一根电线上", "落在一个空着的凳子上" };
    static const char *const T_EN[] = { "early morning", "late morning", "noon", "afternoon", "dusk", "late night" };
    static const char *const M_EN[] = { "on an old window", "on a flight of empty steps", "on a parked bicycle",
                                        "on a small puddle", "on a power line", "on an empty stool" };
    static const char *const T_JA[] = { "早朝", "午前", "正午", "午後", "夕方", "深夜" };
    static const char *const M_JA[] = { "古い窓に", "人のいない階段に", "停めた自転車に",
                                        "小さな水たまりに", "電線に", "空いた椅子に" };
    static const char *const T_ZHT[] = { "清晨", "上午", "正午", "下午", "傍晚", "深夜" };
    static const char *const M_ZHT[] = { "落在一扇舊窗上", "落在一段沒人的台階上", "落在一輛停著的舊車上",
                                         "落在一小塊積水上", "落在一根電線上", "落在一個空著的凳子上" };
    const char *const *t = T_ZH, *const *m = M_ZH;
    if      (g_lang == LANG_EN)  { t = T_EN;  m = M_EN; }
    else if (g_lang == LANG_JA)  { t = T_JA;  m = M_JA; }
    else if (g_lang == LANG_ZHT) { t = T_ZHT; m = M_ZHT; }
    return (i == 0) ? t[esp_random() % 6u] : m[esp_random() % 6u];
}

/* 三行输出里各行的**去标签**：模型有时会自作主张加 "SCENE:"/"标题："。
   只剥已知前缀，不碰正文本身。 */
static const char *scene_line_skip(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '#') ++p;
    static const char *const LBL[] = { "SCENE", "PROMPT", "IMAGE", "TITLE", "BODY", "CAPTION",
                                       "画面", "描述", "标题", "正文", "配文" };
    for (unsigned k = 0; k < sizeof(LBL) / sizeof(LBL[0]); ++k) {
        const size_t n = strlen(LBL[k]);
        if (strncmp(p, LBL[k], n) == 0) {
            const char *q = p + n;
            while (*q == ' ' || *q == '\t') ++q;
            /* 分隔符只认 ASCII 的 : . 与全角「：、」（多字节，得整串比，不能塞进 ' ') */
            const bool sep = (*q == ':' || *q == '.') ||
                             strncmp(q, "：", 3) == 0 || strncmp(q, "、", 3) == 0;
            if (sep) {
                q += ((unsigned char)q[0] < 0x80) ? 1 : 3;
                if (*q == ' ') ++q;
                return q;
            }
        }
    }
    return p;
}

/* ★R170 全检：模型可能「第一处竖线半角、第二处全角」。改成线性扫描，
   半角 '|' 与全角 U+FF5C 一视同仁，返回分隔符长度（1 / 3）。 */
static char *scene_find_bar(char *p, int *sep)
{
    for (; *p; ++p) {
        if (*p == '|') { *sep = 1; return p; }
        if ((unsigned char)*p == 0xEF && (unsigned char)p[1] == 0xBD &&
            (unsigned char)p[2] == 0x9C) { *sep = 3; return p; }
    }
    return NULL;
}

/* 按 UTF-8 字符边界拷贝（不切出半个汉字 → 上屏不出现豆腐块）。 */
static void scene_u8cpy(char *dst, const char *src, int cap)
{
    if (!dst || cap <= 0) return;
    int o = 0;
    while (*src && o < cap - 4) {
        const char *nx = u8_next(src);
        const int cl = (int)(nx - src);
        if (cl <= 0) break;
        memcpy(dst + o, src, (size_t)cl);
        o += cl;
        src = nx;
    }
    dst[o] = 0;
}

bool ws_api_scene(const char *place, bool landmark, int weather,
                  char *en, int encap, char *title, int tcap, char *body, int bcap)
{
    if (!en || !title || !body || encap <= 4 || tcap <= 1 || bcap <= 1) return false;
    en[0] = title[0] = body[0] = 0;
    const char *p  = (place && place[0]) ? place : "家";
    const char *w  = card_weather(weather);
    const char *h0 = scene_hint(0);
    const char *h1 = scene_hint(1);
    char usr[384];   /* ★R170 全检：原来 224B，EN 日常片实测需 284B。
                        ★2026-11-12：landmark 分支的措辞加长（明确禁止可认出的建筑），
                        中文估算已到 220B、英文更长 ⇒ 留出余量防截断。 */
    switch (g_lang) {
    case LANG_EN:
        snprintf(usr, sizeof(usr),
                 "Place: %s. Weather: %s. Light: %s, falling %s. %s"
                 " Invent ONE small ordinary corner there that nobody photographs.%s",
                 p, w, h0, h1, landmark ? "This is a real city, so ONE local landmark MAY appear, but as a partial view or a corner, never a full postcard shot."
                                        : "Nothing famous, just daily life.",
                 landmark ? "" : " Keep it unrecognisable as any city.");
        break;
    case LANG_JA:
        snprintf(usr, sizeof(usr),
                 "場所 %s 天気 %s 光は%s %sに落ちる 今回 %s 誰も撮らない普通の片隅をひとつ考えてください",
                 p, w, h0, h1,
                 landmark ? "この街のランドマークが一角だけ出てもいい 正面からは描かない" : "名所は描かず ただの日常だけ");
        break;
    case LANG_ZHT:
        snprintf(usr, sizeof(usr),
                 "地點 %s 天氣 %s 光線是%s 落在%s 這次 %s 想一個沒人拍的普通角落",
                 p, w, h0, h1,
                 landmark ? "這次可以出現這座城市的地標建築 但要畫它的局部或一角 不要正面全景" : "不要名勝 只要日常");
        break;
    default:
        snprintf(usr, sizeof(usr),
                 "地点 %s 天气 %s 光线是%s 落在%s 这次 %s 想一个没人拍的普通角落",
                 p, w, h0, h1,
                 landmark ? "这次可以出现这座城市的地标建筑 但要画它的局部或一角 不要正面全景" : "不要名胜 只要日常");
        break;
    }
    /* 系统提示词：角色 + 严格三行格式（第 1 行强制英文） */
    const char *sys = NULL;
    switch (g_lang) {
    case LANG_EN:
        sys = "You design 1-bit pencil sketch postcards. Reply in EXACTLY 3 lines, nothing else, no labels. "
              "Line1: English. Name ONE concrete drawable thing as the subject, e.g. a chipped enamel cup on a "
              "windowsill, a bicycle leaning on a brick wall, laundry on a line, a worn stone doorstep. "
              "No light, no shadow, no atmosphere, no mood, no people, no cats, no text. "
              "If line 3 is about a landmark, line 1 must be that landmark. Max 25 words. "
              "Line2: a short title of 2 to 4 words naming that thing only. No abstract or emotional words. "
              "Line3: one sentence of at most 60 characters, one concrete visual detail only. "
              "Never write 'I', never mention waiting for the owner, no feelings. No punctuation, no emoji.";
        break;
    case LANG_JA:
        sys = "あなたは1bit鉛筆スケッチの絵手紙を作ります 必ず3行だけで答えてください 見出しや番号は書かない "
              "1行目 英語で 描ける具体物をひとつだけ 例 a chipped enamel cup on a windowsill "
              "a bicycle leaning on a brick wall laundry on a line a worn stone doorstep "
              "光も影も雰囲気も感情も書かない 人も猫も文字も名所も描かない 25語以内 "
              "2行目 タイトル 2から6文字 画面にある物の名前だけ 抽象語も感情語も使わない "
              "例 窓辺のコップ 壁にもたれた自転車 "
              "3行目 本文 一文 20から28文字 見た具体物だけを書く 私も 飼い主を待つも 気持ちも書かない "
              "例 路地の水たまりに空が映る "
              "句読点と記号と絵文字は一切使わない";
        break;
    case LANG_ZHT:
        sys = "你是1bit鉛筆速寫明信片的畫面設計 嚴格只回三行 不要任何標籤或編號 "
              "第一行 英文 只寫一個具體能畫出來的實物 例如 a chipped enamel cup on a windowsill "
              "a bicycle leaning on a brick wall laundry on a line a worn stone doorstep "
              "不寫光線 不寫影子 不寫氛圍 不寫情緒 不寫人 不寫貓 不寫文字 不寫名勝 25個英文詞以內 "
              "第二行 標題 5到8個字 名詞短語或短句 像明信片上印的那一行 要具體 "
              "不要抽象詞 不要情緒詞 不要提光線和影 "
              "例如 弄堂口 江邊的鐘樓 一隻茶碗 白牆上的窗 沙丘的曲線 "
              "第三行 正文 3到4個短句 用空格隔開 每句4到8個字 只寫看到的具體東西和動作 "
              "不要抒情 不要寫等主人 不要寫心情 "
              "例如 晾衣桿橫在頭頂 老人搬了竹椅坐 巷口誰也不說話 "
              "三行都不要標點 不要表情";
        break;
    default:
        /* ★★2026-11-12 用户令（实机报「图完全看不出是什么」+「标题出现无法识别的字」+
           「文案断句错、不像猫那一套」）三处一起改：
           · 旧提示词写 `只写光线和普通的东西` ⇒ 模型爱写「光影台阶」这种**没有轮廓**的词，
             1bit 200×200 根本画不出来。改为**只准写一个具体能画的实物**并给例词。
           · 标题旧口径是「不多于8字」，没约束词性 ⇒ 编出「旧车暖影」这种怪组合。
             改为「2-6 字的名词短语，只指画面里那个东西」，并给样例。
           · 正文旧口径只有一句「语气像想念主人的小猫」，没样本 ⇒ 飘成
             「我在这光影间静静等待主人微笑」。改为**明确禁止写「我/等主人/心情」**，
             只写看到的具体画面，并给两条样例对齐现有明信片池的调子。 */
        sys = "你是1bit铅笔速写明信片的画面设计 严格只回三行 不要任何标签或编号 "
              "第一行 英文 只写一个具体能画出来的实物 例如 a chipped enamel cup on a windowsill "
              "a bicycle leaning on a brick wall laundry on a line a corner of a stone tower "
              "不写光线 不写影子 不写氛围 不写情绪 不写人 不写猫 不写文字 25个英文词以内 "
              "第二行 标题 5到8个字 名词短语或短句 像明信片上印的那一行 要具体 "
              "不要抽象词 不要情绪词 不要提光线和影 "
              "例如 弄堂口 江边的钟楼 一只茶碗 白墙上的窗 沙丘的曲线 "
              "第三行 正文 3到4个短句 用空格隔开 每句4到8个字 只写看到的具体东西和动作 "
              "必须写第一行那个东西本身 第一行写的是建筑就写那座建筑 不许另起一个场景 "
              "不要抒情 不要写等主人 不要写心情 "
              "例如 晾衣杆横在头顶 老人搬了竹椅坐 巷口谁也不说话 "
              "三行都不要标点 不要表情";
        break;
    }
    char raw[1024];  /* ★R170 全检：260 token 的中文输出可超 512B，正文会被切没 */
    if (!api_card_send(sys, usr, raw, (int)sizeof(raw), 260, 28000, 1)) return false;

    /* 按分隔符切成 3 段：换行优先；模型把三行挤成一行时也认竖线，
       且**半角/全角混用**要能解析（见 scene_find_bar）。 */
    const char *l1 = NULL, *l2 = NULL, *l3 = NULL;
    {
        int d1 = 0;
        char *nl1 = strchr(raw, '\n');
        char *b1  = scene_find_bar(raw, &d1);
        char *p2 = NULL, *cut = NULL;
        int d2 = 0;
        if (nl1 && (!b1 || nl1 <= b1)) {
            *nl1 = 0; p2 = nl1 + 1;
            int db = 0;
            char *nl2 = strchr(p2, '\n');
            char *b2  = scene_find_bar(p2, &db);
            if (nl2 && (!b2 || nl2 <= b2)) { cut = nl2; d2 = 1; }
            else if (b2) { cut = b2; d2 = db; }
            /* ★R202 修（全量复查）：这里原来只给 cut 赋值、**没写 0** —— 与下面竖线分支不同。
               ⇒ l2（标题）一路延伸到串尾，标题 =「标题\n正文」，正文又被 l3 重复一遍。
               模型正常回三行时必中（正是「标题出现无法识别的字」的根因）。 */
            if (!cut) { ESP_LOGW(TAG, "★R170 三行格式没解析出来 → 当失败"); return false; }
            *cut = 0;
            l1 = raw; l2 = p2;
        } else if (b1) {
            const int step1 = d1;   /* ★先存长度再写 0，原来写完才判全角 → 恒读 0 */
            *b1 = 0; p2 = b1 + step1;
            int db = 0;
            char *b2  = scene_find_bar(p2, &db);
            char *nl2 = strchr(p2, '\n');
            if (b2 && (!nl2 || b2 <= nl2)) { cut = b2; d2 = db; }
            else if (nl2) { cut = nl2; d2 = 1; }
            if (!cut) { ESP_LOGW(TAG, "★R170 三行格式没解析出来 → 当失败"); return false; }
            *cut = 0;
            l1 = raw; l2 = p2;
        } else {
            ESP_LOGW(TAG, "★R170 三行格式没解析出来 → 当失败"); return false;
        }
        l3 = cut ? (cut + d2) : "";
    }

    l1 = scene_line_skip(l1);
    l2 = scene_line_skip(l2);
    l3 = scene_line_skip(l3);
    /* 画面必须英文（flux 不吃中文）：非 ASCII 占比过高就当失败，走兜底 */
    int non_ascii = 0, tot = 0;
    for (const char *q = l1; *q; ++q) { ++tot; if ((unsigned char)*q >= 0x80) ++non_ascii; }
    if (tot < 10 || non_ascii * 4 > tot) {
        ESP_LOGW(TAG, "★R170 画面不是英文（%d/%d 非 ASCII）→ 当失败", non_ascii, tot);
        return false;
    }
    /* 组装正文：l3 之后的行若还有内容，空格接上（模型拆行时不丢字）。
       正文为空 = 这一份不能用（宁可走本地兜底，也不要「模型标题 + 模板正文」错配）。 */
    if (strlen(l3) < 2) { ESP_LOGW(TAG, "★R170 正文为空 → 当失败"); return false; }
    if (!l2[0]) { ESP_LOGW(TAG, "★R170 标题为空 → 当失败"); return false; }
    scene_u8cpy(en, l1, encap);
    scene_u8cpy(title, l2, tcap);
    /* 正文：把剩余所有行用空格连起来（已把 l3 之后的 '\n' 全换成空格） */
    {
        char tmp[256];
        int o = 0;
        for (const char *q = l3; *q && o < (int)sizeof(tmp) - 2; ++q)
            tmp[o++] = (*q == '\n' || *q == '\r') ? ' ' : *q;
        while (o > 0 && tmp[o - 1] == ' ') --o;
        tmp[o] = 0;
        scene_u8cpy(body, tmp, bcap);
    }
    ESP_LOGW(TAG, "★R170 现编画面（%s · %s）：%s | %s / %s",
             p, landmark ? "地标类" : "日常类", en, title, body);
    fflush(stdout);
    return true;
}

/* ---------------------------------------------------------------- 状态缓存 */

void ws_save_state(const ws_state_t *st)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "st_utc",  (int32_t)st->now_local);   /* 存本地戳 + 时区可还原 */
    nvs_set_i32(h, "st_wx",   st->weather);
    nvs_set_i32(h, "st_temp", st->temp_c);
    nvs_set_i32(h, "st_phase", st->moon_phase);
    nvs_set_i32(h, "st_land", st->land);
    nvs_set_i32(h, "st_out",  st->outside ? 1 : 0);
    nvs_set_i32(h, "st_outts", (int32_t)st->out_ts);   /* ★ 返家时刻（秒） */
    nvs_set_i32(h, "st_outat", (int32_t)st->out_at);   /* ★ 2026-09-23 出门时刻（秒） */
    nvs_set_i32(h, "st_inti", st->intimacy);
    nvs_commit(h);
    nvs_close(h);
}

/* ★★ 2026-09-26（网络图标修复）：「上次**成功**联网」的本地伪 epoch 落 NVS。
   main.c 的 g_rtc_last_sync 只活在 RTC 保留内存 —— 深睡不丢，但掉电 / 烧录复位
   （冷启动）会清零；清零后只要本次联网失败，就被误判成「从未联网」亮「无网络」
   图标（用户报：明明记忆里存了 2 个 wifi，却亮「没连过互联网」）。
   所以每次联网成功时同步落一份 NVS，冷启动用 ws_load_last_sync() 读回。
   频率上限 = 联网次数（跨槽才联网，一天最多 ~8 次 + 手动），无 NVS 磨损顾虑。 */
void ws_save_last_sync(time_t now_local)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "sync_utc", (int32_t)now_local);
    nvs_commit(h);
    nvs_close(h);
}

/* 读回「上次成功联网」本地伪 epoch；没存过（新设备 / 旧固件升级上来）返回 0，
   语义与 RTC 保留区清零一致 = 从未联网。 */
time_t ws_load_last_sync(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    if (nvs_get_i32(h, "sync_utc", &v) != ESP_OK) v = 0;
    nvs_close(h);
    return (time_t)v;
}

void ws_load_state(ws_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->weather = WX_CLEAR;
    /* ★R193 用户令：「刚领养的猫，初始好感度应该是 0」—— 出厂默认由 60 改成 **0**。
       （0 会被下面的 INTI_FLOOR 夹到 20 ⇒ 状态页显示「刚认识」，不会一上来就「有点熟了」。
         ⚠ 别顺手把 INTI_FLOOR 也改成 0：地板 20 是「长期不互动也不至于归零」的玩法保底，
           改它等于把亲密度衰减的下界一起放开。） */
    st->intimacy = 0;
    st->moon_phase = ws_moon_phase(time(NULL), NULL);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        st->time_ok = false;
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, "st_utc", &v) == ESP_OK) st->now_local = (time_t)v;
    if (nvs_get_i32(h, "st_wx", &v) == ESP_OK) st->weather = v;
    if (nvs_get_i32(h, "st_temp", &v) == ESP_OK) st->temp_c = v;
    if (nvs_get_i32(h, "st_phase", &v) == ESP_OK) st->moon_phase = v;
    if (nvs_get_i32(h, "st_land", &v) == ESP_OK) st->land = v;
    if (nvs_get_i32(h, "st_out", &v) == ESP_OK) st->outside = (v != 0);
    /* ★★ 2026-09-23：出门时刻必须跟 outside 一起恢复 —— 少了它，
       深睡唤醒后立牌第一行会显示「已出门 0h」（明明已经出去半天了）。 */
    if (nvs_get_i32(h, "st_outat", &v) == ESP_OK) st->out_at = (time_t)v;
    /* ★★ 2026-09-30 修复（老账，flash24 快照里就在）：`st_outts` 原来是**只写不读** ——
       ws_save_state() 每次都写它，ws_load_state() 却从来不回读 → `st->out_ts` 载入恒 0。
       而 ws_refresh_outside() 的判据是「out_ts != 0 且 > now 才算在外」→
       唯一走 ws_load_state 的路径（**没配网 / 设置页要求重新配网**的冷启动）会把一只
       **正在外面**的猫改判成「在家」：立牌消失、猫精灵直接回到房间。
       （正常深睡唤醒走的是 RTC 快照 g_rtc_state，绕开了这里 —— 所以这个 bug 平时藏得很深，
        只有"恢复出厂 / 重新配网"之后才会露出来。）
       注：out_ts 与 out_at 必须**一起**恢复，否则「已出门 Nh / 预计 Nh 后回家」
       两行会自相矛盾（上一行按 out_at 算、下一行按 out_ts 算）。 */
    if (nvs_get_i32(h, "st_outts", &v) == ESP_OK) st->out_ts = (time_t)v;
    if (nvs_get_i32(h, "st_inti", &v) == ESP_OK) {
        st->intimacy = v;
        /* ★2026-10-06：与下面 weather/land/moon_phase 同款夹紧（NVS 值可能脏）
           ★R175：改用 INTI_FLOOR/INTI_CEIL（与写入侧同源，不再各写一份字面量） */
        if (st->intimacy < INTI_FLOOR) st->intimacy = INTI_FLOOR;
        if (st->intimacy > INTI_CEIL)  st->intimacy = INTI_CEIL;
    }
    nvs_close(h);
    /* ★★ 2026-10-04 体检修：给三个「会被当数组下标用」的字段**兜底夹紧**。
       原来 st_wx / st_land / st_phase 读出后直接赋值、无 clamp，而消费侧
       win_idx_of()（main.c）要算 `(land*N_PERIOD + period_visual(period))*N_WEATHER + weather`
       去索引 win.bin，fb_apply_win() 又没有条数校验（对比 fb_apply_cat 有 `pose >= n` 的保护）
       → NVS 一旦出现脏值/负值/超范围值（换固件版本、改 key 定义、NVS 损坏），
       就会算出负下标或远超帧数的下标 → **越界读 flash → 花屏或 HardFault**。
       （推测的触发路径，但夹紧本身零成本、零副作用，值得做。）
       夹紧放在读完 NVS 之后统一做，覆盖所有出口（含上面的 nvs_open 失败早退路径）。 */
    if (st->weather    < 0 || st->weather    >= N_WEATHER) st->weather    = WX_CLEAR;
    if (st->land       < 0 || st->land       >= N_LAND)    st->land       = 0;   /* LAND_PEARL */
    if (st->moon_phase < 0 || st->moon_phase >= N_PHASE)   st->moon_phase = 0;
    /* ★★ 2026-10-13 任务二 P1-5 修（load 侧双保险）：出厂兜底窗口
     *   [1767225600, +86400) = 2026-01-01 00:00 起一整天 —— 本工程两处兜底
     *   （net_sync.c ws_sync 的 1767225600+12h、main.c 冷启动的 1767225600+12h）
     *   都落在这个窗口里。老固件/异常路径若把它存了盘，>1.7e9 的判据会放它过，
     *   假时间由此「复活」污染 step_life（见 P1-5 根因注释）。⇒ 该窗口一律判不可信，
     *   不看出处。真实时间不可能落在 2026-01-01 一整天内还被当成设备时钟。 */
    st->time_ok = (st->now_local > 1700000000) &&
                  !(st->now_local >= 1767225600 && st->now_local < 1767225600 + 86400);
    st->from_cache = true;
}

/* ---------------------------------------------------------------- 出门状态机 */
/*
 * ★ 设计（来自交接说明 §12.6，用户认可的规则）：
 *   · 出门条件：吃饱（亲密度 ≥ 55）+ 天气好（晴）+ 距上次出门 ≥ 2 天
 *   · 出门时长：**小时级**（★ 2026-09-21 用户定，分布见下面的 OUT_* 常量）
 *   · 出门期间猫不在家 → 主界面不画猫，改贴「外出提示」立牌
 *   · 亲密度按「天」结算：每过一天 −2，地板 20、天花板 100
 *
 * ★★ 2026-09-21 口径变更（用户原话，别退回去）：
 *   「『预计 12h 后回家』这句话是你要写的，上面的时间最好是要实时刷新的，以小时为单位，
 *     现阶段把出门时间缩短，绝大部分出门时间在 12 小时以内，少部分超过 12 小时，
 *     后面再考虑延长，投喂这块概率不变」
 *   → 出门时长从「1~2 天」改成**小时级**：返家时刻记成**本地 epoch 秒**；
 *     首页立牌上那行字由固件按「剩余小时」实时画（见 main.c 的 fb_text_c /
 *     eta_hours）。旧版按「天」存的 `out_until` 会自动换算，无需清 NVS。
 */
/* ★★ 2026-10-11 用户拍板「延长猫出门的时间」（旧值 4~12 / 13~20，两天前那版是 8~24 / 25~36）。
   最终口径：**短途 8~24h 占 70% · 长途 25~48h 占 30%**；
   **触发概率不动** —— 仍是冷却满 2 天后 `OUT_PCT_FIXED`(70%) 掷骰（用户明确「不动，只改时长」）。
   ⚠ 改这里**必须同步**改 main.c 里 `eta_hours()` / `gone_hours()` 的两个 `h > N` 夹取 ——
     它们要夹到**长途上界**（现 48）；否则「已出门 40h」会被压成 24/36h，
     而且「已出门 Nh」+「预计 Nh 后回家」两行相加 ≠ 总时长（那两处注释讲过为什么必须自洽）。 */
#define OUT_SHORT_PCT    70      /* 70% 走「短途」档 ⇒ 长途档 30% */
/* ★★ 2026-10-20 数值重配：短途 8~24h → **6~14h**、长途 25~48h → **15~30h**。
   原值下一趟门的期望周期 ≈ 冷却(2天) + 掷骰等待(1/0.7≈1.43天) + 行程(均值 22.15h≈0.92天)
   ≈ **4.35 天/趟**，每趟期望掉落 0.53 件 ⇒ **0.85 件/周** —— 对一台"每天开机 3 次"
   的设备来说，等于两周才见到 1~2 件新东西，第 2~3 周就"没得玩了"。
   新值：行程均值降到 ≈ 12h，配合冷却 2→1 天、概率 70→85，
   周期 ≈ 1 + 1/0.85(≈1.18) + 0.5 ≈ **2.7 天/趟**（约 2.6 趟/周）。
   ⚠ 改上界**必须同步** main.c 里 eta_hours() / gone_hours() 的两个 `h > 30` 夹取
     （它们要夹到**长途上界**；否则「已出门 Nh」+「预计 Nh 后回家」两行相加 ≠ 总时长）。 */
/* ★★★ 2026-10-21 数值复配（第二遍）：**把「一趟门」压短、把出门概率降回 75%**。
   10-20 那轮的账：周期 ≈ 冷却1天 + 1/0.85 + 行程11.9h ≈ 2.67 天/趟，认为"空白期太长"；
   但它只缩短时长、**同时把概率抬到 85%、冷却砍到 1 天** ⇒
     · 单次空白仍是**连续 10 小时**（短途均值 10h）—— 陪伴感按"次"计，不按占比计；
     · 一趟产出只有 **0.6 件**（1 掷 × 命中 60%），等于拿陪伴感换掉落 —— 方向错了。
   ⇒ 本轮：单趟**砍半**（短途 3~9h 均值 6 · 长途 12~24h 均值 18），概率 85% → **75%**，
     省下的陪伴时间补给**单趟产出**（掷骰 2→3、发现奖励、折算提价，见 main.c）。
     新账：1/0.75=1.33 + 冷却1 + 行程0.33 ≈ 2.66 天/趟（频率几乎不变），
     但**连续空白 10h → 6h**（长途仍 18h，保留"出远门"的分量）。 */
#define OUT_SHORT_MIN_H   6
#define OUT_SHORT_MAX_H  14      /* ★2026-10-21 二调：3~9 → 6~14（用户：太短了） */
#define OUT_LONG_MIN_H   16
#define OUT_LONG_MAX_H   32      /* ★2026-10-21 二调：12~24 → 16~32（出远门该有分量） */

/* ★★ flash24（2026-09-30）出门四修 —— 用户拍板，根因见 HANDOVER「出门问题诊断」节。
   症状：猫「很久没出过门」。旧判据 = 亲密度≥55 且**必须晴天** 且 距上次出门≥2 天，
   三条硬条件叠加，再加亲密度只降不升 → 猫在第 3 天之后**永久**出不了门。 */

/* ③ 跨天结算上限：单次最多按 3 天算。
   根因：无时间基准那次同步（新设备 / 恢复出厂后）会把 last_day 写成 2026-01-01（20454），
   等联网拿到真实时间后 elapsed 暴跳 ≈270 天 → 亲密度被一次扣到下限 20，
   从此 <55 永久锁死。这里 clamp 住，配合 time_trust 双保险。 */
/* ★2026-10-21 二调 3 → **7**：原 3 天是防"时间跳变一次扣 270 天"的保险，
   但用户"出差一周没开机"是**正常场景**，被夹到 3 天会白丢 4 天的出门/寄信/亲密度。
   7 天 = 正常缺勤的上界；真·跳变仍由 time_trust 双保险兜（见 step_life）。 */
#define OUT_ELAPSED_MAX   7

/* ② 出门概率：★ 2026-10-01 下午按用户要求**与亲密度彻底解耦**。
   用户原话：「出门的概率跟好感度没有什么关系」。
   flash24 那版把亲密度线性映射成概率（30→0% · 55→50% · 80+→90%），等于把
   「亲不亲」和「出不出门」绑成了因果 —— 用户不要这层关系。
   现在 = **固定概率**：出门只看 距上次出门≥2 天（冷却）× 天气不是雷/大雨 × 掷骰。
   想改「猫大概多久出一次门」，只动这一个数（100 = 冷却到了必出门）。 */
/* ★★ 2026-10-20 数值重配 70 → **85**：与冷却 2→1 天、行程缩短配套，把"猫多久出一次门"
   从 ≈4.35 天压到 ≈2.7 天。用户口径不变：仍**与亲密度解耦**（只受性格 ±15/−10 与藏品影响）。 */
#define OUT_PCT_FIXED    60      /* ★R113（用户令 2026-10-31）75 → 60。理由两条：
                                     ① 75 + 藏品上限（旧 20pp）= 95% ⇒ 冷却一到几乎必出门，"在家"的
                                        时段被挤掉，猫在家的姿态/互动/喂食反而看不到；
                                     ② 75→95 那 20pp 的边际感知很弱（天天都出门 = 藏品没手感）。
                                     把基准压到 60、藏品上限提到 28（main.c 聚合侧）⇒ 60 ~ 88%，
                                     藏品从"锦上添花"变成"明显更容易出门"，且留出在家的时间。
                                   探险家 +15 → 75 / 观察家 −10 → 50，分档差仍是 25pp。 */
/* ★★ 2026-10-14 R44：性格出门参数从 main.c **搬来这里**（唯一真源）。
   历史：R26 接线时下面四处用的是裸字面量，main.c 里的同名宏成了孤儿（改宏不生效）。
   数值与搬移前逐个相等 ⇒ 行为零变化。调性格出门参数只动这里。 */
#define OUT_PCT_EXPLORER_DELTA  15     /* 探险家：出门概率 +15pp（70 → 85） */
#define OUT_PCT_OBSERVER_DELTA  (-10)  /* 观察家：出门概率 −10pp（70 → 60） */
#define OUT_LONG_PCT_EXPLORER   50     /* 探险家：长途占比 30% → 50%（长途偏好） */
#define OUT_HOURS_PHILO_PCT     20     /* 哲学家：出门时长 −20% */

/* ① 亲密度每日结算：猫在家陪着 **+0/天**（★2026-10-23 用户拍板：原来 +1/天 会让
   亲密度不经营也必然顶到 100，喂养/抚摸的“经营感”被抹平）；猫出门在外 −2/天（分离会疏远）。
   ⇒ 现在亲密度只由真实互动（喂食/抚摸/收信/回家）推动，长期不管它会缓慢回落。
   ★ 注意：亲密度仍照常累计、照常展示，不参与出门判定。 */
#define OUT_INTI_HOME    0
#define OUT_INTI_GONE   -2

/* ★ 2026-10-04 体检清理：原 out_chance_pct() 已删 —— 它是个**零参数零分支**的函数，
   函数体只有 `return OUT_PCT_FIXED;`，调用点也只一处。承载不了任何策略
   （真要改概率直接改 OUT_PCT_FIXED 宏就够了），反而让读代码的人以为
   「这里将来会有分档逻辑」。现在 step_life 里直接用 OUT_PCT_FIXED。 */

/* ④ 天气门槛放宽（原为「必须 WX_CLEAR」）：排除雷 / 大雨 / 雪，其余（雾 / 小雨 / 中雨 / 晴）都能出门。
   ★A6 天气豁免接线（2026-10-16，机制文档 §4.2③）：查聚合核心掩码 ——
   小雨伞=大雨不拦（位1）· 羽绒围巾=雪天不拦（位2，★2026-10-18 加雪天门槛后生效）·
   鹅卵石=雷也不拦（位4）。携带位即开关（g_loot[].carry 表驱动）。 */
static bool out_weather_ok(int w)
{
    const int wx = loot_fx_wx_mask();
    /* ★★ 2026-10-20 数值重配（**该前提已于 R112 过期，见下方 ★★R112**）：
       屏蔽范围从「雷 + 大雨 + 雪」收窄为**只屏蔽雷**。
       当时的理由：能豁免这三样的藏品（小雨伞 / 羽绒围巾 / 鹅卵石）**全部在"不可获得"清单里**
       （它们没有掉落路径、商店也不卖）⇒ 多雨/多雪地区的玩家会长期"猫一直不出门"，
       而且**手里没有任何办法改善** —— 不可控的外部天气 × 不可获得的解法 = 死局。
       在补上这三件的获得路径之前，先只留雷（雷的戏剧性最强：惊吓姿态 25%）。
       豁免位掩码逻辑**完整保留**（小雨伞位1 / 羽绒围巾位2 / 鹅卵石位4）。
       ── ★★R112（2026-10-31）复核：**上面"三件不可获得"的前提早已不成立** ——
          小雨伞(46) / 羽绒围巾(47) / 鹅卵石 现已全部进掉落表（城市表 `{ LOOT_I_MINI_UMBRELLA, 6 }`、
          `{ LOOT_I_SCARF, 6 }`、`{ LOOT_I_EGG_STONE, 6 }`）。收窄措施没跟着回滚，于是这两件
          变成「文案承诺了、代码里 bit1/bit2 却没人读」的纯空头承诺。
          ⇒ 下面的雨/雪两行**已恢复**，三档天气各有对应藏品可解，不再有"无解的死局"。
          这段注释保留原样，是为了记录"为什么曾经收窄过"，别再走回头路。 */
    if (w == WX_THUNDER && !(wx & 4)) return false;
    /* ★★R112 恢复：上面那段 2026-10-20 的注释说这三件「全部在不可获得清单里」——
       **该前提已过期**：现在 小雨伞(46) 在 LOOT_DROP_CITY（loot_system.c 城市表 `{ LOOT_I_MINI_UMBRELLA, 6 }`）、
       羽绒围巾(47) 在 `{ LOOT_I_SCARF, 6 }`、鹅卵石 在 `{ LOOT_I_EGG_STONE, 6 }`，**三件都拿得到了**。
       于是"雨/雪被豁免的门没开"不再是保护措施，而是**纯空头承诺**：
       小雨伞文案「下大雨也敢出门」、羽绒围巾「下雪也敢出门」，代码里 bit1/bit2 无人读取。
       ⇒ 恢复这两行，三档天气各自有对应藏品可解（雷=鹅卵石 / 大雨=小雨伞 / 雪=羽绒围巾）。 */
    if (w == WX_RAIN_H  && !(wx & 1)) return false;
    if (w == WX_SNOW    && !(wx & 2)) return false;
    return true;
}

/* ③ 「天」的序号：决定跨天。
   ★★ 2026-10-04 体检修：原来只有 1 个参数、直接 local/86400。但 now_local 是
      「本地伪 epoch」（= UTC + tz），这个除法的进位发生在**伪 epoch 的 00:00**，
      也就是**真实本地 08:00**（+8 时区）→ 亲密度结算 / 2 天出门冷却 / last_out
      全部以早上 8 点为界，猫的「一天」从早上 8 点开始。
      负时区（表里有「纽约」UTC-5）边界更离谱，跑到本地 19:00。
      现在显式减掉时区偏移，让「天」的边界落在**本地 00:00**。
      ⚠ 存量 last_day/last_out 是按旧边界算的，首次运行算出的 elapsed 会有偏移；
        有 OUT_ELAPSED_MAX 巨值保护（见 step_life），不会一次砸到地板。 */
static int day_number(time_t local_epoch, int tz_offset_sec)
{
    /* ★2026-10-06 核查修（严重）：原实现 `(local_epoch - tz) / 86400` **方向反了**。
       now_local 是「本地伪 epoch」= UTC + tz，用 gmtime_r 读出来**就是本地挂钟**，
       所以 `now_local / 86400` 的进位本就落在**本地 00:00**（这正是旧代码的正确行为）。
       减掉 tz 之后变成 `UTC / 86400`，进位落到 UTC 00:00 = 本地 08:00 —— 与注释宣称的
       「落在本地 00:00」正好相反，且和 main.c 的 g_rtc_sync_day
       （`now_local / 86400`，不减 tz）**不同源**。
       ⇒ 改回不减 tz：亲密度按天结算 / 出门冷却的换日点回到本地 00:00，两处口径统一。
       ⚠ 存量 last_day 是按旧（08:00）边界算的，首次切换会有 ±1 天偏差，
         有 OUT_ELAPSED_MAX 巨值保护，不会一次砸到地板。 */
    (void)tz_offset_sec;               /* 入参 now_local 已含时区，不需要再减 */
    return (int)(local_epoch / 86400);
}

/* 抽一次出门时长（小时）。用 esp_random()：出门是**事件**，不该每天固定。
   分布 = OUT_SHORT_PCT% 落 [OUT_SHORT_MIN_H, OUT_SHORT_MAX_H]，其余落长途档。
   ★★ 2026-10-11 R26（性格钩子，重设计稿 §5 表）：
   · 探险家「长途偏好」→ 长途占比 30% → **50%**；
   · 哲学家「出门时长 −20%」（抽完再折，下限 1h）。 */
static int pick_out_hours(int persona)
{
    uint32_t r = esp_random();
    const int short_pct = (persona == 0) ? (100 - OUT_LONG_PCT_EXPLORER) : OUT_SHORT_PCT; /* ★R44：原裸 50 */
    int h;
    if ((int)(r % 100) < short_pct)
        h = OUT_SHORT_MIN_H + (int)((r / 100) % (OUT_SHORT_MAX_H - OUT_SHORT_MIN_H + 1));
    else
        h = OUT_LONG_MIN_H + (int)((r / 100) % (OUT_LONG_MAX_H - OUT_LONG_MIN_H + 1));
    if (persona == 3) { h -= h * OUT_HOURS_PHILO_PCT / 100; if (h < 1) h = 1; }  /* ★R44：原 h -= h/5（等价 20%） */
    return h;
}

void ws_refresh_outside(ws_state_t *st)
{
    st->outside = (st->out_ts != 0 && st->out_ts > st->now_local);
}

/* ★★ 2026-10-26 用户令：召回币 —— 用了之后猫**立即回家**（secs=0），或把返家时刻压到 now+secs。
   返回 false = 猫本来就不在外面 / NVS 写失败（调用方不要消耗道具）。
   ★RAM 的 st->out_ts 可能为 0（深睡/复位后快照没带出门态）→ 回读 NVS 兜底，否则会误判「在家」。
   ★必须确认 NVS 写成功才返回 true：旧写法写失败也报成功 ⇒ 道具被吃掉、猫却没回来。
   ⚠ NVS 双键一起写：out_ts（step_life 读）/ st_outts（ws_load_state 读），出门时刻同理。 */
bool ws_out_recall(ws_state_t *st, int secs)
{
    if (!st) return false;
    const time_t now = st->now_local;
    time_t out_ts = st->out_ts;
    time_t out_at = st->out_at;
    if (out_ts == 0) {                              /* 兜底：直接读 NVS 的出门态 */
        nvs_handle_t h0;
        if (nvs_open(NVS_NS, NVS_READONLY, &h0) == ESP_OK) {
            int32_t v = 0;
            if (nvs_get_i32(h0, "out_ts", &v) == ESP_OK) out_ts = (time_t)v;
            if (nvs_get_i32(h0, "out_at", &v) == ESP_OK) out_at = (time_t)v;
            nvs_close(h0);
        }
    }
    if (out_ts == 0 || out_ts <= now) return false;   /* 在家 / 已经在回家路上 */
    if (secs < 0) return false;
    const time_t tgt = now + secs;                    /* secs=0 → 立即回 */
    if (out_ts > tgt) out_ts = tgt;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "★ 召回币：NVS 打开失败 → 不生效、不消耗");
        return false;
    }
    bool ok = nvs_set_i32(h, "out_ts",    (int32_t)out_ts) == ESP_OK &&
              nvs_set_i32(h, "st_outts",  (int32_t)out_ts) == ESP_OK &&
              nvs_set_i32(h, "out_at",    (int32_t)out_at) == ESP_OK &&
              nvs_set_i32(h, "st_outat",  (int32_t)out_at) == ESP_OK;
    /* ★R123（用户令 2026-11-03）：召回币**立即回家**这趟，回家结算的贝币只按 1/3 给
       （12 币换整趟 35~45 币的套利）。标志与出门态同一次 commit 落盘；
       loot_trip_return_cb 读一次即清，不会影响下一趟正常回家。 */
    if (ok && secs == 0) ok = (nvs_set_i32(h, "recall_cut", 1) == ESP_OK);
    if (ok) ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (!ok) {
        ESP_LOGW(TAG, "★ 召回币：NVS 写失败 → 不生效、不消耗");
        return false;
    }
    st->out_ts  = out_ts;
    st->out_at  = out_at;
    st->outside = (out_ts > now);
    ESP_LOGW(TAG, "★★ 召回币生效：%s（%ld）", secs == 0 ? "立即回家" : "返家时刻已前移",
             (long)out_ts);
    return true;
}

/* ★★★ R101（2026-10-28，实机 NVS 实证 bug）：**清出门态的唯一出口**。
   事故：触屏机 NVS 里 `exp_dest = -1`（远行标记已清），但 `out_ts` 仍指在 5 天后，
   立牌退回小时制、两行都被 `h > 32` 夹死 → 屏上恒定「已出门32h / 预计32h后回家」，
   用户报「猫一直不回来、时间也不更新」（真实值：已出门 44.6h / 剩余 75.4h / 总 120h）。
   根因：测试菜单「远行回家」只改 `st->out_ts/out_at` 再 `ws_save_state()`，
   而 ws_save_state 写的是 **st_outts/st_outat**，权威键 `out_ts/out_at` 一个字节没动。
   下一次 step_life 读 NVS 的 `out_ts`（未来时刻）→ `if (st->out_ts != 0)` 此刻 st 是 0
   → **不覆盖** → 旧值复活 → 又被 ws_save_state 回写成 st_outts ⇒「幽灵远行」自我固化。
   ⇒ 四个键（out_ts / out_at / st_outts / st_outat）必须一次 commit 同清，
     任何「让猫回家」的路径都调这里，**不要再手写那几个 nvs_set_i32**。 */
void ws_out_clear(ws_state_t *st)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "out_ts",   0);
        nvs_set_i32(h, "out_at",   0);
        nvs_set_i32(h, "st_outts", 0);
        nvs_set_i32(h, "st_outat", 0);
        nvs_set_i32(h, "st_out",   0);
        /* ★R124：召回折扣标志与串门窗口一并作废 —— 否则「召回后还没结算就强制归家」
           会把 recall_cut 留到下一趟正常回家，白削 1/3 贝币。 */
        nvs_set_i32(h, "recall_cut", 0);
        nvs_set_i32(h, "visit_t0",   0);
        nvs_set_i32(h, "visit_h",    0);
        if (nvs_commit(h) != ESP_OK)
            ESP_LOGW(TAG, "★ 清出门态：commit 失败（下次 step_life 的一致性自愈会再兜一次）");
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "★ 清出门态：NVS 打开失败 → 只清内存态");
    }
    if (st) {
        st->out_ts  = 0;
        st->out_at  = 0;
        st->outside = false;
    }
    ESP_LOGW(TAG, "★★ 出门态已清（out_ts/out_at/st_outts/st_outat 四键同清）");
}

/* ★R125（用户令 2026-11-03）：时钟跳变（换时区 / SNTP 校时回拨）时，把「绝对时刻类」
   NVS 键按同一个 delta 平移 —— 它们存的是**本地伪 epoch**，换 tz 后不换算会让猫
   多在外 ≤时区差、串门窗口短暂复活、远行天数算错。
   覆盖：out_ts/out_at/st_outts/st_outat（出门态）、visit_t0（串门窗口）、exp_start（远行起点）。 */
static void ws_shift_abs_times(time_t shift, ws_state_t *st)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        static const char *KEYS[] = { "out_ts", "out_at", "st_outts", "st_outat", "visit_t0",
                                      "exp_start", "bw_ts", "bw_done_at" };
        for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); ++i) {
            int32_t v = 0;
            if (nvs_get_i32(h, KEYS[i], &v) == ESP_OK && v > 0)
                nvs_set_i32(h, KEYS[i], (int32_t)((time_t)v + shift));
        }
        /* ★R128：好友串门到期时刻 fn0..19_vend 也是本地伪 epoch，换时区/校时同样平移 */
        for (int i = 0; i < 20; ++i) {
            char k[16];
            snprintf(k, sizeof(k), "fn%d_vend", i);
            int32_t v = 0;
            if (nvs_get_i32(h, k, &v) == ESP_OK && v > 0)
                nvs_set_i32(h, k, (int32_t)((time_t)v + shift));
        }
        nvs_commit(h);
        nvs_close(h);
    }
    if (st) {
        if (st->out_ts > 0) st->out_ts += shift;
        if (st->out_at > 0) st->out_at += shift;
    }
    ESP_LOGW(TAG, "★ 时钟跳变 %+llds → 出门/串门/远行绝对时刻已同步平移", (long long)shift);
}

/* ★★ R175（2026-11-10 用户令）：亲密度「梯度阻尼」—— 中间松、两端紧。
   用户原话：「设置一个梯度，某一阶段比较好上升，升到比如 80 再往上升就很难；
   降到比如 40 再往下降也比较难 —— 升也很难、降也很难」。
   做法：给每一「点」标价（cost，单位 = 1/100 点输入），攒够一个 cost 才真动 1 点：
     · x < 40    ：降 1 点要 3.0 点输入（难降，护住地板）；升 1 点仍是 1.0（好爬回来）
     · 40 ≤ x < 80：升/降都是 1.0（好升好降，主活动区）
     · x ≥ 80    ：升 1 点要 4.0 点输入（难升，「亲密无间」变成要经营才到的成就）
   目的：80 以上不再被自动行为（回家 +2/+3、寄片 +2、掉落 +3）轻松顶满，互动才有分量；
   同时不像纯衰减那样「长期不玩就崩」—— 40 以下有明显回拉。
   取舍：余数只存 RAM（s_inti_frac_*，重启清零），最多损失不到 1 点的零头，
   不值得为它新增 NVS 键（既有存档零风险）。日结算走 inti_day_delta()（就近取整、不留余数）。 */
#define INTI_HI          80
#define INTI_LO          40
#define INTI_COST_NORM  100
#define INTI_COST_UP_HI 400
#define INTI_COST_DN_LO 300
/* INTI_FLOOR / INTI_CEIL 已上移到文件头（NVS_NS 旁），三处 clamp 共用一份。 */

/* ★★ R176（2026-11-10 用户拍板）：**自动来源封顶**。
   用户原话：「其实我没太怎么跟它互动，但猫的好感度都这样、都是 100，降不下来，
   是不是有点太累赘了」→ R175 只做了梯度（升得慢），但模拟实测「不互动 180 天仍磨到 100」，
   因为自动行为一趟送 +5 点输入（回家 +2/+3 + 每张片 +2），而衰减一趟只扣 0.84。
   ⇒ 再加一条硬上限：回家 / 寄片 / 远行兜底 / 掉落 / 里程碑 这些**不用人管就会发生**的来源，
   最多把亲密度推到 70；70 以上只有真互动（喂食 / 抚摸 / 猫薄荷 / 小鱼饼干）能推。
   效果：不经营最高停在 70（形影不离），「亲密无间」(85) 必须持续照顾才到得了；
   已经 100 的存档不会自己跳下来，要靠「在外过夜 −2/天」慢慢回落（约 3 个月回到 70 附近）。 */
#define INTI_AUTO_CAP    70

/* ★★ R176b（2026-11-10 用户追问）：「不互动最多能掉到 40 吧？」
   —— 光有封顶 70 还不够：封顶只挡住「推上去」，可 70 以下自动加成照样每趟 +5 输入，
   会把值顶在 70 不动（永远「形影不离」）。用户要的是「完全不互动就该一路掉，最低 40」。
   所以补「闲置淡出」：
     · 连续 INTI_IDLE_START(3) 天没有任何**真互动**（喂食 / 抚摸 / 猫薄荷 / 小鱼饼干）→
       自动加成**停摆**（ws_intimacy_add_auto 直接返回现值），且每天 −1；
     · 淡出**止于 40**（INTI_IDLE_FLOOR）——「还在试探」档的下沿，不会掉回「刚认识」。
   注意与「在外 −2/天」并存：那是分居惩罚，仍可低到 INTI_FLOOR(20)；
   本条只管「人在家、但一直不理它」。 */
#define INTI_IDLE_START   3
#define INTI_IDLE_FLOOR  40

static int s_inti_frac_up, s_inti_frac_dn;

/* 当前值 x 下，往 delta 方向走 1 点要多少「输入信用」。 */
static int inti_cost(int x, int delta)
{
    if (delta > 0) return (x >= INTI_HI) ? INTI_COST_UP_HI : INTI_COST_NORM;
    return (x < INTI_LO) ? INTI_COST_DN_LO : INTI_COST_NORM;
}

/* 互动加成（带余数累积）：返回**实际位移**（可为 0），不 clamp —— 调用方负责 clamp。
   cap = 本次来源允许达到的上限（互动 = INTI_CEIL；自动来源 = INTI_AUTO_CAP）。 */
static int inti_apply_damped(int x, int delta, int cap)
{
    if (delta == 0) return 0;
    if (delta > 0) {
        s_inti_frac_dn = 0;                      /* 反向翻转 → 丢弃反向余数，防「先攒后爆」 */
        if (x >= cap) { s_inti_frac_up = 0; return 0; }
        int credit = s_inti_frac_up + delta * 100;
        int moved  = 0;
        while (x < cap && credit >= inti_cost(x, +1)) {
            credit -= inti_cost(x, +1);
            ++x; ++moved;
        }
        s_inti_frac_up = (x >= cap) ? 0 : credit;
        return moved;
    }
    s_inti_frac_up = 0;
    if (x <= INTI_FLOOR) { s_inti_frac_dn = 0; return 0; }
    int credit = s_inti_frac_dn + (-delta) * 100;
    int moved  = 0;
    while (x > INTI_FLOOR && credit >= inti_cost(x, -1)) {
        credit -= inti_cost(x, -1);
        --x; ++moved;
    }
    s_inti_frac_dn = (x <= INTI_FLOOR) ? 0 : credit;
    return -moved;
}

/* 日结算（跨天，一次最多几笔）：同样按梯度标价，但**就近取整、不留余数** ——
   整数天的批量账攒小数没有意义，四舍五入反而更可预期。
   ★2026-11-12 修：逐点按「当前 x」取 cost，跨 INTI_LO / INTI_HI 阈值自然分段，
     与互动侧 inti_apply_damped() 的逐点计价口径对齐。旧实现只按初始 x 取一次
     cost 就 (mag*100+cost/2)/cost 算完整笔：x=41、delta=-6 会整笔按 NORM(100)
     扣 6 点，把 40 以下「难降」那段直接吃掉；补算天数越多偏差越大。
     单一档位下本实现与旧式逐字等价（cost=100 → 6 点；300 → 2 点；400 → 2 点）。 */
static int inti_day_delta(int x, int delta)
{
    if (delta == 0) return 0;
    const bool up   = (delta > 0);
    const int  sign = up ? 1 : -1;
    int cur = x, left = up ? delta : -delta, pts = 0, credit = 0;
    while (left-- > 0) {
        const int cost = inti_cost(cur, sign);   /* 每点重取 → 跨档自动换价 */
        credit += 100;
        if (credit >= cost) {
            credit -= cost;
            ++pts;
            cur += sign;
            if (cur >= INTI_CEIL || cur <= INTI_FLOOR) { credit = 0; break; }
        }
    }
    if (credit * 2 >= inti_cost(cur, sign)) ++pts;   /* 收尾就近取整 */
    return up ? pts : -pts;
}

/* 内部公用：读现值 → 过梯度阻尼 → 按 cap 夹紧 → 双键落盘。返回新值（失败 -1）。 */
static int inti_add_common(int delta, int cap, const char *who)
{
    if (delta == 0) return -1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    int32_t v = 60;                                  /* 与 ws_load_state 的出厂默认一致 */
    int32_t cur = 0;
    if (nvs_get_i32(h, "st_inti", &cur) == ESP_OK) v = cur;
    const int before = (int)v;
    /* ★R176b：真互动（互动通道 cap=100）→ 闲置计时清零。
       **必须在位移判定之前**：80 以上摸一次可能 moved=0（余数没攒够），
       但它确实是互动，不能因为「这次没涨」就被判成闲置。 */
    const bool is_touch = (cap >= INTI_CEIL);
    if (is_touch) nvs_set_i32(h, "inti_idle", 0);
    else {
        /* ★R176b：闲置淡出期间，**自动来源一律不生效** ——
           否则「每趟回家 +3 / 每张片 +2」会把值稳稳托在 70，用户永远看不到"不理它就掉"。
           这段是「不经营就该掉」能不能成立的关键，不是可省的性能优化。 */
        int32_t idle = 0;
        if (nvs_get_i32(h, "inti_idle", &idle) == ESP_OK && idle >= INTI_IDLE_START) {
            nvs_close(h);
            ESP_LOGD(TAG, "闲置 %d 天 → 自动加成 %+d 不生效（保留 %d）", (int)idle, delta, before);
            return before;
        }
    }
    const int moved  = inti_apply_damped((int)v, delta, cap);   /* ★R175 梯度阻尼 + ★R176 封顶 */
    if (moved == 0) {
        /* 没有真实位移（已到上限 / 余数还没攒够）→ **不写盘**。
           ！唯一例外：真互动刚清了 inti_idle，那一笔必须 commit，否则「摸过但没涨」
             会被当成没摸过，闲置淡出照常往下走。 */
        if (is_touch) nvs_commit(h);
        nvs_close(h);
        ESP_LOGD(TAG, "亲密度 %+d（%s）→ 仍 %d，无位移", delta, who, before);
        return before;
    }
    v = before + moved;
    if (v < INTI_FLOOR) v = INTI_FLOOR;
    if (v > cap)        v = cap;
    nvs_set_i32(h, "st_inti", v);
    nvs_set_i32(h, "inti",    v);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "★ 亲密度 %+d → %d（%s%s）", delta, (int)v, who,
             (moved != delta) ? ((v >= cap) ? "· 已到上限" : "· 梯度阻尼已削") : "");
    fflush(stdout);
    return (int)v;
}

/* ★flash24（2026-09-30）：互动加亲密度（喂食 +2 / 抚摸 +1 由 main.c 调用）。
   过去亲密度**只降不升**，是「猫再不出门」的根因之一；这里补上上升通道。
   · 地板 20 / 天花板 100 与 step_life 同源；
   · NVS 双键一起写：st_inti（ws_load_state 读）/ inti（step_life 写）—— 少写一个就会在
     下次 ws_load_state 时被旧值覆盖回去；
   · 返回新值，调用方直接写回内存，界面立刻显示新档位（不必等下次 ws_sync）。
   ★R175：内部先过梯度阻尼（inti_apply_damped），delta 是「输入的意图值」，
     实际位移可能更小（80+ 时 1/4 速），日志会一并打出。
   ★R176：本函数是**真互动**通道（喂食/抚摸/道具），上限仍是 100。 */
int ws_intimacy_add(int delta)
{
    return inti_add_common(delta, INTI_CEIL, "互动加成");
}

/* ★R176：**自动来源**通道（回家/寄片/远行兜底/掉落/里程碑）—— 最多推到 INTI_AUTO_CAP(70)。
   70 以上调它返回现值且不落盘（省 flash，也避免每趟都写一次 NVS）。
   语义上与 ws_intimacy_add 只差一个上限：值得单独开一个函数，而不是给调用点加参数，
   因为「这事件要不要人来管」是事件本身的属性，写错在调用点就会被下一个改的人搞乱。 */
int ws_intimacy_add_auto(int delta)
{
    return inti_add_common(delta, INTI_AUTO_CAP, "自动·封顶70");
}

/* ★★ 2026-10-11 R26：出门/回家/同步收尾三个事件钩子（见 net_sync.h 的声明注释）。
   掉落/里程碑/保底全在 main.c 结算 —— 本文件只发信号，不认识收藏品。 */
static ws_trip_start_fn  s_trip_start_fn  = NULL;
static ws_trip_plan_fn   s_trip_plan_fn   = NULL;   /* ★2026-10-18 方案甲：出门掷骰前问去向 */
static ws_trip_return_fn s_trip_return_fn = NULL;
static ws_trip_alive_fn  s_trip_alive_fn  = NULL;   /* ★R101：问「是否在远行」（幽灵态自愈用） */
static ws_sync_done_fn   s_sync_done_fn   = NULL;
void ws_set_trip_start(ws_trip_start_fn fn)    { s_trip_start_fn  = fn; }
void ws_set_trip_plan(ws_trip_plan_fn fn)      { s_trip_plan_fn   = fn; }   /* ★2026-10-18 */
void ws_set_trip_return(ws_trip_return_fn fn)  { s_trip_return_fn = fn; }
void ws_set_trip_alive(ws_trip_alive_fn fn)    { s_trip_alive_fn  = fn; }   /* ★R101 */
void ws_set_sync_done(ws_sync_done_fn fn)      { s_sync_done_fn   = fn; }

/* ★★ R26（性格/效果钩子的读取）：petns:persona（0 探/1 观/2 撒/3 哲，-1 无）
 * 与 petns:loot_own 的干花枝位。只在 step_life（联网结算，一天至多几次）读 ——
 * 频率低，NVS 直读可接受；不为此新增钩子 API。
 * ⚠ NS_LOOT_I_FLOWER = main.c g_loot[] 里干花枝的下标（两表必须同步，改表要连带）。 */
#define NS_LOOT_I_FLOWER 17
static int ns_persona_of(void)
{
    nvs_handle_t h;
    int32_t v = -1;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return -1;
    nvs_get_i32(h, "persona", &v);
    nvs_close(h);
    return (int)v;
}
/* ★2026-10-16 清理：ns_loot_owned_bit() 已死 —— 收拢批次后 net_sync 一律走
   loot_fx_*()（main.c 聚合核心）取数；只读低 32 位的旧函数与它的查询包装
   loot_fx_has() 已于 2026-10-26 全量审查一并删除。 */

static void step_life(ws_state_t *st, const ws_config_t *cfg, bool time_trust)
{
    (void)cfg;
    nvs_handle_t h;
    int last_day = 0, last_out = 0;
    int32_t out_ts = 0, out_until_old = 0, out_at = 0;
    int intimacy = st->intimacy;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "last_day", &v) == ESP_OK) last_day = v;
        if (nvs_get_i32(h, "last_out", &v) == ESP_OK) last_out = v;
        if (nvs_get_i32(h, "out_ts",   &v) == ESP_OK) out_ts = v;
        if (nvs_get_i32(h, "out_until", &v) == ESP_OK) out_until_old = v;
        if (nvs_get_i32(h, "out_at",   &v) == ESP_OK) out_at = v;   /* ★ 2026-09-23 */
        /* ★2026-10-06 核查修：出门状态原来**只看 NVS**，把 ws_load_state() 刚从
           st_outts/st_outat 恢复进 st 的值**无条件覆盖掉**（正在外出的猫会被判回家、
           立牌消失）。改成以入参 st 为准，NVS 值只在 st 里为 0 时才兜底。 */
        if (st->out_ts != 0) out_ts = (int32_t)st->out_ts;
        if (st->out_at != 0) out_at = (int32_t)st->out_at;
    } else {
        /* ★★ 2026-10-20 修：**fail-open 判猫回家**是危险的。
           原写法在 NVS 打开失败（句柄耗尽 / 分区忙 / flash 写入中）时把 `st->outside=false`
           并直接 return ⇒ 正在外面的猫被强行判成在家：立牌消失、猫回房间，而且这条早退
           连亲密度结算都不做。一次偶发的 NVS 失败就把"出门状态"永久改错。
           正确取舍：拿不到存档就**什么都不改**（保留调用方传进来的 st 原样），只打日志，
           让上一层的状态继续生效 —— 宁可这一轮不结算，也不能凭空把猫变回家。 */
        ESP_LOGW(TAG, "step_life: NVS 打开失败 → 本轮不结算（保留现有出门状态，不做任何改写）");
        return;
    }

    const time_t now = st->now_local;
    int today = day_number(now, cfg->tz_offset_sec);   /* ★ 2026-10-04：传 tz，边界落本地 00:00 */
    if (last_day == 0) last_day = today;              /* 首次运行 */
    int elapsed = today - last_day;
    if (elapsed < 0) elapsed = 0;

    /* ★flash24 ③-a：跨天结算巨值保护（时间跳变的第二道保险）。
       第一道是下面的 time_trust 检查；万一仍被绕过，最多也只按 OUT_ELAPSED_MAX 天算，
       绝不允许「一次扣 270 天」把亲密度砸到地板。 */
    if (elapsed > OUT_ELAPSED_MAX) {
        ESP_LOGW(TAG, "★ 跨天结算 elapsed=%d 过大（时间跳变？）→ 按 %d 天算",
                 elapsed, OUT_ELAPSED_MAX);
        fflush(stdout);
        elapsed = OUT_ELAPSED_MAX;
    }

    /* ② 旧数据兼容：早先存的是「第几天回来」（out_until）。
          读到就换算成「那天的 0 点」当返家时刻，随后由新逻辑接管（并把旧键清零）。
          ★flash24：本段从「亲密度结算之后」提到之前 —— 下面的亲密度结算要用 outside。 */
    if (out_ts == 0 && out_until_old != 0) {
        out_ts = out_until_old * 86400;
        ESP_LOGW(TAG, "★ 旧版出门数据 out_until=%d（天）→ 换算成返家时刻", (int)out_until_old);
        fflush(stdout);
    }

    /* ③ 出门判定：返家时刻还没到 = 还在外面 */
    bool outside = (out_ts != 0 && (time_t)out_ts > now);

    /* ★★★ R101（2026-10-28）「幽灵出门态」自愈 —— 实机 NVS 实证的 bug 兜底：
       out_at / out_ts / st_outat / st_outts 四键里只要有一对没同步清掉，
       旧值就会在下一轮从这里复活（读 NVS 的 out_ts，st->out_ts 为 0 时不覆盖），
       于是猫「永远在外面」，而立牌已经退回小时制、两行全被 h>32 夹死变成恒定 32。
       判据：在外面 + **不是远行** + 已出门超过普通出行上界（OUT_LONG_MAX_H=32h，留 2h 余量）。
       ★R124 修：普通出门写盘上限是 **48h**（step_life 有 hours>48→48，含怀表/地图册等
       加时藏品），旧 cap=34h 会把 36~48h 的正常长途当脏态强返。cap 改为 50h（=48+2）。 */
    if (outside && !(s_trip_alive_fn && s_trip_alive_fn())) {
        const time_t cap = (time_t)50 * 3600;
        if (out_at > 0 && (now - (time_t)out_at) > cap) {
            ESP_LOGW(TAG, "★★ 幽灵出门态自愈：已出门 %ldh > 普通行程硬上限 50h 且无远行标记 "
                          "→ 判四键不同步，强制归家（out_ts 原值 %ld）",
                     (long)((now - (time_t)out_at) / 3600), (long)out_ts);
            fflush(stdout);
            if (s_trip_return_fn) s_trip_return_fn((time_t)out_at, (time_t)out_ts);
            last_out = today;
            out_ts = 0;
            out_at = 0;
            outside = false;
            /* st_* 两个键也必须同轮清掉，否则 ws_load_state 又把旧值搬回内存 */
            nvs_set_i32(h, "st_outts", 0);
            nvs_set_i32(h, "st_outat", 0);
            nvs_set_i32(h, "st_out",   0);
            /* ★R119 修：这里必须**立即 commit**。原来只 set 不 commit，而下面
               「时间不可信」会在 1560 行早退并 nvs_close(h) —— 未提交的 st_* 归零
               全部丢弃，下一轮 ws_load_state 又把幽灵出门态搬回来，自愈白做。 */
            nvs_commit(h);
        }
    }

    /* ★flash24 ③-b：时间基准不可信（本轮走了出厂兜底时间）→ 生命结算整段跳过：
       不扣也不加亲密度、不出门、**绝不写 last_day**。
       污染源头就是「兜底时间那次同步无条件 nvs_set last_day」——不写盘即根治。 */
    if (!time_trust) {
        ESP_LOGW(TAG, "★ 时间基准不可信 → 本轮跳过生命结算（last_day=%d 保持不动）", last_day);
        fflush(stdout);
        st->intimacy = intimacy;
        nvs_close(h);      /* ★2026-10-06 核查修：原来这条早退路径漏关句柄（NVS 句柄泄漏） */
        return;
    }

    /* ① 按天结算亲密度（★flash24 改）：
         · 猫在家陪着 → +1/天（慢慢亲起来，保证长期能跨过出门门槛）；
         · 猫出门在外 → −2/天（保留原「分开久了会疏远」的设定）。
         另有互动加成：喂食 +2 / 抚摸 +1（main.c 调 ws_intimacy_add）。
       ★★ 2026-10-11 R26（衰减修正，**可叠**）：干花枝「亲密度每日衰减 −1」+
         哲学家「亲密度衰减 −1」（机制文档 §0.5 / 重设计稿 §5）—— 都只作用于
         「出门在外」这条衰减（在家 +1/天 不动），逐日 −1 直到 0（不会变增益）。 */
    int gone_daily = OUT_INTI_GONE;
    /* ★ 2026-10-16 收拢：原来这里自己读 NVS 位图只认「干花枝」一件
       （且只读得到低 32 位 → 下标 ≥32 的件全看不见）。现在问聚合核心：
       合照/双人伞 → −1.5/天（本系统按整日结算，取 −1）、月亮石/常青藤/干花枝 → 减半、
       同心结 → **在外期间完全不变淡**（decay 0）。日结算粒度不够就分两档：
       decay < 100 记 +1；decay <= 25 再 +1（等于"不掉"）。 */
    /* ★scope 接线（2026-10-18 方案甲）：月亮石（夜×0.5）/ 双人伞·常青藤（雨）按
       结算时刻的夜/雨计入 —— 日结算粒度下用「结算那一刻」的场合近似，跨夜长账不拆分。 */
    const bool fx_night = loot_fx_is_night(st->now_local);
    const bool fx_rain  = loot_fx_is_rain(st->weather);
    const int decay = loot_fx_decay_pct(fx_night, fx_rain);
    if (decay < 100) gone_daily += 1;
    if (decay <= 25) gone_daily += 1;
    if (ns_persona_of() == 3)                gone_daily += 1;   /* 哲学家 */
    if (gone_daily > 0) gone_daily = 0;
    if (elapsed > 0) {
        int before = intimacy;
        /* ★R175：日结算也走梯度阻尼 —— 在 40 以下「难降」这段才真的生效
           （否则在外 −2/天 会一路匀速砸到地板，两端阻尼只在互动侧成立 = 半成品）。 */
        const int day_delta = outside ? (gone_daily * elapsed) : (OUT_INTI_HOME * elapsed);
        const int real      = inti_day_delta(intimacy, day_delta);
        intimacy += real;
        if (intimacy < INTI_FLOOR) intimacy = INTI_FLOOR;
        if (intimacy > INTI_CEIL)  intimacy = INTI_CEIL;
        ESP_LOGI(TAG, "经过 %d 天（猫%s），亲密度 %d → %d%s%s",
                 elapsed, outside ? "在外面" : "在家", before, intimacy,
                 (outside && gone_daily != OUT_INTI_GONE) ? "（衰减修正生效）" : "",
                 (real != day_delta) ? "（梯度阻尼）" : "");
        fflush(stdout);
    }
    /* ★★ R176b（2026-11-10 用户追问「不互动最多能掉到 40 吧？」）：**闲置淡出**。
       为什么单独一段、不并进上面的 day_delta：
         · day_delta 走的是「在外 −2 / 在家 0」的既有口径，改它会把 R125 以来的实测基线全推翻；
         · 淡出是**新增的一层**：人在家、但已经 INTI_IDLE_START 天没喂没摸 —— 这时才淡，
           而且地板是 40（还在试探），不是在外分居那种 20（刚认识）。两条语义本来就不同。
       触发后**每天 −1**（关机 N 天就一次 −N，与在外结算同粒度），止于 INTI_IDLE_FLOOR。 */
    {
        int idle_days = 0;
        int32_t v32 = 0;
        if (nvs_get_i32(h, "inti_idle", &v32) == ESP_OK) idle_days = (int)v32;
        if (idle_days < 0) idle_days = 0;
        idle_days += elapsed;
        if (idle_days > 3650) idle_days = 3650;       /* 十年，防溢出，够用 */
        if (!outside && idle_days >= INTI_IDLE_START && intimacy > INTI_IDLE_FLOOR) {
            int drop = elapsed;
            if (intimacy - drop < INTI_IDLE_FLOOR) drop = intimacy - INTI_IDLE_FLOOR;
            if (drop > 0) {
                intimacy -= drop;
                ESP_LOGI(TAG, "★ 闲置 %d 天无互动 → 亲密度 −%d → %d（闲置地板 %d）",
                         idle_days, drop, intimacy, INTI_IDLE_FLOOR);
                fflush(stdout);
            }
        }
        nvs_set_i32(h, "inti_idle", idle_days);
    }
    if (!outside && out_ts != 0) {
        /* ★★ R26：回家结算（掉落/里程碑）—— 必须在清 out_ts/out_at **之前**发，
           main.c 的钩子只关心「这趟结束了」，时刻参数仅作日志用。 */
        if (s_trip_return_fn) s_trip_return_fn((time_t)out_at, (time_t)out_ts);
        /* ★★2026-10-21（用户问：「很长一段时间没动它，期间两次出门会怎么显示？」）：
           **答案（改前）**：不会"叠加显示"，但会**整段丢失** —— step_life 是单次结算，
           跨 N 天只结算"眼前这一趟"，中间 N−1 趟的藏品/明信片/贝币全部丢掉；
           而且回家后 last_out=today ⇒ 当天不会再出门（冷却 1 天）⇒ 也不会"两次叠着"。
           体感 = "猫明明出去了那么久，回来却什么都没带"，主页只显示猫已回家（无带回提示）。
           **改法**：按 elapsed−1 趟**补算**，每趟走完整的回家结算钩子（掉落/折算/寄信/保底）。
           ⇒ 出差一周回来，能看到这几天攒下的一堆东西；补算上限 = OUT_ELAPSED_MAX(7)。 */
        int extra = elapsed - 1;
        if (extra > 2) extra = 2;   /* ★R125（用户令）：最多补算 2 趟 —— 原来按「天数」补算，
                                       关机一周会凭空多出 6 趟完整掉落/贝币/亲密（假收益）。 */
        for (int i = 0; i < extra; ++i) {
            if (s_trip_return_fn) s_trip_return_fn(0, 0);
            ESP_LOGI(TAG, "★ 补算第 %d/%d 趟（离线期间的出门）", i + 1, extra);
        }
        fflush(stdout);
        /* 刚回来：清掉返家时刻与出门时刻，并把「上次出门日」记成今天（下次要重新等 2 天） */
        last_out = today;
        out_ts = 0;
        out_at = 0;
        ESP_LOGI(TAG, "★ 猫回家了");
        fflush(stdout);
    }
    if (!outside && elapsed > 0) {
        /* 出门判据（flash24 重写 → 2026-10-01 下午去掉亲密度项）：
           旧版 = 亲密度≥55 **且** 天气必须 WX_CLEAR **且** 距上次出门≥2 天，三条全是硬卡；
           叠加「亲密度只降不升」，猫在第 3 天之后就永久出不了门（用户报的「很久没出过门」）。
           现在 = ① 距上次出门≥2 天（冷却，保留）× ② 天气只排除雷/大雨 × ③ **固定概率**掷骰。
           ★ 亲密度**不再参与**这项判定（用户：「出门的概率跟好感度没有什么关系」）。 */
        int p_out = OUT_PCT_FIXED;      /* ★ 2026-10-04：原 out_chance_pct() 已删（零分支函数），直接用宏 */
        /* ★★ 2026-10-11 R26（性格钩子，重设计稿 §5 表）：探险家出门概率 +15pp（散心）/
           观察家 −10pp。亲密度仍不参与（用户口径不变）。 */
        const int persona = ns_persona_of();
        if      (persona == 0) p_out += OUT_PCT_EXPLORER_DELTA;   /* ★R44：原裸 15 */
        else if (persona == 1) p_out += OUT_PCT_OBSERVER_DELTA;   /* ★R44：原裸 -10 */
        /* ★★ 2026-10-18 方案甲（场合接线）：掷骰前先问去向 —— 门垫/窗台垫子（城里）、
           望远镜（山野）按场合计入；旅行帽（无场合）恒生效。plan 同时供 trip_start 沿用。 */
        const int plan_dest = s_trip_plan_fn ? s_trip_plan_fn(now, st->weather) : -1;
        p_out += loot_fx_out_pp(plan_dest);
        if (p_out < 0)   p_out = 0;
        if (p_out > 100) p_out = 100;
        /* ★★ 2026-10-20 数值重配：出门冷却 2 天 → **1 天**（last_out==0 = 新机免冷却，保留）。
           理由同上：旧口径下"猫不在家"的空白期太长，而空白期里玩家能做的事只有看立牌。 */
        bool day_ok = (today - last_out >= 1) || (last_out == 0);
        bool wx_ok  = out_weather_ok(st->weather);
        bool rolled = ((int)(esp_random() % 100u) < p_out);
        bool can_go = day_ok && wx_ok && rolled;
        if (can_go) {
            int hours = pick_out_hours(persona);   /* ★R26：探险家长途偏好 / 哲学家 −20% */
            /* ★scope 接线：旧钥匙（城里+4）/ 远行靴（山野+6）/ 风筝线轴（海滨+10）按去向计入；
               旧怀表/地图册（无场合）恒生效（上限 48h 在聚合侧收口）。
               远行时 hours 会被 trip_start 的覆盖值顶掉，这里不特判。 */
            hours += loot_fx_trip_h(plan_dest);
            if (hours > 48) hours = 48;
            /* ★★ 2026-10-16 远行：钩子先调 —— 返回 >0 = 套装齐全自动出发（N 天），
               用覆盖值重算返家时刻；返回 0 = 普通短途，行为与旧版一致。 */
            bool is_trip = false;
            if (s_trip_start_fn) {
                const int ov_h = s_trip_start_fn(now, st->weather);
                if (ov_h > 0) { hours = ov_h; is_trip = true; }
            }
            out_ts = (int32_t)now + hours * 3600;
            out_at = (int32_t)now;      /* ★ 记下「出门那一刻」→ 立牌算已出门时长 */
            outside = true;
            /* ★★ 2026-10-27 R91 串门彩蛋：普通出行才可能中途去好友家待一段（远行不串门）。
               窗口规划好就落 NVS（visit_t0 起点 / visit_h 时长，0 = 本趟不串门）——
               main.c 上报 vis=1、loot_system 暂停寄信，都以这两个键为唯一判据。 */
            int32_t visit_t0 = 0, visit_h = 0;
            if (!is_trip && hours >= VISIT_MIN_TRIP_H &&
                (int)(esp_random() % 100u) < VISIT_P_PCT) {
                int hi = hours - 2 * VISIT_EDGE_H;          /* 首尾各留 EDGE_H 小时正常出行 */
                if (hi > VISIT_MAX_H) hi = VISIT_MAX_H;
                if (hi >= VISIT_MIN_H) {
                    const int vh   = VISIT_MIN_H + (int)(esp_random() % (uint32_t)(hi - VISIT_MIN_H + 1));
                    const int span = hours - vh - 2 * VISIT_EDGE_H;
                    const int off  = VISIT_EDGE_H +
                                     ((span > 0) ? (int)(esp_random() % (uint32_t)(span + 1)) : 0);
                    visit_t0 = (int32_t)now + off * 3600;
                    visit_h  = vh;
                    ESP_LOGW(TAG, "★★ 这趟会去串门：出门后第 %d 小时起，待 %d 小时", off, vh);
                }
            }
            nvs_set_i32(h, "visit_t0", visit_t0);
            nvs_set_i32(h, "visit_h",  visit_h);
            ESP_LOGI(TAG, "★ 猫出门了：预计 %d 小时后回来（已出门从那刻起算）", hours);
            fflush(stdout);
        } else {
            /* ★ 不出门的理由**逐条**打出来 —— 下次「怎么又不出门」不用再从源码里猜 */
            ESP_LOGI(TAG, "今天没出门：概率 %d%%（固定·与亲密度无关）· 天气 %d（%s）· 距上次出门 %d 天%s · 掷骰%s",
                     p_out, st->weather, wx_ok ? "可出" : "禁出·雷/大雨/雪",
                     today - last_out, day_ok ? "" : "（<2 天·冷却中）",
                     rolled ? "中" : "未中");
            fflush(stdout);
        }
    }

    /* ★★ 2026-09-23 新增（立牌第二行「猫猫已出门 Nh」的数据来源）：
       必须有「出门时刻」这一笔 —— 总时长是 esp_random() 抽的，**反推不出来**。
       两条兜底缺一不可：
         · 老数据（09-23 之前存的）只有返家时刻、没有出门时刻 → 按「此刻刚出门」处理，
           宁可本轮显示 0h，也不编一个假的已出门时长；
         · 没出门时出门时刻必须归零，否则残值会被当成「出门中」。 */
    if (outside && out_at == 0) {
        out_at = (int32_t)now;
        ESP_LOGW(TAG, "★ 老数据缺出门时刻 → 按当前时刻起算（本轮「已出门」= 0h）");
        fflush(stdout);
    }
    if (!outside) out_at = 0;

    nvs_set_i32(h, "last_day", today);
    nvs_set_i32(h, "last_out", last_out);
    nvs_set_i32(h, "out_ts",   out_ts);
    nvs_set_i32(h, "out_at",   out_at);    /* ★ 2026-09-23 */
    nvs_set_i32(h, "out_until", 0);        /* 旧键清零，别再被 ② 换算一次 */
    nvs_set_i32(h, "inti", intimacy);
    nvs_set_i32(h, "st_inti", intimacy);   /* ★R170 全检：读侧只认 st_inti，迁移必须双写 */
    nvs_commit(h);
    nvs_close(h);

    st->intimacy = intimacy;
    st->out_ts   = (time_t)out_ts;
    st->out_at   = (time_t)out_at;
    st->outside  = outside;
}

/* ---------------------------------------------------------------- 主同步 */

/* ★★ 2026-09-24（唤醒慢修复）：WiFi 连接预算可调（默认 20s 不变，行为兼容）。
   按键唤醒的「出画面后补同步」在调用 ws_sync() 前会把它收紧到 8s。 */
static int s_wifi_budget_ms = 20000;
void ws_set_wifi_budget_ms(int ms)
{
    s_wifi_budget_ms = (ms > 1000) ? ms : 1000;
    ESP_LOGW(TAG, "WiFi 连接预算 → %d ms", s_wifi_budget_ms);
}

/* ★★ 2026-09-26（flash19c）：最近一次 ws_sync 的「wifi 是否连上」。
   为什么必须有它：main.c 的 4 处联网收尾原本只看 st->from_cache —— 它的语义是
   「**从网络拿到过数据**」（wifi 连上 + SNTP 校时成功才置 false），还兼任
   「重试计数器是否清零」的判据。但网络图标的语义是「**wifi 通不通**」：
   wifi 连上了、只是 SNTP/天气接口失败，用户视角就是「我联网了标志怎么还在」。
   现在两件事分开记账：图标/落盘看 ws_last_net_ok()（连通性），
   重试计数器继续看 from_cache（数据是否真拿到）。 */
static bool s_last_net_ok = false;
bool ws_last_net_ok(void) { return s_last_net_ok; }

/* ---------------------------------------------------------------- 自动漫游 */
/* ★★ 2026-10-07（用户需求）：已存 A/B/C 多个 WiFi 时，当前网（A）没信号而别的（B）在场
   → 同步流程里自动转连 B，并把「当前网」切到 B（写 cfg + NVS ssid/pass）。
   列表与落账通过两个钩子与 main.c 分工（见 net_sync.h 注释；耗电口径也在那里）。 */

static ws_roam_list_fn  s_roam_list_fn  = NULL;   /* main.c 喂「已存列表」 */
static ws_roam_commit_fn s_roam_commit_fn = NULL; /* main.c 对齐「当前网」内存标记 */
void ws_set_roam_list_get(ws_roam_list_fn fn)  { s_roam_list_fn = fn; }
void ws_set_roam_commit(ws_roam_commit_fn fn)  { s_roam_commit_fn = fn; }

/* 等到 GOT_IP（或超时/按键）。与 ws_wifi_connect 的等待循环同一口径：
   分片 1s、每片查按键（用户按键优先）；成功 = 收到 IP_EVENT_STA_GOT_IP。 */
static bool wifi_wait_ip(int timeout_ms)
{
    /* ★★ 2026-10-13 任务二 P2-13：清位逻辑已上移到调用方（漫游候选循环，set_config 之前）。
       本函数不再自行清位 —— 否则会重新打开「disconnect 的异步 DISCONNECTED 竞态」窗口。 */
    if (!s_wifi_eg) s_wifi_eg = xEventGroupCreate();
    int waited = 0;
    const int SLICE = 1000;
    while (waited < timeout_ms) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_eg, WIFI_BIT_OK | WIFI_BIT_FAIL, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(SLICE));
        if (bits & WIFI_BIT_OK) return true;
        /* ★★ 2026-10-11（审计修）：FAIL（WIFI_EVENT_STA_DISCONNECTED）= 本条候选
           **确凿失败**（认证被拒 / 关联不上…）。本工程的事件回调只置位、**不重连**，
           驱动此后也不会自己再试 ⇒ OK 永远不会再来，原写法把剩下的预算全数空等
           （每条白耗最多 ~5s；3 条备选最坏 +15s 纯浪费）。立即判负，交给下一条。 */
        if (bits & WIFI_BIT_FAIL) return false;
        waited += SLICE;
        if (ws_key_pressed()) return false;
    }
    return false;
}

/* 漫游本体：扫一次周围热点 → 从「已存列表 ∩ 在场」里按信号强→弱逐个试
   （最多 3 条、每条 6s、信号门限 -80dBm——再弱连上也掉）。
   成功：把该条写进 cfg + ws_save_config 落 NVS（「当前网」自此切过去）+ commit 回调。
   ⚠ 扫描参数与 net_prov.c h_scan 同款（active 60/300ms 全信道 ≈1-2s），改一处必改另一处。 */
#define ROAM_CAND_MAX   3       /* 最多试 3 条备选 */
#define ROAM_RSSI_MIN   (-80)   /* 信号门限：比这弱的连上也站不住 */
#define ROAM_WEAK_RSSI  (-72)   /* ★2026-10-14 R45：当前网低于此值 = 弱网 → 主动漫游转强。
                                   用户实机口径「信号弱且时好时坏」——原口径「连得上就不切」
                                   会死守弱网（时好时坏的网 8s 内常能连上，漫游永不触发）。
                                   可调：-70 更激进（容易切）、-76 更保守（很少切）。 */
#define ROAM_TRY_MS     6000    /* 每条备选的试连预算 */

static bool ws_wifi_roam(ws_config_t *cfg, int min_rssi)
{
    if (!s_roam_list_fn) return false;
    /* ★ 栈上的大家伙（列表 ~0.8KB + 扫描记录 20×~80B ≈ 1.6KB）全 static ——
       与 geocode_once/fetch_weather 的 acc 同理：ws_sync 是「task main」栈最深的一段
       （那里有 4916B 余量的实测记录），漫游叠在连接/HTTP 之前，别再压栈。本函数不重入。 */
    static EXT_RAM_BSS_ATTR char ssids[8][33], passes[8][65];
    int n = s_roam_list_fn(ssids, passes, 8);
    if (n <= 0) {                                    /* 列表没喂 / 空表（如刚上电未 load）→ 不漫游 */
        ESP_LOGI(TAG, "漫游：已存列表为空 → 跳过");
        return false;
    }

    /* ① 扫描一次（⚠ 2026-10-10 修：先断开再扫 —— 用户「放到 WiFiB 环境 12 小时
       一直不自动切换」的真凶：上一步 ws_wifi_connect 超时返回时**没有 disconnect**，
       STA 仍卡在「连接中」状态（驱动还在内部重试关联）。而 ESP-IDF 在连接态调
       esp_wifi_scan_start 会被驱动拒绝（ESP_ERR_WIFI_STATE）⇒ 每次同步的漫游都
       **静默死在扫描这一步**（10-08 日志「CU_6nAv 8s 超时 → 转连失败」即此）。
       手动切换（ws_wifi_switch）能成功，正因为它切换前先断开 —— 这里补齐同一动作，
       把 STA 拉回 idle 再扫。 */
    /* ★R156：只在「没连上」时才断开再扫。原实现无条件 disconnect —— 弱网路径下
       WiFi 明明 3s 就连上了、也拿到 IP，却因为 RSSI<-72 被自己断掉去扫必然不成立的
       备选，扫完回连又失败，整轮判死（实测一轮 35.6s，最后【用缓存】）。
       已关联时 ESP-IDF 允许带连接扫描；若被拒直接 return false，原连接仍在。 */
    {
        wifi_ap_record_t cur;
        if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 60, .max = 300 }, .passive = 0 },
    };
    const int64_t t0 = esp_timer_get_time();
    esp_err_t se = esp_wifi_scan_start(&sc, true);
    if (se != ESP_OK) {
        ESP_LOGW(TAG, "漫游：扫描失败（%s）→ 放弃（下次同步再试）", esp_err_to_name(se));
        fflush(stdout);
        return false;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    static wifi_ap_record_t rec[20];                 /* 同上：static 防主任务栈溢出 */
    uint16_t got = num;
    bool have = (num && esp_wifi_scan_get_ap_records(&got, rec) == ESP_OK);
    esp_wifi_clear_ap_list();                        /* 无条件清（同 h_scan 的教训：不清会拿到旧结果） */
    if (!have) {
        ESP_LOGW(TAG, "漫游：扫描没有结果");
        fflush(stdout);
        return false;
    }
    ESP_LOGI(TAG, "漫游：扫描完成 %lld ms · %u 条热点，与已存 %d 条对表…",
             (long long)((esp_timer_get_time() - t0) / 1000), got, n);
    fflush(stdout);

    /* ② 组候选：在列表里（取其密码）、不是刚失败的那条、信号 ≥ 门限；按信号强→弱 */
    /* ★R149：加 = {{0}} 清零。下面「满了挤掉最弱」那段要读 cand[j].rssi，
       严格按可达性看只有 nc==ROAM_CAND_MAX（0..MAX-1 全填过）才进该分支，逻辑本身没错；
       但 analyzer 无法把 nc 与元素已初始化挂钩，会对 cand[j].rssi 报 CWE-457 未初始化。
       清零零成本、顺手消掉这条噪音告警（万一以后有人改动 nc 语义也不会读到脏值）。 */
    struct { const char *ssid, *pass; int rssi; } cand[ROAM_CAND_MAX] = {{0}};
    int nc = 0;
    for (uint16_t i = 0; i < got; ++i) {
        const char *ap = (const char *)rec[i].ssid;
        if (!ap[0] || strcmp(ap, cfg->ssid) == 0) continue;      /* 刚失败过的不再试 */
        if (rec[i].rssi < ROAM_RSSI_MIN || rec[i].rssi < min_rssi) continue;
        int li = -1;
        for (int j = 0; j < n; ++j)
            if (strcmp(ssids[j], ap) == 0) { li = j; break; }
        if (li < 0) continue;                                    /* 不在已存列表 → 不是我们的网 */
        if (nc < ROAM_CAND_MAX) {
            cand[nc].ssid = ssids[li];
            cand[nc].pass = passes[li];
            cand[nc].rssi = rec[i].rssi;
            nc++;
        } else {                                                 /* 满了：挤掉最弱的 */
            int weakest = 0;
            for (int j = 1; j < ROAM_CAND_MAX; ++j)
                if (cand[j].rssi < cand[weakest].rssi) weakest = j;
            if (rec[i].rssi > cand[weakest].rssi) {
                cand[weakest].ssid = ssids[li];
                cand[weakest].pass = passes[li];
                cand[weakest].rssi = rec[i].rssi;
            }
        }
    }
    if (nc == 0) {
        ESP_LOGI(TAG, "漫游：已存的 %d 条都不在场（或信号太弱）→ 维持原判失败", n);
        fflush(stdout);
        return false;
    }

    /* ③ 逐个试连（强→弱）。⚠ 用 esp_wifi_set_config + connect（不走 ws_wifi_switch：
       那个会先 disconnect + 110ms 等待 + 重发 connect，试连场景不需要这些反复）。 */
    for (int i = 0; i < nc; ++i) {
        ESP_LOGW(TAG, "漫游：试连备选「%s」（信号 %d dBm，%d/%d）…",
                 cand[i].ssid, cand[i].rssi, i + 1, nc);
        fflush(stdout);
        /* ★★ 2026-10-13 任务二 P2-13 修（报告：「FAIL 早退与异步 DISCONNECTED 事件竞态」）：
           事件组清位必须发生在**本候选所有异步事件都落地之后**。
           ★R212 修（全量审计）：原实现把清位放在 set_config 之前、disconnect 之前 ——
           而本候选自己的 `esp_wifi_disconnect()` 会异步投递 DISCONNECTED → 置 FAIL，
           且这 110ms 等待期间**必然已被处理** ⇒ wifi_wait_ip 第一轮就拿到 FAIL、
           立即判本候选失败（漫游实际永远不成功）。现在改为：
             disconnect → 等 110ms（让 DISCONNECTED 落地）→ **再清位** → connect。
           这样等待期间只剩本候选 connect 的真实结果（OK 优先于 FAIL，见 wifi_wait_ip）。 */
        wifi_config_t wc = {0};
        /* ★R207：漫游候选同样用定长 memcpy（32 字节 SSID / 64 字节密码不截位）。 */
        memcpy(wc.sta.ssid, cand[i].ssid, strnlen(cand[i].ssid, sizeof(wc.sta.ssid)));
        const char *cp = cand[i].pass ? cand[i].pass : "";
        memcpy(wc.sta.password, cp, strnlen(cp, sizeof(wc.sta.password)));
        wc.sta.threshold.authmode = (cand[i].pass && cand[i].pass[0])
                                    ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) continue;
        /* ★★ 2026-10-11（审计修）：先断开再连 —— 上一条候选 6s 超时（无 FAIL 事件）时
           驱动可能仍卡在「连接中」内部重试，此时发 esp_wifi_connect 会被驱动拒绝/吞掉，
           下面的 wifi_wait_ip 等的其实是**上一条**的尝试结果。与 ① 扫描前先断开
           （2026-10-10 用户 12 小时实证那条）同一课：试连前把 STA 拉回 idle。
           （第一条候选前 STA 本就空闲，disconnect 是无害空操作。） */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(110));                  /* 同 ws_wifi_switch 的断开等待 */
        /* ★R212：断开事件已落地，此刻清位才是「本候选 connect 的起点」。 */
        if (s_wifi_eg) xEventGroupClearBits(s_wifi_eg, WIFI_BIT_OK | WIFI_BIT_FAIL);
        esp_wifi_connect();
        if (wifi_wait_ip(ROAM_TRY_MS)) {
            /* 成功 → 这条成为「当前网」：写 cfg + 落 NVS（ws_save_config 是程序内
               改配置的标准入口；lat/lon/city/tz 原样保留——城市是"住哪"，与连哪个网无关） */
            strlcpy(cfg->ssid, cand[i].ssid, sizeof(cfg->ssid));
            strlcpy(cfg->pass, cand[i].pass ? cand[i].pass : "", sizeof(cfg->pass));
            /* ★2026-10-10：只是"换当前网"，tz 不是本次算出来的 → tz_known=false，
               别把可能的 +8 兜底值晋升成可信值（见 ws_save_config 的注释）。 */
            ws_save_config(cfg, false);
            if (s_roam_commit_fn) s_roam_commit_fn(cfg->ssid);
            ESP_LOGW(TAG, "★ 自动漫游：已转连「%s」并设为当前网（原网恢复信号后可再手动切换）",
                     cfg->ssid);
            fflush(stdout);
            return true;
        }
    }
    ESP_LOGW(TAG, "漫游：%d 条备选都连不上 → 维持失败", nc);
    fflush(stdout);
    return false;
}

/* ws_sync 的连接入口：当前条先给足重试机会（12~20s，2.5s 一次）→ 真连不上才交漫游。
   ★R156（用户 19:19 实机报「同步很慢且要失败很多次才成功一次」）：原实现无条件把
   budget 砍到 8s，而 ws_wifi_connect 的首次定时重试恰好设在 8000ms ⇒ 8s 预算内
   **一次重连都发不出去**；实测 6 次冷启动 5 次判失败，日志全是
   `sta is connecting, return error` 后干等 8s。弱网 ASB 一次关联抖动不该等于整轮失败。 */
static bool ws_sync_connect(ws_config_t *cfg, int budget_ms)
{
    int first = budget_ms;
    if (first < 12000) first = 12000;
    if (first > 20000) first = 20000;
    if (ws_wifi_connect(cfg, first)) return true;
    ESP_LOGW(TAG, "当前网「%s」%d ms 没连上 → 尝试自动转连已存的其它 WiFi", cfg->ssid, first);
    fflush(stdout);
    return ws_wifi_roam(cfg, 0);
}


void ws_sync(ws_config_t *cfg, ws_state_t *st)
{
    /* ★★ 2026-09-22：调用方可能已经把墙钟推到「比缓存更新」的时刻
       （深睡唤醒后 now_local = 入睡时刻 + RTC 实测经过时间，见 main.c）。
       旧写法直接 ws_load_state(st) 会把 now_local 覆盖成 NVS 里那份更旧的缓存 ——
       于是**每同步一次就把时钟往回拨最多一个待机周期**，一天同步 8 次就等于
       偷偷倒退了几个小时（用户报「12:37 还是清晨」的另一个成因）。
       现在先记下调用方的时间，装完缓存后取「更晚的那个」。 */
    time_t caller_now = st->now_local;

    /* 先装缓存（保证任何失败路径下 st 都是可用的） */
    ws_load_state(st);

    if (caller_now > st->now_local) {
        st->now_local = caller_now;
        /* ★★ 2026-10-20 修：这里原本**无条件** `st->time_ok = true`，等于把
           ws_load_state() 那条「出厂兜底窗口 [1767225600, +86400) 一律判不可信」的
           双保险（net_sync.c:1155，2026-10-13 为 P1-5 加的）**旁路掉**。
           事故链：冷启动/没网时 main.c 先把 now_local 设成出厂兜底 1767225600+12h
           再调 ws_sync ⇒ 假时间被判可信 ⇒ step_life 拿它结算：写 last_day，
           且 last_out==0 使 day_ok 成立 ⇒ **假时间出门** ⇒ 真时间一同步到立刻判回家，
           触发一次**凭空的归来掉落**；而 1850 那条「假时间不落盘」也被跳过。
           ⇒ 改成与 load 侧**同口径**判窗口：兜底窗口内一律不可信。 */
        st->time_ok   = (caller_now > 1700000000) &&
                        !(caller_now >= 1767225600 && caller_now < 1767225600 + 86400);
        st->from_cache = true;     /* 但毕竟不是网络来的，别把重试计数清零 */
    }

    bool net_ok = ws_sync_connect(cfg, s_wifi_budget_ms);   /* ★2026-10-07：含自动转连 */
    bool time_ok = false;
    s_last_net_ok = net_ok;   /* ★flash19c：图标看「连通性」；数据是否真拿到仍由 from_cache 表达 */

    /* ★★ 2026-10-14 R45（用户实机：「信号弱且时好时坏」+「漫游没成立」）：
       连上之后**测一把信号**——低于 ROAM_WEAK_RSSI 就是弱网，先试转连更强的已存备选，
       转成后本轮 HTTP 全部走新网；没转成（备选都不在场/试连失败）→ **回连原网**再继续
       同步（弱连接也比断开强，原网还能抢救本轮 HTTP）。原口径「连得上就不切」下，
       时好时坏的网 8s 预算内常能连上 ⇒ 漫游永不触发，设备死守弱网——本段补齐该面。 */
    if (net_ok) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi < ROAM_WEAK_RSSI) {
            /* ★R186：弱网自动漫游加一道**净增益**门槛 —— 备选必须比当前强 8 dB 才值得
               断开重连。原实现只要备选 ≥-80 就切，实测本机 -76 dBm 时会为了 -74 的
               备选白断一次、再重连，光这一趟就多花 3~5s（用户报「连 WiFi 有点久」）。 */
            const int gain_min = ap.rssi + 8;
            ESP_LOGW(TAG, "★ 当前网「%s」信号弱（%d dBm < %d dBm）→ 找 +8dB 以上的备选",
                     cfg->ssid, ap.rssi, ROAM_WEAK_RSSI);
            fflush(stdout);
            if (!ws_wifi_roam(cfg, gain_min)) {
                ESP_LOGW(TAG, "漫游未转成（备选不在场或试连失败）→ 回连原网「%s」继续本轮",
                         cfg->ssid);
                fflush(stdout);
                /* ★R156：回连结果必须回填 net_ok。原写法丢弃返回值，接着在
                   **断网状态**下跑天气 12s + SNTP 15s，全超时后判失败。
                   若原连接还在（R156 漫游扫描不断网），本调用命中
                   「已连目标且持有效 IP」早退，零耗时返回 true。 */
                net_ok = ws_wifi_connect(cfg, 8000);
                s_last_net_ok = net_ok;
                if (!net_ok) ESP_LOGW(TAG, "★ 回连原网也失败 → 本轮跳过所有 HTTP");
            }
        }
    }

    if (net_ok) {
        /* ① 城市名 → 经纬度（只在城市变了时才真的发请求） */
        ws_resolve_city(cfg);

        /* ② 天气。★ 必须排在「算本地时间」之前 —— 因为 `timezone=auto` 会顺手
              把时区校准掉；顺序反了第一次同步就会用错时区算出错的时段和月相。 */
        int w = st->weather, t = st->temp_c;
        if (fetch_weather(cfg, &w, &t)) {
            st->weather = w;
            st->temp_c = t;
            /* ★R156：成败判据不再绑死 SNTP。原来 from_cache 只有校时成功才清，
               而 SNTP 走 UDP 123、弱网丢包常见 ⇒ WiFi 通了、天气也拿到了仍显示
               「同步失败」（用户「失败很多次才成功一次」的主因）。天气是真实网络
               数据，拿到即算本轮同步成功；校时失败只影响时间精度，继续用缓存时间。 */
            st->from_cache = false;
        }

        /* ③ 时间（用刚校准过的时区）★R156：15s→7s，弱网下不值得为 UDP 校时干等 */
        time_ok = fetch_utc_time(7000);
        if (time_ok) {
            time_t utc = time(NULL);
            /* ★2026-10-07：去掉 `? : 8*3600` —— UTC±0 用户的 tz 就是 0，被它一判又变 +8 */
            const time_t old_now = st->now_local;
            st->now_local = utc + cfg->tz_offset_sec;
            /* ★R125：换时区/校时回拨（>1h）→ 出门/串门/远行绝对时刻按同一 delta 平移 */
            if (st->outside && (st->now_local - old_now > 3600 || st->now_local - old_now < -3600))
                ws_shift_abs_times(st->now_local - old_now, st);
            st->time_ok = true;
            setenv("TZ", "UTC0", 1);          /* 保持内部用 UTC，自己加偏移 */
            tzset();
            st->from_cache = false;
        }
    }

    /* ③ 时间兜底：用缓存的时间 + 开机的单调时钟走时。
          ★ 这里只保证「链路通」，精确到小时足够（时段是小时级划分）。
          ★flash24：**必须把「用了出厂兜底」这件事标出来** —— 兜底时间（2026-01-01）虽然
            数值上 >1.7e9 会被判成 time_ok=true，但它是假的：拿它进 step_life 会把
            last_day 写成 20454，等真时间一到 elapsed 暴跳 → 亲密度一次砸到地板（见 step_life）。 */
    bool time_fallback = false;
    if (!st->time_ok) {
        st->now_local = time(NULL);            /* 未同步 = 1970 + 开机秒数 */
        if (st->now_local < 1700000000) {
            /* 完全没有时间基准：用出厂兜底（2026-01-01 12:00 本地）
               ★★ 2026-10-07 再修（上次方向修反了）：now_local 是「本地伪 epoch」= UTC + tz，
                  gmtime_r 按原值分解**就是挂钟** → 显示 12:00 ⇔ 伪 epoch = UTC 午夜 + 12h，
                  **不加也不减 tz**。
                  · 原始版 `+ tz + 12h`：+8 显示 20:00（深夜）；
                  · 2026-10-04 改成 `- tz + 12h`：+8 显示 **04:00**，还是深夜 —— 方向反了；
                  · main.c touch_demo_run 的同款兜底已同步改（两处必同源）。 */
            st->now_local = 1767225600 + 12 * 3600;
            time_fallback = true;
            ESP_LOGW(TAG, "无时间基准，用出厂兜底时间");
        }
    }

    /* ④ 月相：纯计算 */
    time_t utc_for_moon = st->now_local - cfg->tz_offset_sec;
    st->moon_phase = ws_moon_phase(utc_for_moon, &st->moon_age_pct);

    /* ⑤ 时段 */
    st->period = ws_period_of(st->now_local);

    /* ⑥ 窗外风景：★ 直接取配网时用户选的那一项，**不由城市反推**。
          城市只管天气（上面 ①/②），风景只管画面观感 —— 两者完全解耦。 */
    st->land = land_clamp(cfg->land);

    /* ⑦ 出门状态机（按天结算）★flash24：把「时间是否可信」透传进去（兜底时间不许结算） */
    step_life(st, cfg, !time_fallback);

    /* ⑧ 存缓存，供下次断网用
     *   ★★ 2026-10-13 任务二 P1-5 修：**兜底时间不许落盘**。原来无条件 ws_save_state，
     *      出厂兜底（2026-01-01 12:00 = 1767225600+43200）数值上 >1.7e9，下次
     *      ws_load_state 会把它判成 time_ok=true「有效时间」复活 → step_life 拿假时间
     *      结算：last_day 被写成 20454，真时间一到 elapsed≈126 天被夹到 3 → 亲密度
     *      一次性砸地；出了门的猫 out_ts 落在假坐标系，真时间一到立即判「回家」。
     *      ⇒ 本轮用过兜底就跳过存盘（保留其余缓存字段也不差这一刻：下次真同步会覆写）。
     *      双保险在 load 侧：兜底窗口 [1767225600, +86400) 一律判不可信（见 ws_load_state）。 */
    if (!time_fallback) ws_save_state(st);
    else ESP_LOGW(TAG, "★ 本轮用过出厂兜底时间 → 跳过存盘（防假时间冒充有效时间复活）");

    {
        /* ★ 2026-09-22：把「设备以为现在几点」明确打出来 ——
           用户报「12:37 还是清晨」时，一眼就能分辨是「时间走偏」还是「渲染没刷」。 */
        struct tm tml;
        gmtime_r(&st->now_local, &tml);      /* now_local 已是本地伪时间，按 UTC 读才对得上表 */
        ESP_LOGI(TAG, "★ 设备本地时间 = %04d-%02d-%02d %02d:%02d:%02d（%s）",
                 tml.tm_year + 1900, tml.tm_mon + 1, tml.tm_mday,
                 tml.tm_hour, tml.tm_min, tml.tm_sec,
                 st->time_ok ? "时间有效" : "★时间无效·正在用兜底");
    }
    ESP_LOGI(TAG, "★ 同步结果：时段=%d 天气=%d 月相=%d(%d%%) 地标=%d 出门=%d 亲密度=%d %s",
             st->period, st->weather, st->moon_phase, st->moon_age_pct,
             st->land, st->outside, st->intimacy,
             st->from_cache ? "【用缓存】" : "");
    /* ★ 2026-09-21：栈余量自检（★2026-11-12 修正口径）：ws_sync() 启动时跑在
       task main 上，也可能跑在后台 ws_sync 任务（栈 12,288 B）里；
       uxTaskGetStackHighWaterMark(NULL) 报的是**当前任务**余量，不恒为主任务。
       所以文案不写「主任务」，也不提示去抬 CONFIG_ESP_MAIN_TASK_STACK_SIZE。 */
    ESP_LOGI(TAG, "★ 当前任务栈余量 = %u 字节（<512 需警惕）",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    /* ★★ 2026-10-11 R26：同步收尾钩子（main.c 在这里做「空箱保底羽毛」掷骰）。
       net_ok = 本次 WiFi 是否连通（ws_last_net_ok；SNTP/天气失败但连通也算「同步过」）。 */
    if (s_sync_done_fn) s_sync_done_fn(s_last_net_ok);
    fflush(stdout);
}

/* ══ 2026-10-14 R46 · 后台同步任务（方案 B，用户拍板）══════════════════════
 * 目的（用户实机反馈）：同步链阻塞主循环，期间触摸不轮询 → 屏幕无响应 30s+。
 * 把同步链（连接 + 漫游 + HTTP 天气/对时）放进独立任务，主循环继续响应输入。
 *
 * 三点分工契约（本拆分成立的前提，改同步链时勿破坏）：
 *   1. epd/fb 只允许主循环碰 —— sync 任务**只写数据**（st / NVS），绝不渲染；
 *      「同步拿到新数据要不要补刷」的决策留在主循环（render_key 比对，幂等铁律）。
 *   2. 后台同步期间主循环**不得写回 g_app.st**（ws_sync_async_busy() 守卫）——
 *      否则主循环的 fresh 时间累加会把 sync 刚写回的网络真值整结构覆盖掉。
 *      时间数学：sync 写回的是「当下」的服务器真值（天然含投递以来的流逝），
 *      主循环 pending 的本地累加被真值取代即弃 —— 收尾 clock_rebase() 一步闭环。
 *   3. st 字段竞态（主循环抚摸 += vs step_life 结算、64 位 now_local 读撕裂）：
 *      均为体验级字段（最坏丢 1 点亲密度/错显一帧），刻意不加锁；账本类计数
 *      （trip_total/feed_cnt 等）各走独立 NVS 键，NVS API 内部自锁，无交叉丢失。
 * 阻塞模式保留：启动/配网流程（任务创建之前的四五个调用点）仍直接调 ws_sync() ——
 *   那些画面本来就在「正在连接」过渡屏上，阻塞无感，且天然无并发。
 * 单飞（single-flight）：同一时刻至多一个 ws_sync 在跑（任务单实例 + busy 标志），
 *   主循环重复投递直接忽略（上一轮还没跑完，再排队只是白等）。 */
static TaskHandle_t s_sync_task   = NULL;
static volatile bool s_async_busy = false;   /* sync 链正在后台跑（主循环写回守卫读这个） */
static volatile bool s_async_done = false;   /* 有一轮后台同步完成待主循环收尾 */
static ws_config_t *s_async_cfg   = NULL;
static ws_state_t  *s_async_st    = NULL;

static void sync_task_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     /* 等主循环投递（计数清零式） */
        s_async_busy = true;
        s_async_done = false;
        ws_sync(s_async_cfg, s_async_st);
        s_async_busy = false;
        s_async_done = true;                          /* 收尾（rebase/bookkeep/补刷）在主循环 */
    }
}

void ws_sync_task_start(void)
{
    if (s_sync_task) return;                          /* 幂等 */
    if (xTaskCreate(sync_task_main, "ws_sync", 12288, NULL, 4, &s_sync_task)  /* ★R170 全检：8K→12K */ != pdPASS) {
        ESP_LOGE(TAG, "★ 后台同步任务创建失败（RAM 不足？）→ 主循环自动退回阻塞模式");
        fflush(stdout);
    }
}

/* 投递一次后台同步（非阻塞）。任务没起（启动早期）→ 降级为原地阻塞，行为与 R45 全同。 */
void ws_sync_begin(ws_config_t *cfg, ws_state_t *st)
{
    if (!s_sync_task) { ws_sync(cfg, st); return; }
    if (s_async_busy) {                               /* 单飞：上一轮没完不再排队 */
        ESP_LOGW(TAG, "后台同步仍在跑 → 本次投递忽略");
        fflush(stdout);
        return;
    }
    s_async_cfg = cfg;
    s_async_st  = st;
    xTaskNotifyGive(s_sync_task);
}

bool ws_sync_async_busy(void) { return s_async_busy; }
bool ws_sync_async_done(void) { return s_async_done; }
void ws_sync_async_consume(void) { s_async_done = false; }

/* ---------------------------------------------------------------- 配网 */
/* 实现在 net_prov.c，这里只转发（保持单文件职责清晰） */
