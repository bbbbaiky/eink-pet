/*
 * net_prov.c — 热点配网（类似小智）
 * ===========================================================================
 * 流程（用户拍板：做热点配网）：
 *   ① 开机读 NVS 没有 WiFi 配置 → 自建热点
 *      热点名：PetCat-XXXX（XXXX = MAC 后两字节，避免多台设备冲突）
 *      无密码（或固定密码 12345678），手机连上即可
 *   ② 起 HTTP 服务器（80 端口）+ DNS 劫持（把任意域名解析到自己），
 *      这样手机连上热点后会自动弹出配网页（Captive Portal）。
 *   ③ 网页上填：WiFi 名 / 密码 / 窗外风景 / 居住城市（★城市是「国家 → 城市」下拉：
 *      默认中国走省→市两级；海外 139 国直接选城市，提交 "CC:CityId" 键）
 *   ④ 提交 → 写 NVS → 重启 → 下次开机直接连
 *
 * ★ 为什么要有 DNS 劫持：iOS/Android 连上热点会先探测特定 URL 判断
 *   "这个网能不能上网"，探测失败就弹配网页。不劫持的话用户得手动
 *   在浏览器里敲 192.168.4.1，体验差一截。
 * ===========================================================================
 */
#include "net_sync.h"
#include "assets.h"
#include "main_api.h"
#include "esp_heap_caps.h"   /* webui PSRAM 装载 */
#include "prov_city_data.h"   /* ★R217 冻结资产：省→市下拉数据，见本文件头注释 */
#include "prov_world_data.h"  /* ★ 2026-10-14 自动生成的海外「国家→市」下拉数据（PROV_WORLD_JSON），见 scripts/gen_world_cities.py */
#include "i18n.h"             /* ★ 2026-10-13 多语言：g_lang / LANG_EN —— 配网页按设备语言出中/英两套 */
#include "friend.h"           /* ★R92：添加/修改朋友门户要给网页预填好友的码与名 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>            /* ★ socket/bind 失败时要打 errno 出来 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <driver/gpio.h>      /* ★ 配网页「长摁 BOOT 跳过」要直接读按键电平，见 prov_boot_down() */
#include <esp_http_server.h>
#include <esp_timer.h>        /* ★ 2026-09-29：h_scan 里量扫描耗时（诊断"扫描很慢"） */
#include <nvs.h>
#include <nvs_flash.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

/* ★ 2026-10-12 取名功能：net_prov.c 调用 main.c 提供的 UI / 校验钩子。
   ★R150：原三行手写 extern 已删 —— 本文件第 21 行 include 的 main_api.h 才是真源
   （pet_draw_naming_wait 在 490 行、pet_name_valid / pet_draw_naming_confirm 在 569/570 行）。*/

/* TAG 用 main_api.h 的 extern（pet_epaper 前缀） */

/* ══ web 资源仓（重构 2e）：10 段 HTML 从 app rodata 搬 assets AID_WEBUI，PSRAM 常驻 ══
 * 省 app flash 52.6KB。整仓一次读入 PSRAM（最大单段 22KB，PSRAM 8MB 充裕）；
 * 访问器返回指针+长度（原 strlen() 语义）。失败时回退为 404 空串（浏览器侧可见）。 */
enum { WEBUI_POST_PAGE = 0, WEBUI_UPG_PAGE, WEBUI_NAME_ZH, WEBUI_NAME_EN, WEBUI_NAME_JA,
       WEBUI_NAME_ZHT, WEBUI_API_ZH, WEBUI_API_EN, WEBUI_API_OK_ZH, WEBUI_API_OK_EN, WEBUI_N };
static uint8_t *s_webui    = NULL;
static bool     s_webui_bad = false;   /* ★R95：载入已判失败 → 不再每个请求重读 52KB flash */
static uint32_t s_webui_off[WEBUI_N];
static uint32_t s_webui_len[WEBUI_N];
static const char *webui_page(int idx, int *out_len)
{
    if (!s_webui && !s_webui_bad) {
        uint32_t n = 0;
        bool ok = false;
        if (!assets_region_read(AID_WEBUI, 0, &n, 4) || n != WEBUI_N) {
            ESP_LOGE(TAG, "★ webui.bin 目录异常（n=%u，期望 %d）", (unsigned)n, WEBUI_N);
        } else {
            uint32_t hdr[WEBUI_N * 2];
            uint32_t total = 4 + WEBUI_N * 8;
            if (assets_region_read(AID_WEBUI, 4, hdr, sizeof(hdr))) {
                for (int i = 0; i < WEBUI_N; ++i) {
                    s_webui_off[i] = hdr[i * 2];
                    s_webui_len[i] = hdr[i * 2 + 1];
                    /* ★★ R95（2026-10-27 用户实拍「点取名跳一整页英文+乱码」的真根因）：
                       这里原来写的是 `off + len + 4`（想给末尾 NUL 留余量）。可
                       pack_assets.py 写进分区目录的是**精确 len(data)、不做填充**，
                       webui.bin 末页正好顶到区域末尾 ⇒ 算出 52574 > 区域实长 52570
                       ⇒ assets_region_read() 的 `rel + n > len` 边界检查返回 false
                       ⇒ 读入失败、s_webui = NULL，而 s_webui_len[] 在失败前**已填好**
                       ⇒ h_name() 拿 "" 当指针、拿 4770 当长度，把 app 区 .rodata
                       （mbedtls 证书 OID 表：rsaEncryption / id-ecPublicKey / …）
                       当网页推给手机 = 那张乱码页。
                       ⚠ 各页数据本身**已把自己末尾的 NUL 含在 len 里**（打包时逐字节拷贝），
                         所以不需要任何额外余量；再加就必然越界。**别把这 +4 加回来。** */
                    uint32_t end = s_webui_off[i] + s_webui_len[i];
                    if (end > total) total = end;
                }
                s_webui = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
                if (!s_webui) ESP_LOGE(TAG, "★ webui PSRAM 分配失败（%u B）", (unsigned)total);
                else if (!assets_region_read(AID_WEBUI, 0, s_webui, total)) {
                    heap_caps_free(s_webui); s_webui = NULL;
                    ESP_LOGE(TAG, "★ webui.bin 读入失败（%u B）", (unsigned)total);
                } else {
                    ok = true;
                    ESP_LOGI(TAG, "★ webui.bin 载入 PSRAM：%u B × %d 页", (unsigned)total, WEBUI_N);
                }
            }
        }
        if (!ok) {
            s_webui_bad = true;
            memset(s_webui_len, 0, sizeof(s_webui_len));   /* ★ 关键：别留"真长度 + 空指针" */
        }
    }
    if (out_len) *out_len = 0;
    if (!s_webui || idx < 0 || idx >= WEBUI_N) return "";
    if (out_len) *out_len = (int)s_webui_len[idx];
    return (const char *)s_webui + s_webui_off[idx];
}

#define PROV_SSID_PREFIX "PetCat-"

/* ★ 2026-10-14 R44：原 PREV_TOKEN("@@PREV@@")/PGL_TOKEN("@@PGL@@") 运行时占位宏已退役
   —— 模板参数化后（PROV_SEGS 段表）改为 INJ_PREV/INJ_PGL 注入位直发，不再需要
   整页 strstr 搜索替换。海外城市数据标签语言的说明并入段表区块头注释。 */

/* ★ 2026-09-19 用户订正（本文件第 3 次改口径，以这版为准）：
   「居住城市做一个选择，不要让用户填写」→ 城市改成**下拉联动**，不让用户打字。
   ★ 2026-10-14 海外城市国际化：改成**国家 → 城市**：
     · 国家下拉（默认中国）：中国 + 147 个海外国家（四语国名，PGL 令牌按设备语言出）；
     · 中国：保持「省 → 市」两级（PROV_CITY_JSON，中文名键，天气离线查表按中文名）；
     · 海外：国家 → 城市直接一级（PROV_WORLD_JSON，每国 ≤10 个主要城市，不做州级——
       天气本身是城市级网格精度，多一级州只是白多一步，项目既有口径）；
     · 提交值：中国 = 省+市中文名拼接（老格式，存量设备零迁移）；
       海外 = "CC:CityId"（如 US:LosAngeles）→ 固件 wc_find 查表拿经纬度/三语名/国家码。
   城市 → 经纬度 不联网地理编码（Open-Meteo 实测不认中文名，全返回 null），
   改由固件**离线查表**，见 net_sync.c 的 ws_resolve_city + city_coords.h / world_cities.h。 */

static volatile bool s_submitted = false;
/* ★★ 2026-09-30 新增：配网结果状态机（给手机上的页面轮询用）。
   ── 来由：spec §4.4d 要求「**只有设备真的连上**才进成功页」，
      而旧实现是「表单里有 ssid 就 return true」——密码填错也报「已保存」。
      现在 ws_provision 提交后会**同步试连**一次，结果写进这里；
      手机页面轮询 /status 就能知道到底成没成（而不是只看"已保存"）。
   0=未提交/等待 · 1=已提交，正在连接 · 2=连接成功 · 3=连接失败（可重填） */
static volatile int  s_prov_state = 0;
/* ★ 2026-09-30：提交代次。手机每 POST 一次 /save 就 +1 ——
   ws_provision 靠它区分「这次失败是我刚试的那条」还是「用户已经重填了新的」（见其注释③）。 */
static volatile int  s_submit_gen = 0;
static char s_got_ssid[33], s_got_pass[65], s_got_city[64];
static int  s_got_land = LAND_PEARL;      /* 窗外风景：0=富士山 1=东方明珠 2=星球 */

/* ★ 2026-10-12 取名子流程共享状态 */
static volatile bool    s_name_saved = false;   /* POST /namesave 通过校验 */
static volatile bool    s_name_skip  = false;   /* ★ 2026-10-12：GET /nameskip —— 手机点「暂不取名」主动退出 */
static char             s_name_buf[16];          /* 手机提交的合法名字 */
static volatile int64_t s_name_act_ms = 0;       /* 最近一次 HTTP 请求时刻（心跳/离热点判定）*/
/* ★2026-10-26 改名卡：独立取名门户（不经过配网流程）——h_namesave 落盘后由看门任务收热点。 */
static bool             s_name_standalone = false;

/* ---------------------------------------------------------------- 配网页面 */
/* ★★ 2026-10-14 R44 多语言：配网页从「四套整页模板」重构为 **共享骨架段 + 四语文案段表**
   （PROV_SEGS，见下方生成区块；由 scripts/_tools/_prov_gen.py 自动生成，勿手改）。
   动因：四套整页各自把 PROV_CITY_JSON + PROV_WORLD_JSON（≈58KB 城市数据）编译期拼接进
   模板 → 同一份城市数据在 rodata 里复制 4 份（≈232KB 浪费）；改参数化后 rodata 只剩一份，
   bin 缩 ≈200KB。h_root() 按段表 chunked 逐段直发（INJ_CITY/INJ_WORLD 在此刻才拼进流）。
   ── 文案差异收敛在 20 个 INJ_TXT 位（P_TXT 四语列 zh/en/ja/zht），结构与原四套逐行对应；
      历史上「为什么不用运行时查表替换」的顾虑（JS 动态串粒度）在生成器方案下不再成立：
      文案位由四套整页 diff 自动提取，粒度与原文案一致，且生成后有**逐字节拼装验证**。
   ── 维护约定：改可见文案 = 改 INJ_TXT 位四语列（四列都要改）；改结构/样式 = 改骨架段
      并保证四语骨架仍逐字相同（或重跑生成器，它会再做一次拼装等价验证）。
   ── 中国城市下拉（省/市）**恒中文**：net_sync 的 ws_resolve_city + city_coords.h 按中文名
      离线查经纬度，换英文名天气同步必坏 —— 功能约束，非漏翻。海外部分按 INJ_PGL 出
      zh/en/ja/zht 四语标签（PROV_WORLD_JSON 内嵌四列，JS 按 PGL 取列）。
   ── ⚠ 默认名 lang_default_name 仍**逐语言判等**（日=ミオ/英=Mio/中=猫猫/繁=貓貓），
      别改成「!= LANG_ZH 就用英文名」的写法，否则日文态会拿到英文默认名。 */
/* ★ 2026-09-19 重写：四项独立字段。
   关键订正（用户原话）：「这里面其实是有俩，一个是窗外风景…还有一个是城市，
   这是使用者居住的城市，精确到区，这是用来同步天气的」——
   所以风景是**用户自己挑的画面元素**（下拉，二选一），城市是**查天气用的地址**，
   两者互不推导。 */
/* ═══════ 配网页 · 共享骨架 + 四语文案段表（R44 生成，勿手改） ═══════
 * 由 scripts/_tools/_prov_gen.py 从四份整页模板 PROV_HTML_ZH/EN/JA/ZHT 自动生成。
 * 为什么重写：四份整页各自把 PROV_CITY_JSON + PROV_WORLD_JSON（≈58KB 城市数据）
 *   编译期拼接进模板 → 同一份城市数据在 rodata 里复制了 4 份（≈232KB 浪费）。
 * 新结构：① 四语逐字相同的骨架行只存 1 份（PROV_SEGS[].shell）；
 *         ② 可见文案差异 → INJ_TXT 位（seg.txt 按 g_lang 取列，zh/en/ja/zht）；
 *         ③ 城市 JSON → INJ_CITY/INJ_WORLD 位，发送时才拼进流，rodata 仅 1 份；
 *         ④ @@PREV@@/@@PGL@@ 令牌 → INJ_PREV/INJ_PGL 注入位（省 strstr 搜索）。
 * 发送 = httpd chunked 逐段直发（h_root），全程零整页堆缓冲。
 * ★ 维护：改配网页文案 = 改 P_TXT 对应四语列；改结构/样式 = 改骨架段并保证
 *   四语骨架仍逐字相同，或重跑生成器。 */

typedef enum {
    INJ_NONE = 0,   /* 纯骨架段，无注入 */
    INJ_TXT,        /* 四语文案位（txt[] 按 g_lang 取列） */
    INJ_CITY,       /* 中国省市 JSON（PROV_CITY_JSON，rodata 单份共用） */
    INJ_WORLD,      /* 海外国家城市 JSON（PROV_WORLD_JSON，rodata 单份共用） */
    INJ_PREV,       /* 上次填参 JS（h_root 运行时 build_prev_js 生成；malloc 失败注 null） */
    INJ_PGL,        /* PGL 语言字面量（'zh'/'en'/'ja'/'zht'） */
} prov_inj_t;

typedef struct {
    const char *shell;        /* 本段骨架（"" = 无，纯注入位） */
    prov_inj_t  inj;          /* 段后注入什么 */
    const char *txt[LANG_N];  /* INJ_TXT 四语文案（列序 = lang_t） */
} prov_seg_t;

static const prov_seg_t PROV_SEGS[] = {
    /*0*/ { "", INJ_TXT, { "<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>", "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>", "<!DOCTYPE html><html lang='ja'><head><meta charset='utf-8'>", "<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='utf-8'>" } },
    /*1*/ { "<meta name='viewport' content='width=device-width,initial-scale=1'>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*2*/ { "", INJ_TXT, { "<title>墨水屏猫 · 配网</title><style>", "<title>Mio · WiFi Setup</title><style>", "<title>ミオ · WiFi 設定</title><style>", "<title>貓貓 · Wi-Fi 設定</title><style>" } },
    /*3*/ { "*{box-sizing:border-box}body{font-family:-apple-system,'PingFang SC',sans-serif;margin:0;padding:24px 18px;background:#f5f4f0;color:#2c2c2a}h1{font-size:20px;font-weight:500;margin:0 0 6px}p.hint{font-size:13px;color:#5f5e5a;margin:0 0 20px;line-height:1.6}.card{background:#fff;border:1px solid #d3d1c7;border-radius:12px;padding:18px;max-width:430px}label{display:block;font-size:13px;color:#444441;margin:14px 0 6px}input,select{width:100%;padding:11px 12px;font-size:15px;border:1px solid #b4b2a9;border-radius:8px;background:#fff;color:#2c2c2a}select{appearance:none;-webkit-appearance:none;padding-right:36px;background-image:url(\"data:image/svg+xml;charset=utf-8,%3Csvg xmlns='http://www.w3.org/2000/svg' width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='%23888780' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M6 9.5 L12 15 L18 9.5'/%3E%3C/svg%3E\");background-repeat:no-repeat;background-position:right 10px center;background-size:20px 20px}.two>div{min-width:0}.two{display:flex;gap:10px}.two>div{flex:1}#cc{margin-bottom:10px}.eye-wrap{position:relative}#passField{padding-right:44px}.eye-btn{position:absolute;right:4px;top:50%;transform:translateY(-50%);width:36px;height:36px;margin:0;padding:0;display:flex;align-items:center;justify-content:center;background:transparent;border:0;border-radius:8px;color:#888780;cursor:pointer;-webkit-tap-highlight-color:transparent}.eye-btn:active{background:#f0eee8}button{width:100%;margin-top:22px;padding:13px;font-size:16px;border:0;border-radius:8px;background:#534ab7;color:#fff;font-weight:500}button:active{background:#3c3489}.rowlab{display:flex;justify-content:space-between;align-items:baseline;gap:10px}.rowlab>span{font-weight:400;color:#888780;text-align:right;flex:0 1 auto;min-width:0}.note{font-size:12px;color:#888780;margin-top:10px;line-height:1.6}hr{border:0;border-top:1px solid #e6e4dc;margin:20px 0 4px}</style></head><body>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*4*/ { "", INJ_TXT, { "<h1>墨水屏猫 · 配网</h1><p class='hint'>填好下面四项，设备就会去连你家的 WiFi，并按你选的城市同步真实的时间、天气和月相。</p>", "<h1>Mio · WiFi Setup</h1><p class='hint'>Fill in the four items below and the device will join your WiFi, then sync real time, weather and moon phase for the city you pick.</p>", "<h1>ミオ · WiFi 設定</h1><p class='hint'>以下の4項目を入力すると、デバイスはあなたのWiFiに接続し、選んだ都市の実際の時刻・天気・月の満ち欠けを同期します。</p>", "<h1>貓貓 · Wi-Fi 設定</h1><p class='hint'>填好下面四項，裝置就會連上你家的 Wi-Fi，並按你選的城市同步真實的時間、天氣和月相。</p>" } },
    /*5*/ { "<form class='card' method='POST' action='/save'>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*6*/ { "", INJ_TXT, { "<label class='rowlab'>1 · WiFi 名称（SSID）", "<label class='rowlab'>1 · WiFi name (SSID)", "<label class='rowlab'>1 · WiFi 名（SSID）", "<label class='rowlab'>1 · Wi-Fi 名稱（SSID）" } },
    /*7*/ { "<span id='ss'></span></label><div class='two'><div><select id='ssidSel'></select></div><div style='flex:0 0 78px'><button type='button' id='btnScan'", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*8*/ { "", INJ_TXT, { " style='margin-top:0;padding:11px 4px;font-size:14px'>扫描</button></div>", " style='margin-top:0;padding:11px 4px;font-size:14px'>Scan</button></div>", " style='margin-top:0;padding:11px 4px;font-size:14px'>スキャン</button></div>", " style='margin-top:0;padding:11px 4px;font-size:14px'>掃描</button></div>" } },
    /*9*/ { "</div><label style='font-weight:400;margin-top:8px'><input type='checkbox' id='chkMan' style='width:auto;padding:0;margin:0 6px 0 0;vertical-align:-2px'>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*10*/ { "", INJ_TXT, { "手动输入名称（上面没扫到我的 WiFi 时用）</label><input id='ssidManual' maxlength='32' style='display:none' placeholder='直接填 WiFi 名'>", "Type the name manually (if my WiFi wasn't found)</label><input id='ssidManual' maxlength='32' style='display:none' placeholder='Enter WiFi name'>", "手動で名前を入力（一覧に自分のWiFiがない場合）</label><input id='ssidManual' maxlength='32' style='display:none' placeholder='WiFi 名を入力'>", "手動輸入名稱（上面沒有我的 Wi-Fi 時使用）</label><input id='ssidManual' maxlength='32' style='display:none' placeholder='直接填 Wi-Fi 名'>" } },
    /*11*/ { "<input type='hidden' name='ssid' id='ssidField'>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*12*/ { "", INJ_TXT, { "<label>2 · WiFi 密码</label>", "<label>2 · WiFi password</label>", "<label>2 · WiFi パスワード</label>", "<label>2 · Wi-Fi 密碼</label>" } },
    /*13*/ { "<div class='eye-wrap'><input id='passField' name='pswd_new' type='password' autocomplete='new-password'", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*14*/ { "", INJ_TXT, { " maxlength='64' placeholder='没有密码就留空'><button type='button' id='btnEye' class='eye-btn' aria-label='显示或隐藏密码'>", " maxlength='64' placeholder='Leave empty if no password'><button type='button' id='btnEye' class='eye-btn' aria-label='Show or hide password'>", " maxlength='64' placeholder='パスワードなしなら空のまま'><button type='button' id='btnEye' class='eye-btn' aria-label='パスワードを表示/非表示'>", " maxlength='64' placeholder='沒有密碼就留空'><button type='button' id='btnEye' class='eye-btn' aria-label='顯示或隱藏密碼'>" } },
    /*15*/ { "<svg id='eyeOn' viewBox='0 0 24 24' width='20' height='20' fill='none' stroke='currentColor' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='M1.8 12S5.4 5 12 5s10.2 7 10.2 7-3.6 7-10.2 7S1.8 12 1.8 12z'/><circle cx='12' cy='12' r='3.1'/></svg><svg id='eyeOff' viewBox='0 0 24 24' width='20' height='20' fill='none' stroke='currentColor' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round' style='display:none'><path d='M2 12s3.6-7 10-7c1.9 0 3.6.6 5 1.5'/><path d='M22 12s-3.6 7-10 7c-1.9 0-3.6-.6-5-1.5'/><path d='M3 3l18 18'/></svg></button></div><hr>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*16*/ { "", INJ_TXT, { "<label>3 · 窗外风景（猫窗口里的景，纯画面）</label>", "<label>3 · Window scenery (the view from the cat's window)</label>", "<label>3 · 窓の風景（猫の窓からの眺め）</label>", "<label>3 · 窗外風景（貓咪窗外的景色）</label>" } },
    /*17*/ { "<select name='scenery' id='scen'>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*18*/ { "", INJ_TXT, { "<option value='pearl' selected>东方明珠</option><option value='fuji'>富士山</option><option value='planet'>星球</option>", "<option value='pearl' selected>Oriental Pearl Tower</option><option value='fuji'>Mount Fuji</option><option value='planet'>Planet</option>", "<option value='pearl' selected>東方明珠</option><option value='fuji'>富士山</option><option value='planet'>惑星</option>", "<option value='pearl' selected>東方明珠</option><option value='fuji'>富士山</option><option value='planet'>星球</option>" } },
    /*19*/ { "", INJ_TXT, { "</select>", "</select>", "</select>", "</select>" } },
    /*20*/ { "", INJ_TXT, { "<div class='note'>只影响窗外的画面，与城市、天气都无关，以后也随时能换。</div><label>4 · 居住城市（用来同步天气）</label>", "<div class='note'>Only changes the view outside the window - it has nothing to do with city or weather, and you can switch it anytime.</div><label>4 · Home city (for weather sync)</label>", "<div class='note'>窓の外の映像だけが変わります。都市や天気とは無関係で、いつでも切り替えられます。</div><label>4 · 居住都市（天気の同期用）</label>", "<div class='note'>只影響窗外的畫面，與城市、天氣無關，之後也可以隨時更換。</div><label>4 · 居住城市（用於同步天氣）</label>" } },
    /*21*/ { "<select id='cc'></select><div class='two'><div id='pvWrap'><select id='pv'></select></div><div><select id='ct'></select></div></div><input type='hidden' name='city' id='ck'>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*22*/ { "", INJ_TXT, { "<div class='note'>先选国家（默认中国），再选城市；中国仍是左边选省、右边选市，两步就填好了，不用打字。天气是城市级精度，选到市就足够准。</div>", "<div class='note'>Pick a country first (China by default), then the city. For China: province on the left, city on the right - two taps, no typing. Chinese city names stay in Chinese on purpose: the weather sync matches them exactly.</div>", "<div class='note'>まず国を選び（デフォルトは中国）、次に都市を選びます。中国は左が省、右が市——2回タップするだけで入力は不要です。中国の都市名は意図的に中国語のままです。天気の同期はこの名前で照合しています。</div>", "<div class='note'>先選國家（預設中國），再選城市；中國為左邊選省、右邊選市，兩步即可，無需打字。中國城市名保持中文：天氣同步依賴精確比對。</div>" } },
    /*22c*/ { "", INJ_TXT, { "<button type='submit'>保存并连接</button>", "<button type='submit'>Save and connect</button>", "<button type='submit'>保存して接続</button>", "<button type='submit'>儲存並連接</button>" } },
    /*23*/ { "</form>", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*24*/ { "", INJ_TXT, { "<p class='note'>保存后设备会自动重启并尝试联网，约 10 秒。</p>", "<p class='note'>The device reboots and tries to connect after saving - about 10 seconds.</p>", "<p class='note'>保存後、デバイスは再起動して接続を試みます。約10秒です。</p>", "<p class='note'>儲存後裝置會自動重新啟動並嘗試連線，約 10 秒。</p>" } },
    /*25*/ { "<script>var PC=", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*26*/ { "", INJ_CITY, { NULL, NULL, NULL, NULL } },
    /*27*/ { ";var PW=", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*28*/ { "", INJ_WORLD, { NULL, NULL, NULL, NULL } },
    /*29*/ { ";var PREV=", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*30*/ { "", INJ_PREV, { NULL, NULL, NULL, NULL } },
    /*31*/ { ";var PGL=", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*32*/ { "", INJ_PGL, { NULL, NULL, NULL, NULL } },
    /*33*/ { ";(function(){var cc=document.getElementById('cc'),pv=document.getElementById('pv'),ct=document.getElementById('ct'),ck=document.getElementById('ck'),pvWrap=document.getElementById('pvWrap'),ks=Object.keys(PC);function cnName(o){return o.n[PGL==='zh'?0:(PGL==='ja'?2:(PGL==='zht'?3:1))];}function cityName(a){return a[PGL==='zh'?1:(PGL==='ja'?3:(PGL==='zht'?4:2))];}function fillCountry(){var cs=Object.keys(PW);cs.sort(function(a,b){return cnName(PW[a]).localeCompare(cnName(PW[b]));});var o=document.createElement('option');o.value='CN';o.textContent=(PGL==='en'?'China':(PGL==='ja'?'中国':(PGL==='zht'?'中國':'中国')));cc.appendChild(o);for(var i=0;i<cs.length;i++){o=document.createElement('option');o.value=cs[i];o.textContent=cnName(PW[cs[i]]);cc.appendChild(o);}}function fillPv(){pv.innerHTML='';for(var i=0;i<ks.length;i++){var o=document.createElement('option');o.value=ks[i];o.textContent=ks[i];pv.appendChild(o);}}function sync(){if(cc.value==='CN'){ck.value=(!ct.value||ct.value===pv.value)?pv.value:(pv.value+ct.value);}else{ck.value=ct.value;}}function fillCity(){if(cc.value==='CN'){pvWrap.style.display='';if(!pv.options.length||!PC[pv.value])fillPv();var a=PC[pv.value]||[];ct.innerHTML='';for(var i=0;i<a.length;i++){var o=document.createElement('option');o.value=a[i];o.textContent=a[i];ct.appendChild(o);}}else{pvWrap.style.display='none';var a=PW[cc.value]?PW[cc.value].c:[];ct.innerHTML='';for(var i=0;i<a.length;i++){var o=document.createElement('option');o.value=a[i][0];o.textContent=cityName(a[i]);ct.appendChild(o);}}sync();}fillCountry();fillCity();cc.onchange=fillCity;pv.onchange=fillCity;ct.onchange=sync;var sel=document.getElementById('ssidSel'),f=document.getElementById('ssidField'),man=document.getElementById('ssidManual'),chk=document.getElementById('chkMan'),st=document.getElementById('ss'),pf=document.getElementById('passField'),sc=document.getElementById('scen');function paint(){var v=chk.checked?man.value.replace(/^\\s+|\\s+$/g,''):sel.value;f.value=(v==='__hint__')?'':v;}function opt(v,t,on,dis){var o=document.createElement('option');o.value=v;o.textContent=t;o.selected=!!on;if(dis)o.disabled=true;sel.appendChild(o);return o;}", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*34*/ { "", INJ_TXT, { "function load(){st.textContent='扫描中…';", "function load(){st.textContent='Scanning...';", "function load(){st.textContent='スキャン中...';", "function load(){st.textContent='掃描中...';" } },
    /*35*/ { "fetch('/scan',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){sel.innerHTML='';var n=0,arr=[];(j.aps||[]).forEach(function(a){if(!a.s)return;arr.push({s:a.s,r:a.r,e:a.e,prev:!!(PREV.s&&a.s===PREV.s)});n++;});arr.sort(function(a,b){return b.r-a.r;});arr.forEach(function(x){", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*36*/ { "", INJ_TXT, { "if(x.prev){opt(x.s,'★ '+x.s+'（上次已连）'+(x.r>-999?('　'+x.r+'dBm'):''),true);}else{opt(x.s,x.s+'　'+x.r+'dBm'+(x.e?'　需密码':''),false);}});if(!sel.options.length){opt('__hint__','（没扫到，请勾选下面手动输入）',true,true);}st.textContent=n?('扫到 '+n+' 个'):'没扫到';paint();}).catch(function(){st.textContent='扫描失败，可勾选手动输入';});}", "if(x.prev){opt(x.s,'* '+x.s+' (last used)'+(x.r>-999?(' '+x.r+'dBm'):''),true);}else{opt(x.s,x.s+' '+x.r+'dBm'+(x.e?' secured':''),false);}});if(!sel.options.length){opt('__hint__','(Nothing found - tick manual entry below)',true,true);}st.textContent=n?('Found '+n):'Nothing found';paint();}).catch(function(){st.textContent='Scan failed - try manual entry';});}", "if(x.prev){opt(x.s,'★ '+x.s+'（前回使用）'+(x.r>-999?('　'+x.r+'dBm'):''),true);}else{opt(x.s,x.s+'　'+x.r+'dBm'+(x.e?'　暗号化':''),false);}});if(!sel.options.length){opt('__hint__','（見つかりません。下の手動入力をオンにしてください）',true,true);}st.textContent=n?(n+' 件見つかりました'):'見つかりません';paint();}).catch(function(){st.textContent='スキャン失敗。手動入力をお試しください';});}", "if(x.prev){opt(x.s,'★ '+x.s+'（上次使用）'+(x.r>-999?('　'+x.r+'dBm'):''),true);}else{opt(x.s,x.s+'　'+x.r+'dBm'+(x.e?'　有密碼':''),false);}});if(!sel.options.length){opt('__hint__','（沒有找到——請勾選下方的手動輸入）',true,true);}st.textContent=n?('找到 '+n+' 個'):'沒有找到';paint();}).catch(function(){st.textContent='掃描失敗——請改用手動輸入';});}" } },
    /*37*/ { "function showPrev(){sel.innerHTML='';", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*38*/ { "", INJ_TXT, { "opt('__hint__','（点上方「扫描附近网络」查找）',true,true);", "opt('__hint__','(Tap Scan above to look for networks)',true,true);", "opt('__hint__','（上のスキャンを押してネットワークを探す）',true,true);", "opt('__hint__','（點上面的掃描來搜尋網路）',true,true);" } },
    /*39*/ { "st.textContent='';paint();}chk.onchange=function(){man.style.display=chk.checked?'block':'none';sel.disabled=chk.checked;paint();};man.oninput=paint;sel.onchange=function(){", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*40*/ { "", INJ_TXT, { "if(sel.value!=='__hint__'&&sel.value!==PREV.s)pf.value='';   /* 换网 → 清密码 */", "if(sel.value!=='__hint__'&&sel.value!==PREV.s)pf.value='';", "if(sel.value!=='__hint__'&&sel.value!==PREV.s)pf.value='';", "if(sel.value!=='__hint__'&&sel.value!==PREV.s)pf.value='';" } },
    /*41*/ { "paint();};document.getElementById('btnScan').onclick=load;var eye=document.getElementById('btnEye'),eOn=document.getElementById('eyeOn'),eOff=document.getElementById('eyeOff');eye.onclick=function(){var vis=pf.type==='text';pf.type=vis?'password':'text';eOn.style.display=vis?'block':'none';eOff.style.display=vis?'none':'block';};if(PREV.p&&PREV.p.length&&sel.value===PREV.s){pf.value=PREV.p;}else{pf.value='';}", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*42*/ { "", INJ_TXT, { "if(PREV.sc){sc.value=(PREV.sc.indexOf('富士')>=0)?'fuji':'pearl';}", "if(PREV.sc){sc.value=(PREV.sc.indexOf('富士')>=0||PREV.sc.indexOf('Fuji')>=0)?'fuji':'pearl';}", "if(PREV.sc){sc.value=(PREV.sc.indexOf('富士')>=0||PREV.sc.indexOf('Fuji')>=0)?'fuji':'pearl';}", "if(PREV.sc){sc.value=(PREV.sc.indexOf('富士')>=0||PREV.sc.indexOf('Fuji')>=0)?'fuji':'pearl';}" } },
    /*43*/ { "if(PREV.c){var m=/^([A-Z]{2}):/.exec(PREV.c);if(m&&PW[m[1]]){cc.value=m[1];fillCity();for(var j=0;j<ct.options.length;j++){if(ct.options[j].value===PREV.c){ct.selectedIndex=j;break;}}sync();}else{var hit='';for(var i=0;i<ks.length;i++){if(PREV.c.slice(0,ks[i].length)===ks[i]){hit=ks[i];break;}}if(hit){pv.value=hit;fillCity();var tail=PREV.c.slice(hit.length);if(tail){for(var j=0;j<ct.options.length;j++){if(ct.options[j].value===tail){ct.selectedIndex=j;break;}}}sync();}}}document.querySelector('form').onsubmit=function(){", INJ_NONE, { NULL, NULL, NULL, NULL } },
    /*44*/ { "", INJ_TXT, { "if(!f.value){alert('请先选择或手动输入 WiFi 名称');return false;}return true;};", "if(!f.value){alert('Pick or type a WiFi name first');return false;}return true;};", "if(!f.value){alert('先にWiFi名を選ぶか入力してください');return false;}return true;};", "if(!f.value){alert('請先選擇或輸入 Wi-Fi 名稱');return false;}return true;};" } },
    /*45*/ { "showPrev();setTimeout(function(){if(pf.value)pf.value='';},600);})();</script></body></html>", INJ_NONE, { NULL, NULL, NULL, NULL } },
};

/* PGL 语言字面量（下标 = lang_t）—— 海外城市下拉的数据标签语言 */
static const char *const PROV_PGL_VAL[LANG_N] = { "'zh'", "'en'", "'ja'", "'zht'" };


/* ------------------------------------------------ 上次参数带入（NVS 预填）*/
/* ★ 2026-09-21 用户反馈：「之前它把我原有的 WiFi 信息自动带入进去了，现在好像不会带入了」。
   做法：配网页每次被请求时，直接读 NVS 里上一套配置，注入页面 JS 常量 PREV。
   注意 nvs_get_str 在**键不存在**时不会动缓冲区，所以每个字段先清零再用。 */
static void load_prev(char *ssid, size_t ns, char *pass, size_t np,
                      char *scenery, size_t nsc, char *city, size_t nc)
{
    ssid[0] = pass[0] = scenery[0] = city[0] = 0;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return;
    size_t l;
    l = ns; if (nvs_get_str(h, "ssid",    ssid,    &l) != ESP_OK) ssid[0] = 0;
    l = np; if (nvs_get_str(h, "pass",    pass,    &l) != ESP_OK) pass[0] = 0;
    l = nsc; if (nvs_get_str(h, "scenery", scenery, &l) != ESP_OK) scenery[0] = 0;
    l = nc; if (nvs_get_str(h, "city",    city,    &l) != ESP_OK) city[0] = 0;
    nvs_close(h);
    ssid[ns - 1] = pass[np - 1] = scenery[nsc - 1] = city[nc - 1] = 0;
}

/* 把一段文本安全地塞进 JS 单引号字符串（中文 UTF-8 原样放过，只处理会破语的字符） */
static void jsq(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '\'' || c == '"') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '<')                      { memcpy(out + n, "\\x3C", 4); n += 4; }
        else if (c == '\n' || c == '\r')        { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c < 0x20 || c == 0x7f)         { out[n++] = ' '; }
        else                                    { out[n++] = (char)c; }
    }
    out[n] = 0;
}

static void build_prev_js(char *dst, size_t cap)
{
    /* ★★ 2026-10-04 体检修（安全）：原来这里把 load_prev() 读到的**主路由明文密码**
       原样拼进 JS（`{s:'…',p:'%s',…}` → 页面 231 行 `if(PREV.p){pf.value=PREV.p;}` 自动填进密码框）。
       而配网热点是 **WIFI_AUTH_OPEN 无密码**（softap_start 里 813 行）→ 周围任何人连上
       `PetCat-XXXX` 打开 http://192.168.4.1/ 就能读到这台设备家���路由的密码。
       —— 改成**不回填密码**（pass 传空 → e2 为空 → PREV.p 为空 → 页面不填）。
       SSID / 城市 / 地标仍回填（那不是秘密，且预填 SSID 能显著减少配网摩擦）。
       用户只需在密码框手打一次，浏览器/页面不再替我们泄露。 */
    char ssid[33] = {0}, pass[65] = {0}, sc[32] = {0}, city[64] = {0};
    load_prev(ssid, sizeof(ssid), pass, sizeof(pass), sc, sizeof(sc), city, sizeof(city));
    (void)pass;                          /* ★ 故意不用：安全，见上方说明 */
    char e1[160], e2[8], e3[160], e4[320];
    jsq(e1, sizeof(e1), ssid);
    e2[0] = '\''; e2[1] = '\''; e2[2] = 0;      /* 密码恒为空字符串 */
    jsq(e3, sizeof(e3), sc);
    jsq(e4, sizeof(e4), city);
    snprintf(dst, cap, "{s:'%s',p:%s,sc:'%s',c:'%s'}", e1, e2, e3, e4);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ★★★ 2026-10-19 API 配置页（用户令 · API 前端批次）
 * ---------------------------------------------------------------------------
 * 用户口径（原话拆条）：
 *   ① 设备端点「API接口」页的开关（**未配置时**）或底部「去配置/重新配置」→ 进配网，
 *      手机连上热点**直接弹出这一页**（不再先出 WiFi 表单页）；
 *   ② 「之前只输入一个 API Key，正常应该还要输入 URL 吧」⇒ 每通道 = Key + 接口地址 + 模型；
 *   ③ 「要给两个入口，一个是文字的和一个是图像的」⇒ 两个区块；
 *   ④ 提示三句：只填文字 → 只有文字自定义；只填图像 → 只有图像自定义；都填 → 都用；
 *      勾了「同一把 Key」但那个接口其实只支持出图 → 文字那块就不给它出（**错误鉴别**，
 *      落在后端批次；本批只把「共用」这个开关收下来存档）；
 *   ⑤ 「先做前端，后端后面再说」。
 * 安全：**API Key 绝不回填到页面** —— 配热点是**无密码**的，回填等于把 Key 印给任何
 *   连上热点的人看。页面上只回填 URL/模型（非敏感），Key 一律走「填了才覆盖」。
 * 四语：本批先中文（其余语言按项目口径回退中文，等中文定稿后统一补翻）。
 * ═══════════════════════════════════════════════════════════════════════════ */
static volatile bool s_api_saved = false;   /* 手机点了「保存」→ ws_api_provision 收门户 */

/* 本次配网目标是不是「配 API」（NVS petns:prov_again == 2，由 main.c 侧写入） */
static bool prov_goal_is_api(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "prov_again", &v);
        nvs_close(h);
    }
    return v == 2;
}

static esp_err_t h_api_page(httpd_req_t *r);      /* 前向声明（定义在 WiFi 表单页之后） */
static esp_err_t h_api_save(httpd_req_t *r);
/* ★2026-10-19：h_api_save 用 form_get，而 form_get 定义在本块**之后** → 必须前向声明 */
static bool form_get(const char *body, const char *key, char *out, size_t cap);

/* 原 h_root 的主题：WiFi 配网表单页（PROV_SEGS 段表 chunked 直发）。
   ★2026-10-19 抽成独立函数：`/` 按目标分派，`/wifi` 强制走这里（API 页上的「先配 WiFi」链接）。 */
static esp_err_t send_wifi_form(httpd_req_t *r)
{
    ESP_LOGW(TAG, "HTTP GET %s", r->uri);        /* ★ 诊断用：手机到底请求了哪条路径 */
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    /* ★ 2026-09-29：断开式可用性加固（用户强调「外网没有访问」）。
       本页**零外部资源**（CSS/JS 全内联，已静态校验），所以无外网也能完整渲染。
       这几条头再把「浏览器自作主张」的口子堵上：
         · Pragma/Expires  → 老安卓 WebView 只认这两个，不认 Cache-Control
         · Connection: close → 手机在某些省电策略下复用连接会挂住，直接断开更稳 */
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    httpd_resp_set_hdr(r, "Expires", "0");
    /* ★★ 2026-10-14 R44：四份整页模板 → PROV_SEGS 段表按语言逐段 chunked 直发。
       城市 JSON（INJ_CITY/INJ_WORLD）在此刻才拼进流 → rodata 只有一份（原 4 份）。
       @@PREV@@/@@PGL@@ 令牌 → INJ_PREV/INJ_PGL 注入位，免去整页 strstr 搜索。 */
    const int lang = (int)g_lang;
    /* ★★ 2026-09-21：此缓冲必须在堆上（httpd 任务栈仅 4096B，栈上分配会 panic，历史见下）。 */
    char *prev = (char *)malloc(1024);
    if (prev) build_prev_js(prev, 1024);          /* malloc 失败也能出页面：PREV 注 "null" */
    esp_err_t err = ESP_OK;
    const size_t n_seg = sizeof(PROV_SEGS) / sizeof(PROV_SEGS[0]);
    for (size_t i = 0; err == ESP_OK && i < n_seg; ++i) {
        const prov_seg_t *sg = &PROV_SEGS[i];
        if (sg->shell[0]) err = httpd_resp_send_chunk(r, sg->shell, strlen(sg->shell));
        const char *inj = NULL;
        switch (sg->inj) {
        case INJ_NONE:  break;
        case INJ_TXT:   inj = sg->txt[lang]; break;      /* 四语文案列；"" 列跳过 */
        case INJ_CITY:  inj = PROV_CITY_JSON; break;
        case INJ_WORLD: inj = PROV_WORLD_JSON; break;
        case INJ_PREV:  inj = prev ? prev : "null"; break;
        case INJ_PGL:   inj = PROV_PGL_VAL[lang]; break;
        }
        if (err == ESP_OK && inj && inj[0]) err = httpd_resp_send_chunk(r, inj, strlen(inj));
    }
    if (err == ESP_OK) err = httpd_resp_send_chunk(r, NULL, 0);   /* 空块收尾 = chunked 结束 */
    free(prev);
    return err;
}

/* ── API 配置页 HTML（占位符 %s 五个：文字 URL / 文字模型 / same 勾选态 / 图像 URL / 图像模型）
   ⚠ 本模板里**不能出现裸 %**（CSS 的 width:100%% 已按 printf 转义写好）。── */

/* ★2026-10-25 二.3：英文版（g_lang == LANG_EN 时用）*/


/* 保存结果页（设备收到 s_api_saved 后几秒关热点回主页） */


/* ★A9 注入修复（2026-10-19）：API 配置页把 NVS 里的 URL/模型名回填进 HTML 的
   value='...'。URL/模型名由用户在配网页输入（配网热点无密码，任何连上的人都能
   提交），若不转义，含 `"` 或 `<` 的输入可闭合属性注入脚本/破坏页面。 */
static void html_escape(const char *src, char *dst, size_t dstsz)
{
    if (!src || dstsz == 0) return;
    size_t o = 0;
    for (const char *p = src; *p && o + 7 < dstsz; ++p) {
        switch (*p) {
        case '&':  memcpy(dst + o, "&amp;",  5); o += 5; break;
        case '<':  memcpy(dst + o, "&lt;",   4); o += 4; break;
        case '>':  memcpy(dst + o, "&gt;",   4); o += 4; break;
        case '"':  memcpy(dst + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(dst + o, "&#39;",  5); o += 5; break;
        default:   dst[o++] = *p; break;
        }
    }
    dst[o] = 0;
}

static esp_err_t h_api_page(httpd_req_t *r)
{
    /* 回填只回 URL/模型（非敏感）；**API Key 一律不回填**（热点无密码，回填等于印给别人看） */
    char turl[192] = {0}, tmodel[48] = {0}, iurl[192] = {0}, imodel[48] = {0};
    int32_t same = 0;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
        size_t l1 = sizeof(turl), l2 = sizeof(tmodel), l3 = sizeof(iurl), l4 = sizeof(imodel);
        nvs_get_str(h, "api_url",       turl,   &l1);
        nvs_get_str(h, "api_model",     tmodel, &l2);
        nvs_get_str(h, "api_img_url",   iurl,   &l3);
        nvs_get_str(h, "api_img_model", imodel, &l4);
        nvs_get_i32(h, "api_same_key",  &same);
        nvs_close(h);
    }
    /* static（不在 httpd 栈上）；httpd 单任务串行处理，非重入安全（同 zfont/acc 先例） */
    static EXT_RAM_BSS_ATTR char pg[8192];
    /* ★A9：回填值先过 HTML 转义（防 `"`/`<` 闭合属性注入脚本） */
    static EXT_RAM_BSS_ATTR char eturl[1152], etmodel[288], eiurl[1152], eimodel[288];
    html_escape(turl,   eturl,   sizeof(eturl));
    html_escape(tmodel, etmodel, sizeof(etmodel));
    html_escape(iurl,   eiurl,   sizeof(eiurl));
    html_escape(imodel, eimodel, sizeof(eimodel));
    int wl; const char *tpl = webui_page((g_lang == LANG_EN) ? WEBUI_API_EN : WEBUI_API_ZH, &wl);
    snprintf(pg, sizeof(pg), tpl, eturl, etmodel, (same == 1) ? "checked" : "",
             eiurl, eimodel);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    ESP_LOGW(TAG, "HTTP GET /api → API 配置页（%d 字节）", (int)strlen(pg));
    return httpd_resp_send(r, pg, strlen(pg));
}

static esp_err_t h_api_save(httpd_req_t *r)
{
    /* ★R208：1024 → 2048 —— 文字/图像各一把最长 256B 的 key + 两条 URL + 模型名，
       1024 会截掉后半段表单（表现为"网址/模型没存进去"）。 */
    char body[2048] = {0};
    if (r->content_len <= 0) { httpd_resp_send(r, "BAD", 3); return ESP_OK; }   /* ★2026-10-23 修：无 Content-Length 的 chunked 请求直接回绝，别把空表单当"已保存" */
    /* ★R213（用户令）：超缓冲的请求**拒绝**，不再静默截断 —— 截断后 form_get 拿不到后半段，
       表现是「网址/模型没存进去」却回了保存成功，用户查不出原因。 */
    if (r->content_len > (int)sizeof(body) - 1) {
        ESP_LOGW(TAG, "★ API 配置：表单 %d B > 缓冲 %d B → 拒绝保存", (int)r->content_len, (int)sizeof(body) - 1);
        httpd_resp_send(r, "BAD", 3);
        return ESP_OK;
    }
    int total = (int)r->content_len;
    int got = 0;
    const int64_t t0 = esp_timer_get_time();       /* 总时限 10s（同 h_save，防慢速客户端拖死） */
    while (got < total) {
        int n = httpd_req_recv(r, body + got, total - got);
        if (n <= 0) break;
        got += n;
        if ((esp_timer_get_time() - t0) / 1000 >= 10000) break;
    }
    /* ★R213（用户令）：没收全就**不落盘** —— 原来半截 body 照样解析并回「已保存」。 */
    if (got != total) {
        ESP_LOGW(TAG, "★ API 配置：只收到 %d/%d B（超时/断连）→ 拒绝保存", got, total);
        httpd_resp_send(r, "BAD", 3);
        return ESP_OK;
    }
    body[got] = 0;

    char tkey[256] = {0}, turl[192] = {0}, tmodel[48] = {0};
    char ikey[256] = {0}, iurl[192] = {0}, imodel[48] = {0}, same[8] = {0};
    form_get(body, "tkey",   tkey,   sizeof(tkey));
    form_get(body, "turl",   turl,   sizeof(turl));
    form_get(body, "tmodel", tmodel, sizeof(tmodel));
    form_get(body, "ikey",   ikey,   sizeof(ikey));
    form_get(body, "iurl",   iurl,   sizeof(iurl));
    form_get(body, "imodel", imodel, sizeof(imodel));
    form_get(body, "same",   same,   sizeof(same));
    const bool is_same = (same[0] == '1');

    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) == ESP_OK) {
        if (tkey[0])   nvs_set_str(h, "api_key",   tkey);        /* 留空 = 不改动（不覆盖老 Key） */
        if (turl[0])   nvs_set_str(h, "api_url",   turl);
        if (tmodel[0]) nvs_set_str(h, "api_model", tmodel);
        /* ★R158 修：原来这里包着 `if (!is_same)` —— 勾了「同一把 Key 和地址」就把下面
           三个键**整块跳过**，用户在图像通道认真填的地址/模型一个字节都存不进去，
           于是 api_img_url 永远为空 → 图像通道回落硬编码阿里云 → key 对不上 → 秒失败。
           现在改成「填了就存」；same 只表示 key 复用（本来 key 就有兜底回落）。 */
        if (ikey[0])   nvs_set_str(h, "api_img_key",   ikey);
        if (iurl[0])   nvs_set_str(h, "api_img_url",   iurl);
        if (imodel[0]) nvs_set_str(h, "api_img_model", imodel);
        nvs_set_i32(h, "api_same_key", is_same ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "★ API 配置已保存：文字 %s · 图像 %s（同一把 key = %d）",
             tkey[0] ? "新填" : "未改",
             is_same ? "随文字通道" : (ikey[0] ? "新填" : "未改"), is_same ? 1 : 0);
    fflush(stdout);
    s_api_saved = true;                    /* 通知 ws_api_provision：可以收门户了 */
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    { int wl; const char *wp = webui_page((g_lang == LANG_EN) ? WEBUI_API_OK_EN : WEBUI_API_OK_ZH, &wl);
      httpd_resp_send(r, wp, wl); }
    return ESP_OK;
}

/* ── `/` 按配网目标分派（★2026-10-19）：API 目标 → 直接出 API 配置页 ── */
static esp_err_t h_root(httpd_req_t *r)
{
    if (prov_goal_is_api()) {
        ESP_LOGW(TAG, "HTTP GET / → 本次目标是配 API → 直接出 API 配置页");
        return h_api_page(r);
    }
    return send_wifi_form(r);
}

/* ── `/wifi` = 强制出 WiFi 表单页（API 页顶部「先去配 WiFi」链接的落点）── */
static esp_err_t h_wifi(httpd_req_t *r)
{
    ESP_LOGW(TAG, "HTTP GET /wifi → WiFi 配网表单页（由 API 页转来）");
    return send_wifi_form(r);
}

/* URL 解码（%XX 与 +） */
static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

/* 从 application/x-www-form-urlencoded 里取一个字段（含 URL 解码） */
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            /* ★2026-10-23 修（H3）：先按**编码后**长度进临时缓冲，URL 解码后再按 decoded
               长度截到 cap。★R216 全量检查：tmp 原为 256B、依据只写「密码 64B」，
               但 API 页 tkey/ikey 是 256B，percent-encode 后最坏 768B 会被静默截断，
               且截断后仍 return true 回「已保存」→ 之后鉴权恒 401 且查不出原因。
               改为 1024B；真超长则拒绝，绝不静默写错值。 */
            char tmp[1024];
            if (n >= sizeof(tmp)) {
                ESP_LOGW(TAG, "form_get: 字段 %.*s 编码后 %u B 超缓冲，拒绝",
                         (int)klen, key, (unsigned)n);
                return false;
            }
            memcpy(tmp, v, n);
            tmp[n] = 0;
            url_decode(tmp);                    /* 原地解码（只缩不涨） */
            size_t dn = strlen(tmp);
            if (dn >= cap) dn = cap - 1;
            memcpy(out, tmp, dn);
            out[dn] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* ★★ 2026-09-30 重写：提交后的页面改成**结果页**，而不是旧版那个骗人的「已保存」。
   ── 旧文案「已保存 / 设备正在连接 WiFi，约 10 秒后会自动刷新屏幕」在密码填错时同样会出现，
      用户会以为配好了、然后发现设备还是没网 —— 这是 spec §4.4d 明确要避免的。
   ── 新页面：先显示「连接中…」，每秒轮询 /status：
        {"r":"ok"}   → 「连接成功」+ 提示可以关掉页面（此时设备已收热点）
        {"r":"fail"} → 「连接失败，请检查密码 / 换个 WiFi」+ 「返回重填」按钮
      设备连接成功后会先把热点留 2.5s 让这一轮轮询拿到 ok，再收门户。 */
/* ★ 2026-10-14 四语化：结果页同样中/繁/英/日四套，h_save() 按 g_lang 选（维护约定同 PROV_HTML）。 */

/* ════════════════════════════════════════════════════════════════════════════
   ★★ R96/R97（2026-10-27 用户要求）：取名窗口关闭后**不许再挂着一颗死链按钮**
   ── 为什么必须处理：手机结果页一旦从 /status 拿到 r=ok、nm=0，就**彻底不再轮询**
      （ok 分支里一个 setTimeout 都没有）。设备端取名子流程靠 `idle >= LEAVE_MS`
      判定"手机走了"，超时即 break → prov_portal_stop() 把 HTTP+DNS+热点一起收掉。
      那颗 href='/name' 按钮从那一刻起指向**已经不存在的服务器**，点了只能看到浏览器
      自己的 ERR_CONNECTION_REFUSED（R95 修的乱码页是另一件事，别混）。
   ── 做法：页面在 ok 之后起倒计时（比设备端 LEAVE_MS 早 1 秒留余量），到点把按钮
      换成引导文案。两种语言套逐句对应，改文案务必四套一起改（本项目 WA 铁则）。
   ── ★R97 用户拍板：
      ① 窗口从 15 秒**延长到 30 秒**（LEAVE_MS 15000 → 30000）⇒ 倒计时 29 秒；
      ② 引导文案**只留一套、不分版本**（我先按 BOOT / 点屏写了两种，被否 ——
         原话「你注明版和那个按键版就可以用同一个文案啊，你这样的话就分两个文案了」）
         ⇒ 措辞中性成「在设备上使用默认名」，两版都成立；
      ③ 文案再精简一点。
   ════════════════════════════════════════════════════════════════════════════ */
/* ★ 引导文案：**按键版 / 触屏版共用同一句**（R97）。别再加 #if TOUCH_DEMO 分叉。 */
#define NAME_GONE_ZH  "在设备上使用默认名；或回设置页重新配网，再次进入取名页。"
#define NAME_GONE_ZHT "在裝置上使用預設名稱；或回設定頁重新配網，再次進入取名頁。"
#define NAME_GONE_EN  "Use the default name on the device, or go back to Settings and reconnect to name your cat."
#define NAME_GONE_JA  "デバイスで既定の名前を使うか、設定画面から再接続して名前を付けてください。"

static const char SAVE_OK_HTML_ZH[] =
"<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>正在连接</title><style>"
"body{font-family:-apple-system,'PingFang SC',sans-serif;padding:48px 20px;text-align:center;"
"background:#f5f4f0;color:#2c2c2a}h1{font-size:20px;font-weight:500}"
"p{font-size:14px;color:#5f5e5a;line-height:1.7}"
"p.note{font-size:12.5px;color:#888780;line-height:1.75;margin-top:16px;text-align:left}"
"a.btn{display:inline-block;margin-top:14px;padding:12px 22px;border-radius:8px;"
"background:#534ab7;color:#fff;text-decoration:none;font-size:15px}"
"</style></head><body>"
"<h1 id='t'>正在连接…</h1><p id='d'>设备正在用你填的 WiFi 联网，请稍候（约 10 秒）。</p>"
"<div id='act'></div>"
"<script>"
"var n=0;function q(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()})"
".then(function(j){if(j.r==='ok'){document.getElementById('t').textContent='连接成功';"
"document.getElementById('d').textContent=j.nm?'设备已联网，名字已经取好了。':'设备已联网。现在可以给猫猫取个名字：';"
"if(!j.nm){document.getElementById('act').innerHTML=\"<a class='btn' href='/name'>给猫猫取名</a>\";"
"setTimeout(function(){document.getElementById('t').textContent='取名窗口已关闭';"
"document.getElementById('d').textContent='设备为省电已收起热点。';"
"document.getElementById('act').innerHTML=\"<p class='note'>" NAME_GONE_ZH "</p>\";},29000);}}"
"else if(j.r==='fail'){document.getElementById('t').textContent='连接失败';"
"document.getElementById('d').textContent='没能连上这个 WiFi，请检查密码，或换一个网络重试。';"
"document.getElementById('act').innerHTML=\"<a class='btn' href='/'>返回重填</a>\";}"
"else{if(++n<40)setTimeout(q,1000);}}).catch(function(){"
"if(++n<40)setTimeout(q,1000);else{document.getElementById('t').textContent='设备已重启';"
"document.getElementById('d').textContent='页面已失去连接，说明设备可能已经开始工作，请直接看屏幕。';}});}"
"q();"
"</script></body></html>";

/* ── EN 套：结构与 SAVE_OK_HTML_ZH 逐行对应，仅可见文案英文化（猫猫 → Mio）。
      ★ JA/ZHT 套见下方（gen_prov_lang.py 生成，勿手改）。── */
static const char SAVE_OK_HTML_EN[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Connecting</title><style>"
"body{font-family:-apple-system,'PingFang SC',sans-serif;padding:48px 20px;text-align:center;"
"background:#f5f4f0;color:#2c2c2a}h1{font-size:20px;font-weight:500}"
"p{font-size:14px;color:#5f5e5a;line-height:1.7}"
"p.note{font-size:12.5px;color:#888780;line-height:1.75;margin-top:16px;text-align:left}"
"a.btn{display:inline-block;margin-top:14px;padding:12px 22px;border-radius:8px;"
"background:#534ab7;color:#fff;text-decoration:none;font-size:15px}"
"</style></head><body>"
"<h1 id='t'>Connecting...</h1><p id='d'>The device is joining your WiFi - please wait (about 10 seconds).</p>"
"<div id='act'></div>"
"<script>"
"var n=0;function q(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()})"
".then(function(j){if(j.r==='ok'){document.getElementById('t').textContent='Connected';"
"document.getElementById('d').textContent=j.nm?'You are online. The name is already set.':'You are online. Now give the cat a name:';"
"if(!j.nm){document.getElementById('act').innerHTML=\"<a class='btn' href='/name'>Name your cat</a>\";"
"setTimeout(function(){document.getElementById('t').textContent='Naming window closed';"
"document.getElementById('d').textContent='The device stopped its hotspot to save power.';"
"document.getElementById('act').innerHTML=\"<p class='note'>" NAME_GONE_EN "</p>\";},29000);}}"
"else if(j.r==='fail'){document.getElementById('t').textContent='Connection failed';"
"document.getElementById('d').textContent='Could not join this WiFi. Check the password, or try another network.';"
"document.getElementById('act').innerHTML=\"<a class='btn' href='/'>Back to form</a>\";}"
"else{if(++n<40)setTimeout(q,1000);}}).catch(function(){"
"if(++n<40)setTimeout(q,1000);else{document.getElementById('t').textContent='Device rebooted';"
"document.getElementById('d').textContent='The page lost contact with the device - it may already be working. Just look at its screen.';}});}"
"q();"
"</script></body></html>";

/* ── JA 套：结构与 SAVE_OK_HTML_EN 逐行对应，仅可见文案日文化（Mio → ミオ）。
   ★ 2026-10-14 四语化新增，由 scripts/_tools/gen_prov_lang.py 生成（勿手改）。── */
static const char SAVE_OK_HTML_JA[] =
"<!DOCTYPE html><html lang='ja'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>接続中</title><style>"
"body{font-family:-apple-system,'PingFang SC',sans-serif;padding:48px 20px;text-align:center;"
"background:#f5f4f0;color:#2c2c2a}h1{font-size:20px;font-weight:500}"
"p{font-size:14px;color:#5f5e5a;line-height:1.7}"
"p.note{font-size:12.5px;color:#888780;line-height:1.75;margin-top:16px;text-align:left}"
"a.btn{display:inline-block;margin-top:14px;padding:12px 22px;border-radius:8px;"
"background:#534ab7;color:#fff;text-decoration:none;font-size:15px}"
"</style></head><body>"
"<h1 id='t'>接続中…</h1><p id='d'>デバイスがWiFiに接続しています。しばらくお待ちください（約10秒）。</p>"
"<div id='act'></div>"
"<script>"
"var n=0;function q(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()})"
".then(function(j){if(j.r==='ok'){document.getElementById('t').textContent='接続成功';"
"document.getElementById('d').textContent=j.nm?'接続しました。名前は設定済みです。':'インターネットに接続しました。猫に名前を付けてください：';"
"if(!j.nm){document.getElementById('act').innerHTML=\"<a class='btn' href='/name'>猫に名前を付ける</a>\";"
"setTimeout(function(){document.getElementById('t').textContent='名前の入力画面を終了しました';"
"document.getElementById('d').textContent='節電のため、デバイスはホットスポットを停止しました。';"
"document.getElementById('act').innerHTML=\"<p class='note'>" NAME_GONE_JA "</p>\";},29000);}}"
"else if(j.r==='fail'){document.getElementById('t').textContent='接続失敗';"
"document.getElementById('d').textContent='このWiFiに接続できませんでした。パスワードを確認するか、別のネットワークでお試しください。';"
"document.getElementById('act').innerHTML=\"<a class='btn' href='/'>入力画面に戻る</a>\";}"
"else{if(++n<40)setTimeout(q,1000);}}).catch(function(){"
"if(++n<40)setTimeout(q,1000);else{document.getElementById('t').textContent='デバイスが再起動しました';"
"document.getElementById('d').textContent='ページとの接続が切れました。デバイスはすでに動いている可能性があります。画面を直接ご確認ください。';}});}"
"q();"
"</script></body></html>";

/* ── ZHT 套：结构与 SAVE_OK_HTML_EN 逐行对应，仅可见文案繁体化（Mio → 貓貓）。
   ★ 2026-10-14 四语化新增，由 scripts/_tools/gen_prov_lang.py 生成（勿手改）。── */
static const char SAVE_OK_HTML_ZHT[] =
"<!DOCTYPE html><html lang='zh-Hant'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>連線中</title><style>"
"body{font-family:-apple-system,'PingFang SC',sans-serif;padding:48px 20px;text-align:center;"
"background:#f5f4f0;color:#2c2c2a}h1{font-size:20px;font-weight:500}"
"p{font-size:14px;color:#5f5e5a;line-height:1.7}"
"p.note{font-size:12.5px;color:#888780;line-height:1.75;margin-top:16px;text-align:left}"
"a.btn{display:inline-block;margin-top:14px;padding:12px 22px;border-radius:8px;"
"background:#534ab7;color:#fff;text-decoration:none;font-size:15px}"
"</style></head><body>"
"<h1 id='t'>連線中…</h1><p id='d'>裝置正在連上你的 Wi-Fi，請稍候（約 10 秒）。</p>"
"<div id='act'></div>"
"<script>"
"var n=0;function q(){fetch('/status',{cache:'no-store'}).then(function(r){return r.json()})"
".then(function(j){if(j.r==='ok'){document.getElementById('t').textContent='連線成功';"
"document.getElementById('d').textContent=j.nm?'裝置已連網，名字已經取好了。':'裝置已連網。現在可以幫貓貓取個名字：';"
"if(!j.nm){document.getElementById('act').innerHTML=\"<a class='btn' href='/name'>幫貓貓取名</a>\";"
"setTimeout(function(){document.getElementById('t').textContent='取名視窗已關閉';"
"document.getElementById('d').textContent='裝置為省電已收起熱點。';"
"document.getElementById('act').innerHTML=\"<p class='note'>" NAME_GONE_ZHT "</p>\";},29000);}}"
"else if(j.r==='fail'){document.getElementById('t').textContent='連線失敗';"
"document.getElementById('d').textContent='無法連上這個 Wi-Fi，請檢查密碼，或換個網路再試。';"
"document.getElementById('act').innerHTML=\"<a class='btn' href='/'>返回重新填寫</a>\";}"
"else{if(++n<40)setTimeout(q,1000);}}).catch(function(){"
"if(++n<40)setTimeout(q,1000);else{document.getElementById('t').textContent='裝置已重新啟動';"
"document.getElementById('d').textContent='頁面已失去連線，裝置可能已經開始運作，請直接查看螢幕。';}});}"
"q();"
"</script></body></html>";

static const char PROV_STATUS_JSON[] = "{\"r\":\"wait\"}";

/* ★★ 2026-09-30 新增：/status —— 手机结果页轮询配网结果（见 SAVE_OK_HTML_ZH）。 */
/* ★★ 2026-10-17：设备**是否已经有名字**（pet_name 非"-"）。
   用户报「已经取过名字了，还会弹出取名的后续页面」—— 真正的入口是手机 /status 页
   无条件注入的那颗「给猫猫取名」按钮：设备根本不缺名字，是网页在邀请他再取一次。
   ★★ R94（2026-10-27）：判据**只认 pet_name**，不再看 `named_once` ——
   那个键曾经在"取名没成功"时也被置位（见 ws_provision 收尾的旧写法），
   结果就是用户现在的处境：名字还是「-」，但系统一口咬定"已取过名"，再也不让取。 */
static bool prov_has_name(void)
{
    bool has = false;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
        char b[16] = {0};
        size_t n = sizeof(b);
        if (nvs_get_str(h, "pet_name", b, &n) == ESP_OK && b[0] && strcmp(b, "-") != 0) has = true;
        nvs_close(h);
    }
    return has;
}

static esp_err_t h_status(httpd_req_t *r)
{
    s_name_act_ms = esp_timer_get_time() / 1000;   /* ★ 2026-10-12：心跳（取名子流程判手机是否还在）*/
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const char *s = PROV_STATUS_JSON;
    if      (s_prov_state == 2) s = prov_has_name() ? "{\"r\":\"ok\",\"nm\":1}"
                                                    : "{\"r\":\"ok\",\"nm\":0}";
    else if (s_prov_state == 3) s = "{\"r\":\"fail\"}";
    httpd_resp_sendstr(r, s);
    return ESP_OK;
}

/* ══ 2026-10-12 取名功能：/name 表单页 + /namesave 提交 ══ */
/* ★★ 2026-10-13 按第七轮定稿设计稿重做（旧版三处不符）：
   ① 旧版有「暂不取名」跳过链接 —— 设计稿 ⑦ 明确「没有跳过按钮」，
      正常流程不放跳过；退出网页本身就是跳过（设备 90s 心跳 / 15s 离热点检测已覆盖），
      /nameskip handler 保留作兜底（旧缓存页面可能还在调），但页面不再给入口。
   ② 旧版「用默认名猫猫」是白底描边 alt 按钮（用户：太丑）—— 改设计稿的
      文字链接「不取名了，用默认名「猫猫」」，且走二次确认。
   ③ 旧版点「确定」直接落盘 —— 设计稿 ⑥ 要求**正常取名也走二次确认**（防误触）。
      现在分两步：不带 confirm=1 只校验，带 confirm=1 才写 NVS。 */
/* ★ 2026-10-13 多语言：取名页同样中/英两套，h_name() 按 g_lang 选（维护约定同 PROV_HTML）。
   ★ EN 套里默认名 = **Mio**（用户拍板：「猫猫在英文版本里就改成 Mio」）——
     与设备端命名屏的 i18n 译文（「用默认名猫猫」→ Use "Mio"）保持一致。 */

/* ── EN 套：结构与 NAME_HTML_ZH 逐行对应，仅可见文案英文化；默认名 = Mio。
      ★ JA/ZHT 套见下方（gen_prov_lang.py 生成，默认名 ミオ/貓貓，勿手改）。── */

/* ── JA 套：结构与 NAME_HTML_EN 逐行对应，仅可见文案日文化；默认名 = ミオ。
   ★ 2026-10-14 四语化新增，由 scripts/_tools/gen_prov_lang.py 生成（勿手改）。── */

/* ── ZHT 套：结构与 NAME_HTML_EN 逐行对应，仅可见文案繁体化；默认名 = 貓貓。
   ★ 2026-10-14 四语化新增，由 scripts/_tools/gen_prov_lang.py 生成（勿手改）。── */

static esp_err_t h_name(httpd_req_t *r)
{
    s_name_act_ms = esp_timer_get_time() / 1000;
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    /* ★ 2026-10-14 四语化：按设备语言选 中/繁/英/日 套 */
    const int idx = WEBUI_NAME_ZH + ((g_lang == LANG_ZHT) ? 3 :
                    (g_lang == LANG_JA) ? 2 : (g_lang == LANG_EN) ? 1 : 0);
    /* ★★ R95：长度必须与指针**同源**（走 out_len）。旧写法 len 直接读 s_webui_len[]，
       webui 载入失败时就成了「空串指针 + 4770 字节真长度」→ 把 .rodata 当网页发出去
       （用户实拍：一整页 mbedtls 证书 OID 表 = 乱码）。资源真缺就明确报 500。 */
    int wl = 0;
    const char *tpl = webui_page(idx, &wl);
    if (!tpl || wl <= 0) {
        ESP_LOGE(TAG, "★ 取名页资源缺失（webui idx=%d）→ 回 500，不发垃圾字节", idx);
        fflush(stdout);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_OK;
    }
    httpd_resp_send(r, tpl, wl);
    return ESP_OK;
}

/* ★ 2026-10-14 四语化：namesave 失败原因按设备语言出（手机页 JS 直接显示 j.reason）。
   三种失败各一个查表函数；分支顺序不影响行为（默认分支 = EN）。
   js[160] 缓冲仍按 EN 最长案留（JA/ZHT 均短于 EN）。 */
static const char *ns_reason_empty(void)
{
    return (g_lang == LANG_ZH)  ? "{\"ok\":false,\"reason\":\"名字不能为空\"}" :
           (g_lang == LANG_ZHT) ? "{\"ok\":false,\"reason\":\"名字不能為空\"}" :
           (g_lang == LANG_JA)  ? "{\"ok\":false,\"reason\":\"名前は空にできません\"}"
                                : "{\"ok\":false,\"reason\":\"Name can't be empty\"}";
}

static const char *ns_reason_bad_fmt(void)
{
    return (g_lang == LANG_ZH)  ? "{\"ok\":false,\"bad\":\"%s\",\"reason\":\"有字不在设备字库\"}" :
           (g_lang == LANG_ZHT) ? "{\"ok\":false,\"bad\":\"%s\",\"reason\":\"有字不在裝置字庫\"}" :
           (g_lang == LANG_JA)  ? "{\"ok\":false,\"bad\":\"%s\",\"reason\":\"デバイスのフォントにない文字があります\"}"
                                : "{\"ok\":false,\"bad\":\"%s\",\"reason\":\"Some characters are missing from the device font\"}";
}

static const char *ns_reason_max(void)
{
    return (g_lang == LANG_ZH)  ? "{\"ok\":false,\"reason\":\"最多 5 个字符\"}" :
           (g_lang == LANG_ZHT) ? "{\"ok\":false,\"reason\":\"最多 5 個字元\"}" :
           (g_lang == LANG_JA)  ? "{\"ok\":false,\"reason\":\"最大5文字です\"}"
                                : "{\"ok\":false,\"reason\":\"At most 5 characters\"}";
}

static esp_err_t h_namesave(httpd_req_t *r)
{
    s_name_act_ms = esp_timer_get_time() / 1000;
    char body[256] = {0};
    /* ★R213（用户令）：超缓冲 / 没收全 → 直接回失败；★R216：无 Content-Length 的
       chunked 请求（<=0）也提前拒绝，与 h_save / h_api_save 三入口口径统一。 */
    if (r->content_len <= 0 || r->content_len > (int)sizeof(body) - 1) {
        ESP_LOGW(TAG, "★ 取名：表单 %d B > 缓冲 %d B → 拒绝", (int)r->content_len, (int)sizeof(body) - 1);
        httpd_resp_set_type(r, "application/json; charset=utf-8");
        httpd_resp_sendstr(r, "{\"ok\":false}");
        return ESP_OK;
    }
    int total = (int)r->content_len;
    int got = 0;
    if (total < 0) total = 0;
    /* ★★ 2026-10-20 修（httpd 任务可被单客户端拖死）：单次 httpd_req_recv 只有默认 5s 超时，
       慢速客户端每次只吐 1 字节就能把**唯一的 httpd 任务**按住十几分钟（本 handler 的
       body 是 256B ≈ 17 分钟），期间其他手机根本打不开配网页 —— 而配网热点是**开放 AP**，
       任何连得上的人都能触发。同文件 h_save()（1204 一带）与 h_api_save()（440 一带）
       都已有这道 10s 总时限闸，本 handler 是当初漏补的第三个入口。 */
    const int64_t recv_t0 = esp_timer_get_time();
    while (got < total) {
        int n = httpd_req_recv(r, body + got, total - got);
        if (n <= 0) break;
        got += n;
        if ((esp_timer_get_time() - recv_t0) / 1000 >= 10000) break;   /* 总时限 10s */
    }
    if (got != total) {
        ESP_LOGW(TAG, "★ 取名：只收到 %d/%d B → 拒绝", got, total);
        httpd_resp_set_type(r, "application/json; charset=utf-8");
        httpd_resp_sendstr(r, "{\"ok\":false}");
        return ESP_OK;
    }
    body[got] = 0;
    /* ★★ 2026-10-13 修「猫猫→猀」必现 bug：
       form_get 的 cap 是在 **URL 解码之前** 按字符数截断的，而 percent-encoded
       的汉字一个字要 3 倍长（「猫」= %E7%8C%AB = 9 字符）。旧代码给 16 字节，
       导致「猫猫」（18 字符）被截成 15 字符 = %E7%8C%AB%E7%8C —— 最后一个
       字节丢失 ⇒ UTF-8 序列不完整 ⇒ 解出 **U+7300「猀」**（生僻字，不在字库）
       ⇒ 校验必然失败。任何 ≥2 个汉字的名字都必中。
       修法：取值缓冲区按 **encoded 最长** 给（5 码点 × 4 字节 UTF-8 × 3 = 60，
       留余量取 80），解码后的真实长度由 pet_name_valid 按码点数（≤5）把关。 */
    char name[80] = {0};
    form_get(body, "name", name, sizeof(name));
    char conf[8] = {0};
    form_get(body, "confirm", conf, sizeof(conf));   /* ★ 二次确认：=1 才真正落盘 */
    char bad[8] = {0};
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (!pet_name_valid(name, bad)) {
        /* ★2026-10-14 四语化：reason 文案按设备语言出（查表函数见本函数上方）。
           ⚠ js[] 缓冲按 EN 最长案 160B：旧值 80B 装不下
             「Some characters are missing from the device font」+ 结构开销（88B）会截断。 */
        /* ★R94：失败必须留痕 —— 手机只看到一句文案，串口这边要能查出
           「用户到底提交了什么、卡在哪一关」，否则这种 bug 只能靠猜。 */
        ESP_LOGW(TAG, "★ 取名被拒：收到「%s」→ %s%s", name,
                 name[0] == 0 ? "名字为空" : (bad[0] ? "字库缺字：" : "超过 5 个字符"),
                 bad[0] ? bad : "");
        fflush(stdout);
        if (name[0] == 0)
            httpd_resp_sendstr(r, ns_reason_empty());
        else if (bad[0]) {
            char js[160];
            snprintf(js, sizeof(js), ns_reason_bad_fmt(), bad);
            httpd_resp_sendstr(r, js);
        } else
            httpd_resp_sendstr(r, ns_reason_max());
        return ESP_OK;
    }
    /* ★ 2026-10-13：校验通过但不带 confirm=1 ⇒ 只回报「可保存」，交给手机二次确认页，
       此时**不写 NVS、不置 s_name_saved**（旧版这里就落盘了，与设计稿 ⑥ 不符）。 */
    if (conf[0] != '1') {
        httpd_resp_sendstr(r, "{\"ok\":true,\"check\":1}");
        return ESP_OK;
    }
    strncpy(s_name_buf, name, sizeof(s_name_buf) - 1);
    s_name_buf[sizeof(s_name_buf) - 1] = 0;
    s_name_saved = true;
    /* ★★ R94（2026-10-27 用户报「点保存跳报错页 → 再配网说已取过名 → 名字还是横杠」）：
       **不再分「配网流程 / 改名卡门户」两条落盘路径 —— 一律在这里当场写 NVS。**
       旧写法只在 s_name_standalone（改名卡）时写盘，配网路径只置 s_name_saved、等
       ws_provision 的取名子循环稍后落盘。这中间有个致命空档：子循环 50ms 轮询看到
       s_name_saved 就去写盘，可如果**手机那头已经收到失败/断连**（响应丢了、热点被
       子流程收掉），子循环仍会以 named=false 收尾，然后**无条件**把 named_once 置 1
       ⇒ 用户的猫名字还是 "-"，但系统从此认定"取过名了"，再也不弹取名页。
       当场写盘后：只要手机看到过 ok，名字一定已在 flash 里；不一致状态从根上消失。 */
    {
        nvs_handle_t hn;
        if (nvs_open("petns", NVS_READWRITE, &hn) == ESP_OK) {
            nvs_set_str(hn, "pet_name", s_name_buf);
            nvs_commit(hn);
            nvs_close(hn);
            strlcpy(g_pet_name, s_name_buf, 16);
            ESP_LOGW(TAG, "★ 取名：名字已写入 NVS「%s」%s", g_pet_name,
                     s_name_standalone ? "（改名卡门户）" : "（配网流程）");
        } else {
            ESP_LOGE(TAG, "★ 取名：NVS 打开失败，名字没写进去（%s）", s_name_buf);
        }
    }
    /* 改名卡门户：名字写入即收工（看门任务看到 s_name_saved 会关热点）*/
    ESP_LOGI(TAG, "★ 取名：手机提交「%s」通过校验", s_name_buf);
    fflush(stdout);
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

/* ★ 2026-10-12：GET /nameskip —— 手机页「暂不取名」主动通知设备退出取名子流程。
   修前手机页写的是 `href='/'`，只是把手机浏览器导航回配网首页，**设备完全不知情**，
   仍要干等到 90s 心跳超时 / 180s 上限 —— 用户以为跳过了，其实没跳。
   现在置 s_name_skip，设备子循环立即 break（named 保持 false → pet_name 不写）。 */
static esp_err_t h_nameskip(httpd_req_t *r)
{
    s_name_act_ms = esp_timer_get_time() / 1000;
    s_name_skip   = true;
    ESP_LOGI(TAG, "★ 取名：手机点「暂不取名」→ 通知设备退出取名子流程");
    fflush(stdout);
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_save(httpd_req_t *r)
{
    char body[1280] = {0};
    /* ★R213（用户令）：超缓冲 / 没收全 → 明确失败，不再静默截断后当成「已配网」。
       截断会让 SSID/密码后半段消失，手机看到的是回执页，实际连不上。 */
    if (r->content_len <= 0 || r->content_len > (int)sizeof(body) - 1) {
        ESP_LOGW(TAG, "★ 配网表单长度异常 %d B（缓冲上限 %d）→ 直接判失败",
                 (int)r->content_len, (int)sizeof(body) - 1);
        s_prov_state = 3;
        httpd_resp_send(r, "BAD", 3);
        return ESP_OK;
    }
    int total = (int)r->content_len;
    int got = 0;
    /* ★★ 2026-10-13 任务二 P2-12 修：循环加**总时限 10s**。单次 recv 有 5s 默认超时，
       但慢速客户端每次只吐 1 字节可把唯一 httpd 任务按住数分钟（511B ≈ 34 分钟），
       期间其他手机打不开配网页（开放 AP，任何手机都能触发）。 */
    const int64_t recv_t0 = esp_timer_get_time();
    while (got < total) {
        int n = httpd_req_recv(r, body + got, total - got);
        if (n <= 0) break;
        got += n;
        if ((esp_timer_get_time() - recv_t0) / 1000 >= 10000) {
            ESP_LOGW(TAG, "★ 配网表单接收超过 10s 总时限 → 截断已收 %d/%d B（防慢速客户端拖死 httpd）",
                     got, total);
            break;
        }
    }
    if (got != total) {
        ESP_LOGW(TAG, "★ 配网表单只收到 %d/%d B → 判失败，避免半份表单被当成配网完成", got, total);
        s_prov_state = 3;
        httpd_resp_send(r, "BAD", 3);
        return ESP_OK;
    }
    body[got] = 0;

    char ssid[33] = {0}, pass[65] = {0}, city[64] = {0}, scenery[24] = {0};
    form_get(body, "ssid", ssid, sizeof(ssid));
    /* ★ 2026-10-04：input 的 name 从 'pass' 改成 'pswd_new'（挡住浏览器自动填充），
       这里必须**同步**，否则收不到密码。旧名保留成兜底，万一有旧缓存页面还在提交。 */
    if (!form_get(body, "pswd_new", pass, sizeof(pass))) {
        form_get(body, "pass", pass, sizeof(pass));
    }
    form_get(body, "city", city, sizeof(city));
    form_get(body, "scenery", scenery, sizeof(scenery));
    /* ★★ 2026-10-17（用户）：「我连接 wifi 的时候还是在下面看到了填写 api 的入口，
       这个我不是说了不要了吗，应该是单独的入口」⇒ 配网页**彻底移除** API Key 字段
       （输入框 / 标签 / 说明 / 这里的落盘全删）。远行 API 只从设备侧
       「功能页 2/2 → API接口」这一条路进入，配置走手机门户的独立页（下批）。
       ⚠ 老 NVS 里残留的 api_key 不主动删：g_api_key 读不到就当未配置，
         设备端开关自然显示「未配置 / 去配置」。 */
    /* ★R195：第三个风景「星球」（value=planet）。老页面只发 fuji/pearl，其余一律东方明珠。 */
    int land = LAND_PEARL;
    if (strcmp(scenery, "fuji") == 0)        land = LAND_FUJI;
    else if (strcmp(scenery, "planet") == 0) land = LAND_PLANET;
    ESP_LOGI(TAG, "收到配网：SSID=%s 城市=%s 窗外风景=%s",
             ssid, city, ws_land_name(land));
    fflush(stdout);

    if (ssid[0]) {
        strncpy(s_got_ssid, ssid, sizeof(s_got_ssid) - 1);
        strncpy(s_got_pass, pass, sizeof(s_got_pass) - 1);
        strncpy(s_got_city, city[0] ? city : "上海市", sizeof(s_got_city) - 1);
        s_got_land = land;
        s_prov_state = 1;               /* ★ 2026-09-30：已提交，ws_provision 马上会去试连 */
        s_submit_gen++;                 /* ★ 提交代次（重填一次就 +1，见 ws_provision 注释③） */
        s_submitted  = true;
    } else {
        /* ★2026-10-06 核查修：SSID 为空时原来**没有任何 else** —— 结果页永远停在
           「正在连接…」，40 次轮询后静默停摆，用户不知道自己哪里填错了。 */
        ESP_LOGW(TAG, "★ 配网表单 SSID 为空 → 标记失败，让手机页面立刻看到 fail");
        s_prov_state = 3;
    }
    /* ★ 2026-09-30：这里只是「表单收到了」的回执页 —— 真正的成败由 /status 轮询告知，
       见 SAVE_OK_HTML_ZH 的说明（spec §4.4d：只有设备真的连上才算成功）。
       ★ 2026-10-14 四语化：按设备语言选 中/繁/英/日 套。 */
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const char *ok_html = (g_lang == LANG_ZH)  ? SAVE_OK_HTML_ZH  :
                          (g_lang == LANG_ZHT) ? SAVE_OK_HTML_ZHT :
                          (g_lang == LANG_JA)  ? SAVE_OK_HTML_JA  : SAVE_OK_HTML_EN;
    httpd_resp_send(r, ok_html, strlen(ok_html));
    return ESP_OK;
}

/* -------------------------------------------------------- WiFi 扫描（/scan）*/
/* ★ 2026-09-21 用户反馈 #2：「也没有办法自动搜索附近的 wifi，一定要我自己手动填写了」。
   返回 JSON：{"ok":1,"aps":[{"s":名字,"r":信号dBm,"e":1|0}, …]}，按信号强→弱排，同名前缀去重取最强。
   ★ 扫描必须在 **APSTA** 模式下做（见 softap_start）：纯 AP 模式下扫描会把热点自己踢掉，
     手机正看着这个页面就会断线。APSTA 下手机不掉线，扫完照常回。
   ★ block=true 的扫描约 1–2 秒，跑在 httpd 任务里，期间其它请求排队 —— 配网页只有这一个页面，可以接受。 */
typedef struct { char s[33]; int r; int e; } prov_ap_t;

static void jsonq(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s && n + 7 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')      { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c == '<')              { memcpy(out + n, "\\u003C", 6); n += 6; }
        else if (c == '>')              { memcpy(out + n, "\\u003E", 6); n += 6; }
        else if (c < 0x20 || c == 0x7f) { out[n++] = ' '; }
        else                            { out[n++] = (char)c; }
    }
    out[n] = 0;
}

/* ★★ 2026-09-29 用户反馈 #1：「每次扫描WiFi都很慢」。
   ── 真因：`esp_wifi_scan_start(NULL, true)` 用**默认扫描参数**，而 IDF 默认
      active scan 每信道 `min=120ms / max=1500ms`。13 个信道若频繁撞上 max
      → 最坏 ≈ 19.5s；常态也要 3~6s，用户体感「很慢」。
   ── 修法：显式给 `scan_time`，把每信道探测窗口收成 `min=60 / max=300`，
      并限定 1..13 信道（中国 2.4G 全信道，免去未知信道空等）。
      最坏 ≈ 13×0.3 = 3.9s，常态 1~2s。配网页只有一个页面，够用且明显更快。
   ── ⚠ 别把 max 压到 100ms 以下：弱信号 AP 会在窗口内来不及应答 → 扫不到家里 WiFi
      （那是比"慢"更糟的体验）。300ms 是"扫得到"与"够快"的平衡点。 */
static esp_err_t h_scan(httpd_req_t *r)
{
    ESP_LOGW(TAG, "HTTP GET %s → 开始扫描", r->uri);
    httpd_resp_set_type(r, "application/json; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,     /* channel 0 = 扫全部（下面用 scan_time 限速） */
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = { .min = 60, .max = 300 },        /* ms/信道 —— 见上方注释 */
            .passive = 0,                               /* 主动扫描用不到 */
        },
    };
    const int64_t t0 = esp_timer_get_time();
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    const int64_t took_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGW(TAG, "扫描耗时 %lld ms（active 60~300ms/信道）", (long long)took_ms);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "扫描失败：%s", esp_err_to_name(e));
        httpd_resp_sendstr(r, "{\"ok\":0,\"aps\":[]}");
        return ESP_OK;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    /* ★★ 2026-09-29：`scan_get_ap_num` 给的是**总数**，但取记录时要自己保证缓冲够大 ——
       IDF 文档要求传入的 `*number` 是"缓冲能放几条"，返回时被改写成实际写入数。
       若只按 num 恰好分配、又碰上扫描期间新增/丢失的 AP，就会漏取。
       多留 8 条余量（最多仍读 32 条，配网页够用）。 */
    uint16_t cap = num + 8;
    if (cap > 40) cap = 40;
    if (num > 32) num = 32;

    char *out = calloc(1, 2048);
    prov_ap_t *aps = num ? calloc(num, sizeof(prov_ap_t)) : NULL;
    if (out == NULL || (num && aps == NULL)) {
        free(out); free(aps); esp_wifi_clear_ap_list();
        httpd_resp_sendstr(r, "{\"ok\":0,\"aps\":[]}");
        return ESP_OK;
    }

    int kept = 0;
    if (num) {
        wifi_ap_record_t *rec = calloc(cap, sizeof(wifi_ap_record_t));
        uint16_t got = cap;
        if (rec != NULL && esp_wifi_scan_get_ap_records(&got, rec) == ESP_OK) {
            for (int i = 0; i < got; ++i) {
                if (rec[i].ssid[0] == 0) continue;              /* 隐藏 SSID：跳过 */
                int hit = -1;
                for (int j = 0; j < kept; ++j)
                    if (strncmp(aps[j].s, (const char *)rec[i].ssid, 32) == 0) { hit = j; break; }
                if (hit >= 0) {                                 /* 同名多条 → 只留最强的 */
                    if (rec[i].rssi > aps[hit].r) aps[hit].r = rec[i].rssi;
                } else if (kept < (int)num) {
                    strncpy(aps[kept].s, (const char *)rec[i].ssid, 32);
                    aps[kept].s[32] = 0;
                    aps[kept].r = rec[i].rssi;
                    aps[kept].e = (rec[i].authmode == WIFI_AUTH_OPEN) ? 0 : 1;
                    kept++;
                }
            }
            ESP_LOGW(TAG, "扫描完成：共 %u 条记录（缓冲 %u）→ 去重后 %d 个热点", got, cap, kept);
        }
        free(rec);
    }
    /* ★★ 2026-09-29：**必须**清列表，且要**无条件**清（旧代码只在 kept==0 时清）。
       IDF 的扫描结果存在驱动内部；不 clear，下一次 scan_start 可能拿到旧结果、
       或直接返回 ESP_ERR_WIFI_STATE。表现正是「第二次点扫描扫不到/结果不刷新」。
       取完记录（或取失败）后立刻释放，是 IDF 示例的标准收尾。 */
    esp_wifi_clear_ap_list();

    for (int a = 0; a < kept; ++a)                          /* 信号强→弱（冒泡，最多 32 项） */
        for (int b = a + 1; b < kept; ++b)
            if (aps[b].r > aps[a].r) { prov_ap_t t = aps[a]; aps[a] = aps[b]; aps[b] = t; }

    /* ★★★ 2026-09-30 修复（本文件唯一有安全性质的 bug）：**堆越界写**。
       ── 旧写法把 `snprintf` 的**返回值**（= 若缓冲足够时"应该写"的长度）
          直接累加进 p，而循环闸只卡 `p < 1900`。单条记录最坏 ≈176 字节
          （`jsonq` 把 `<`/`>` 各展成 6 字符 → esc 可到 152 字符），
          于是 p 能冲到 2081 → 末尾 `snprintf(out + p, 2048 - p, …)` 的 size
          **变成负数**，按 size_t 传进去就是天文数字 → 在 calloc(1,2048) 块外写。
          触发条件：十几个名字里带 `<`/`>` 的超长 SSID（恶意 AP 可造）。
       ── 新写法：先格式化进**定长小缓冲 item**（尺寸按最坏值取，绝不可能截断），
          再按**实写长度** p 追加，且追加前先判「本条 + 收尾 "]}” 放不放得下」——
          p 永远 ≤ 2047，`2048 - p` 永远为正。 */
    int p = snprintf(out, 2048, "{\"ok\":1,\"aps\":[");
    if (p < 0) p = 0;
    if (p > 1900) p = 1900;
    for (int a = 0; a < kept; ++a) {
        char esc[160];
        char item[224];                                  /* 最坏 ≈1+6+152+6+4+5+1+1 = 176 B */
        jsonq(esc, sizeof(esc), aps[a].s);
        int il = snprintf(item, sizeof(item), "%s{\"s\":\"%s\",\"r\":%d,\"e\":%d}",
                          a == 0 ? "" : ",", esc, aps[a].r, aps[a].e);
        if (il < 0 || il >= (int)sizeof(item)) continue; /* 单条异常：跳过，不炸缓冲 */
        if (p + il + 3 > 2048) break;                    /* 本条 + 收尾 "]}” 放不下 → 停 */
        memcpy(out + p, item, il);
        p += il;                                         /* ★ 现在 p = 实写长度 */
    }
    memcpy(out + p, "]}", 3);                            /* 含结尾 NUL，共 3 字节 */
    httpd_resp_sendstr(r, out);
    free(out); free(aps);
    return ESP_OK;
}

/* 各种「探测联网状态」的 URL 一律重定向到配网页 —— 让手机自动弹窗 */
static esp_err_t h_redirect(httpd_req_t *r)
{
    ESP_LOGW(TAG, "HTTP GET %s → 302 跳配网页（未登记的路径走这里）", r->uri);
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

/* ★ 2026-09-19 弹窗调优（用户实测：302 档仍要手动输网址）：
   手机系统的连通性探测不吃 302，判定基准是「响应码/正文是否符合预期」——
   · Android  /generate_204           期望 204     → 给 200+内容 = 「需登录」→ 自动弹
   · iOS      /hotspot-detect.html    期望 "Success" → 200+别的 = 弹 CNA 小窗
   · Windows  /ncsi.txt /connecttest.txt /redirect 期望特定正文 → 不符 = 弹「登录」
   所以探测路径改回 **200 OK + HTML**，内容 meta-refresh 跳配网页 ——
   系统弹出的那个小窗里直接就是表单。Cache-Control 禁缓存，防止拿旧结果不弹。 */
/* ★ 2026-10-14 四语化：探测弹窗页也分中/繁/英/日四套，h_portal() 按 g_lang 选。
   这段只会在手机系统弹窗里露一小下，正文极简，不值得为它引入查表机制。 */
static const char PROBE_200_HTML_ZH[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\">"
    "</head><body><p>请打开 "
    "<a href=\"http://192.168.4.1/\">http://192.168.4.1/</a></p></body></html>";
static const char PROBE_200_HTML_EN[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\">"
    "</head><body><p>Please open "
    "<a href=\"http://192.168.4.1/\">http://192.168.4.1/</a></p></body></html>";

/* ── JA 套：与 PROBE_200_HTML_EN 对应，仅可见文案日文化。
   ★ 2026-10-14 四语化新增，由 scripts/_tools/gen_prov_lang.py 生成（勿手改）。── */
static const char PROBE_200_HTML_JA[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\">"
    "</head><body><p>次を開いてください "
    "<a href=\"http://192.168.4.1/\">http://192.168.4.1/</a></p></body></html>";

/* ── ZHT 套：与 PROBE_200_HTML_EN 对应，仅可见文案繁体化。
   ★ 2026-10-14 四语化新增，由 scripts/_tools/gen_prov_lang.py 生成（勿手改）。── */
static const char PROBE_200_HTML_ZHT[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"refresh\" content=\"0; url=http://192.168.4.1/\">"
    "</head><body><p>請開啟 "
    "<a href=\"http://192.168.4.1/\">http://192.168.4.1/</a></p></body></html>";

static esp_err_t h_portal(httpd_req_t *r)
{
    ESP_LOGW(TAG, "HTTP GET %s → 200+配网页（手机连通性探测路径命中）", r->uri);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const char *probe = (g_lang == LANG_ZH)  ? PROBE_200_HTML_ZH  :
                        (g_lang == LANG_ZHT) ? PROBE_200_HTML_ZHT :
                        (g_lang == LANG_JA)  ? PROBE_200_HTML_JA  : PROBE_200_HTML_EN;
    httpd_resp_send(r, probe, strlen(probe));
    return ESP_OK;
}

/* ---------------------------------------------------------------- DNS 劫持 */
/* ★★ 2026-09-30 修复：本任务原来**永远不退出**（`for(;;)` 里 recvfrom 无超时），
   于是「配网结束」后它还挂着 UDP:53 + 6144B 栈跑完整个会话（用户报的耗电项之一）。
   现在：① 加停止标志；② recvfrom 设 500ms 超时 → 循环能定期抬头看标志；
   ③ 退出时自己 close(sock) + vTaskDelete(NULL)，并清 s_dns_alive 让收门户方知道已停。 */
static volatile bool s_dns_stop  = false;
static volatile bool s_dns_alive = false;
static int           s_dns_sock  = -1;

static void dns_task(void *arg)
{
    s_dns_alive = true;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { s_dns_alive = false; vTaskDelete(NULL); return; }
    s_dns_sock = sock;
    /* 500ms 接收超时：没包也定期返回，好让 s_dns_stop 能被及时看到 */
    struct timeval rtv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    struct sockaddr_in sa = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGE(TAG, "★ DNS 劫持绑定 53 端口失败（errno=%d）", errno);
        fflush(stdout);
        close(sock); s_dns_sock = -1; s_dns_alive = false;
        vTaskDelete(NULL); return;
    }
    ESP_LOGW(TAG, "★ DNS 劫持已就绪（UDP 53，所有域名 → 192.168.4.1）");
    fflush(stdout);
    uint8_t buf[512];
    int logged = 0;                     /* 只为诊断：头 12 条查询打到串口，之后闭嘴免得刷屏 */
    for (;;) {
        if (s_dns_stop) break;          /* ★ 收门户时由 prov_portal_stop() 置位 */
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) continue;            /* 超时（EAGAIN）/被信号打断 → 回循环顶看停止标志 */
        if (n < 12) continue;

        /* ★★ 2026-09-21 修复（这是「配网页不自动弹」的第二层原因）：
           旧代码是「把收到的请求原样拷 n 字节，再在屁股后面追加一条 A 记录」。
           但手机解析器普遍带 **EDNS0**（Additional 段里挂着一条 OPT，ARCOUNT=1），
           于是报文实际变成 [头部][问题段][OPT][我们追加的 A 记录]，而 ARCOUNT 还是 1 ——
           解析器把 OPT 当成 answer、把我们的 A 记录当成 additional，
           答案段里就**没有**该域名的 A 记录 → 解析失败 → 连通性探测失败 → 不弹配网页。
           正确做法：只拷「头部 + 问题段」，把 NS/AR 计数归零，再追加自己的 A 记录。 */
        int q = 12;
        while (q < n && buf[q] != 0) {                       /* 跳过一个个标签 */
            if ((buf[q] & 0xC0) == 0xC0) { q += 2; break; }  /* 压缩指针（问题段一般不会出现） */
            q += 1 + buf[q];
        }
        if (q < n && buf[q] == 0) q += 1;                    /* 名字结尾那个 0 */
        q += 4;                                              /* QTYPE + QCLASS */
        if (q > n) q = n;
        uint16_t qtype = (q >= 4) ? (uint16_t)((buf[q - 4] << 8) | buf[q - 3]) : 1;

        uint8_t resp[512];
        memcpy(resp, buf, q);
        resp[2] = 0x81; resp[3] = 0x80;   /* QR=1, RD=1 → RA=1, RCODE=0 */
        /* ★★ 2026-10-04 体检修：原来这三行的偏移整体错了 2 字节，导致 2026-09-21
           那次「ARCOUNT 归零」修复**实际没生效** —— 这很可能就是配网页不自动弹的真正根因。
           DNS 头布局： 0-1 ID / 2-3 flags / **4-5 QDCOUNT** / **6-7 ANCOUNT** /
                        **8-9 NSCOUNT** / **10-11 ARCOUNT**。
           原写法 resp[4..5]=0(标注释 NSCOUNT) 把 QDCOUNT 清了、resp[8..9]=0(标注释 ARCOUNT)
           其实清的是 NSCOUNT，而真正的 ARCOUNT(10,11) 根本没动 → 响应声称 ARCOUNT=1
           却不含 OPT 记录，解析器按 additional 段读就错位；同时 QDCOUNT=0 与「报文里带着
           问题段」自相矛盾，严格解析器直接丢包。
           —— 正确：QDCOUNT 原样透传（绝不覆盖 4/5），后三个计数归零。 */
        /* resp[4..5] = QDCOUNT：不碰，原样透传（我们确实回了一个问题） */
        resp[6] = 0; resp[7] = 0;         /* ANCOUNT = 0（qtype==1 时下面置 1） */
        resp[8] = 0; resp[9] = 0;         /* NSCOUNT = 0 */
        resp[10] = 0; resp[11] = 0;       /* ARCOUNT = 0 ★ 关键：丢掉请求里的 EDNS0 OPT */
        int p = q;

        if (qtype == 1) {                 /* 只有 A 记录可答；AAAA 等一律回「无此记录」 */
            if (p + 16 > (int)sizeof(resp)) continue;   /* ★ 边界保护：绝不越界写栈缓冲 */
            resp[7] = 1;
            resp[p++] = 0xC0; resp[p++] = 0x0C;   /* 指针 → 问题里的域名 */
            resp[p++] = 0x00; resp[p++] = 0x01;   /* TYPE = A */
            resp[p++] = 0x00; resp[p++] = 0x01;   /* CLASS = IN */
            resp[p++] = 0x00; resp[p++] = 0x00;   /* TTL 高 16 位 */
            resp[p++] = 0x00; resp[p++] = 0x3C;   /* TTL = 60s */
            resp[p++] = 0x00; resp[p++] = 0x04;   /* RDLENGTH = 4 */
            resp[p++] = 192; resp[p++] = 168; resp[p++] = 4; resp[p++] = 1;
        }

        if (logged < 12) {                /* 诊断：把手机问的域名打出来，验证劫持真的生效 */
            char nm[64]; int k = 0;
            for (int i = 12; i < q - 4 && k < 60 && buf[i] != 0; ) {
                int len = buf[i++];
                if ((len & 0xC0) == 0xC0 || i + len > q) break;
                if (k) nm[k++] = '.';
                for (int j = 0; j < len && k < 60; ++j) nm[k++] = (char)buf[i + j];
                i += len;
            }
            nm[k] = 0;
            ESP_LOGW(TAG, "DNS 查询：「%s」type=%u → %s", nm, qtype,
                     qtype == 1 ? "答 192.168.4.1" : "答空");
            logged++;
        }
        sendto(sock, resp, p, 0, (struct sockaddr *)&from, fl);
    }
    /* ★ 收门户路径也会走到这里：释放 socket 再自删任务，别把 UDP:53 留着 */
    close(sock);
    s_dns_sock  = -1;
    s_dns_alive = false;
    ESP_LOGW(TAG, "★ DNS 劫持任务已退出（socket 已释放）");
    fflush(stdout);
    vTaskDelete(NULL);
}

/* ★★ 2026-09-30 修复：把配网的「热点 + HTTP 门户 + DNS 劫持 + 扫描出的 STA」整体收掉。
   ── 旧实现无论走哪条出路都只 `httpd_stop()`：softAP、DHCP server、dns_task（6144B 栈）
      全部常驻 —— 配完网设备还带着一个**无密码热点**跑完整个会话（用户报的耗电项）。
   ── keep_sta=true  ：配网成功那条路。把 WiFi 从 APSTA 降成 STA（AP/DHCP 随之下线），
                        **保留刚连上的 STA** —— 紧接着的 ws_sync 要拿它取时间/天气。
   ── keep_sta=false ：长摁 BOOT 跳过那条路。降到 WIFI_MODE_NULL（热点与 STA 都停），
                        后面 ws_sync 需要时会自己按需重启 WiFi 栈。
   ⚠ 只降 mode、不 esp_netif_destroy(AP)：AP netif 对象留着（约 2KB 堆），接口已 down、
      DHCP 已停、功耗归零；一旦 destroy，之后同一次开机想再进配网就没法重建了（低风险选择）。 */
static void prov_portal_stop(httpd_handle_t srv, bool keep_sta)
{
    s_dns_stop = true;                                  /* ① DNS 任务：最多等 1s 让它自己退 */
    for (int i = 0; i < 20 && s_dns_alive; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_alive) {                                  /* 还没退（异常）→ 强删 + 关 socket */
        if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
        ESP_LOGW(TAG, "★ DNS 任务未自行退出 → 强制收尾");
    }
    if (srv) httpd_stop(srv);                           /* ② HTTP 门户 */
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_APSTA) {
        esp_err_t e = esp_wifi_set_mode(keep_sta ? WIFI_MODE_STA : WIFI_MODE_NULL);
        ESP_LOGW(TAG, "★ 配网门户已收（HTTP/DNS 停 · 热点下线 · %s）mode=%s",
                 keep_sta ? "保留已连上的 STA" : "STA 也停", esp_err_to_name(e));
    } else if (!keep_sta && m != WIFI_MODE_NULL) {
        /* ★★ 2026-10-13 任务二 P2-11 修：keep_sta=false（长摁 BOOT 跳过）承诺「STA 也停」，
           但原来只在 APSTA 分支落实；此刻若已是 STA 模式（非常规但可达的路径），
           跳过配网后设备仍连着网 —— 违背「跳过=离线跑」口径且白耗电。这里补齐：
           非 APSTA 且不保留 STA → 一律降到 NULL。 */
        esp_err_t e = esp_wifi_set_mode(WIFI_MODE_NULL);
        ESP_LOGW(TAG, "★ 配网门户已收（HTTP/DNS 停 · 当前已是 mode=%d 非 APSTA → 按不保留 STA 一并停掉）%s",
                 (int)m, esp_err_to_name(e));
    } else {
        ESP_LOGW(TAG, "★ 配网门户已收（HTTP/DNS 停 · 当前 mode=%d，未动 WiFi）", (int)m);
    }
    fflush(stdout);
}

/* ---------------------------------------------------------------- 主流程 */

/* ★ DHCP option 114（RFC 8910 Captive Portal URI）要的值。
   ⚠ 这个字符串必须**活到 DHCP 服务器结束**（ESP-IDF 只存指针，不拷贝），所以是 static。 */
static const char CAPTIVE_PORTAL_URI[] = "http://192.168.4.1/";

static void softap_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    /* ★ 事件循环是**全局单例**：配网这条路（本函数）和 net_sync.c 的 wifi_stack_once()
       都会去创建它，而 esp_event_loop_create_default() 第 2 次调用返回 ESP_ERR_INVALID_STATE。
       以前两边都用 ESP_ERROR_CHECK 包着 → 「手机提交成功 → 紧接着联网」那一下会直接 abort()。
       台面上不容易看出来（NVS 已落，重启后又能连上），但「配网成功」页会一闪而过。
       这里与 net_sync.c 一起改成容忍「已存在」。 */
    esp_err_t el = esp_event_loop_create_default();
    if (el != ESP_OK && el != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(el);

    /* ★2026-10-07：AP netif **只建一次**。配网支持「同会话再进一次」，而
       prov_portal_stop() 故意不 destroy 它（好让第二次能重建）→ 第二次进来它还挂着，
       再 create 一次会去注册重名的 ifkey。 */
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap) ap = esp_netif_create_default_wifi_ap();
    /* ★ 顺手把 **STA netif** 也建出来（只建一次）—— 为的是能在「不踢掉热点」的前提下扫 WiFi：
       纯 AP 模式下 esp_wifi_scan_start() 会让 AP 短暂停摆，手机正看着配网页就会断线；
       切到 APSTA（见下面 set_mode）就能边服务热点边扫描。
       ⚠ 必须判存在性：esp_netif_create_default_wifi_sta() 内部对自己重复调用会 ESP_ERROR_CHECK 失败。 */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    /* ★★ 2026-09-21 新增：DHCP option 114（RFC 8910 Captive Portal URI）。
       Android 11+ / 新鸿蒙 / 新 MIUI 会**直接读这个字段**判定「这是强制门户」并弹登录页，
       比等 HTTP 探测更快也更稳（HTTP 探测那条路现在也修好了，两条一起上）。
       ⚠ 设置 DHCP 选项要求服务器处于**停止**状态 —— 先直接试；
         万一这时已经起来了（返回 ALREADY_STARTED），再走 stop → set → start。
         两种情况都打日志，出问题一眼能看出是卡在哪一步。
       （DNS 那一路不用管：sdkconfig 里 CONFIG_LWIP_DHCPS_ADD_DNS=y 会把 option 6 填成
         热点自身 IP 192.168.4.1，正好喂给 dns_task 的劫持。） */
    esp_err_t o114 = ESP_FAIL;
    if (ap != NULL) {
        o114 = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                      (void *)CAPTIVE_PORTAL_URI, sizeof(CAPTIVE_PORTAL_URI));
        if (o114 == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            esp_netif_dhcps_stop(ap);
            o114 = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                          (void *)CAPTIVE_PORTAL_URI, sizeof(CAPTIVE_PORTAL_URI));
            esp_err_t rs = esp_netif_dhcps_start(ap);
            ESP_LOGW(TAG, "  （DHCP 已在跑，走了一遍 stop→set→start：start=%s）", esp_err_to_name(rs));
        }
    }

    /* 热点名带 MAC 尾字节，避免两台设备重名 */
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) != ESP_OK) {   /* ★2026-10-23 修：读 MAC 失败时用随机尾字节补，避免多台设备热点名都退化成 PetCat-0000 重名 */
        uint32_t rr = esp_random();
        mac[4] = (uint8_t)(rr >> 8); mac[5] = (uint8_t)rr;
        ESP_LOGW(TAG, "★ 读 AP MAC 失败 → 用随机尾字节命名热点");
    }
    char ssid[33];
    snprintf(ssid, sizeof(ssid), PROV_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    wifi_config_t ac = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,       /* 无密码，连上就能配 */
        },
    };
    strncpy((char *)ac.ap.ssid, ssid, sizeof(ac.ap.ssid) - 1);
    /* ★ APSTA 而不是 AP：见上面 STA netif 那段 —— 为了能扫描附近 WiFi 而不掉线。 */
    /* ★★ 2026-10-07 修：这三个 ESP_ERROR_CHECK 是**第二次进配网必崩**的真凶。
       ── 一模一样的坑在 net_sync.c 已修（esp_wifi_start 在栈已 started 时返回
          ESP_ERR_WIFI_NOT_STOPPED，而 ESP_ERROR_CHECK 内部就是 assert → abort 重启）。
       ── 本函数支持「同会话再进一次配网」（1100 行注释明文承诺 = 设置里重新配网），
          第二次进时 WiFi 必然已经是 started（第一次配网成功 → 转 STA → 同步过 → 还在跑）
          ⇒ 撞 NOT_STOPPED ⇒ abort。注释宣称支持、代码必崩，这就是「假承诺」。
       ── 同文件 899 行（event loop）/ 907 行（STA netif）都已判存在性，唯独这三条没跟上。 */
    {
        esp_err_t e1 = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (e1 != ESP_OK) {
            ESP_LOGE(TAG, "★ set_mode(APSTA) 失败（%s）", esp_err_to_name(e1));
            fflush(stdout);
            return;
        }
        esp_err_t e2 = esp_wifi_set_config(WIFI_IF_AP, &ac);
        if (e2 != ESP_OK) {
            ESP_LOGE(TAG, "★ set_config(AP) 失败（%s）", esp_err_to_name(e2));
            fflush(stdout);
            return;
        }
        esp_err_t e3 = esp_wifi_start();
        if (e3 != ESP_OK && e3 != ESP_ERR_WIFI_NOT_STOPPED) {
            ESP_LOGE(TAG, "★ esp_wifi_start 失败（%s）", esp_err_to_name(e3));
            fflush(stdout);
            return;
        }
    }

    ESP_LOGW(TAG, "★ 配网热点已开：SSID=%s（无密码）", ssid);
    ESP_LOGW(TAG, "★ DHCP option 114（门户地址）=%s → %s", CAPTIVE_PORTAL_URI, esp_err_to_name(o114));
    ESP_LOGW(TAG, "★ 手机连上后：正常会自动弹配网页；没弹就手动开浏览器进 http://192.168.4.1/");
    fflush(stdout);
}

static httpd_handle_t prov_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* ★A10 溢出修复（2026-10-19）：实际注册 17 条（root/save/scan/stat/wifi/api×2/
       name/namesave/nameskip/g1~g6/any），旧值 16 会让第 17 条 **u_any 兜底路由注册失败**
       —— 未匹配路径直接 404（门户探测兜不住），且 httpd_register_uri_handler 返回
       ESP_ERR_HTTPD_HANDLERS_FULL 静默吞掉。留 3 条余量防后续加页再踩。 */
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;
    /* ★★★ 2026-09-21（第十二版之后）根因修复之二：**httpd 任务栈不够** ★★★
       `HTTPD_DEFAULT_CONFIG()` 给的 `stack_size = 4096`，而本文件的 `h_root()`
       在第十一版被改成「分三段发」之后，栈上多了 `prev[1024]` + `build_prev_js()` 的
       `ssid/pass/sc/city + e1..e4`（≈1150B）≈ **2.2KB 局部变量**，再叠上 httpd 自身
       解析请求的帧 → **一请求 `GET /` 就栈溢出 → panic 重启**。
       台面现象极具误导性：手机探测/弹窗/手动敲 192.168.4.1 全都打不开，
       墨水屏**反复闪**（每次 panic 重启都会重画配网引导页）—— 看着像「配网没生效」，
       其实是设备一直在重启。IDF 文档对 stack_size 的原话就是
       「handler 用得多就必须自己调大」。给到 8KB，另把 h_root 的大缓冲挪到堆上。 */
    cfg.stack_size = 8192;
    /* ★★★ 2026-09-21 根因修复（配网不自动弹窗的真正原因）★★★
       本工程之前一直以为「uri 以 * 结尾，ESP-IDF 就会自动前缀匹配」—— **错的**。
       esp_http_server 的默认匹配器是 `httpd_uri_match_simple()`（**精确**比较长度+内容），
       只有把 `cfg.uri_match_fn` 显式指成 `httpd_uri_match_wildcard`，`*` 才会生效。
       （证据：esp-idf/components/esp_http_server/src/httpd_uri.c:102 ——
         `hd->config.uri_match_fn ? hd->config.uri_match_fn(...) : httpd_uri_match_simple(...)`，
         而 HTTPD_DEFAULT_CONFIG() 里 `.uri_match_fn = NULL`。）
       所以旧代码里注册的 `/generate_204*`、`/hotspot-detect.html*`、`/ *` **一条都命中不了**
       （连 `/generate_204` 本身都因为模板多了个 `*` 而长度不等）→ 手机探测一律吃 **404** →
       判定「这网没被劫持」→ 既不弹配网页，也不给联网。手动敲 192.168.4.1 才进得来。
       ★ 打开通配匹配后：`/`、`/save`、`/scan` 仍是精确匹配（模板无 `*`），不会被 `/ *` 抢走。 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv = NULL;
    esp_err_t he = httpd_start(&srv, &cfg);
    if (he != ESP_OK) {
        ESP_LOGE(TAG, "★ httpd_start 失败：%s（配网页将打不开！）", esp_err_to_name(he));
        fflush(stdout);
        return NULL;
    }
    ESP_LOGW(TAG, "★ 配网页 HTTP 服务已启动（80 端口，栈 8KB，17 条路由）");
    fflush(stdout);

    /* ★ 注册顺序 = 匹配顺序：无 `*` 的精确路径必须排在前，否则会被 `/ *` 兜底抢走 */
    httpd_uri_t u_root = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t u_scan = { .uri = "/scan", .method = HTTP_GET,  .handler = h_scan };
    /* ★ 2026-09-30：/status —— 提交后手机页面轮询连接结果（见 SAVE_OK_HTML_ZH） */
    httpd_uri_t u_stat = { .uri = "/status", .method = HTTP_GET, .handler = h_status };
    /* ★★★ 2026-10-19 API 配置页（前端批次）：
       /wifi = 强制出 WiFi 表单页（API 页顶部的「先去配 WiFi」链接）；
       /api  = GET 出 API 配置页 · POST 保存 key/url 到 NVS。
       三条都是**精确路径**，注册在通配兜底路由之前才不会被抢走（见下方顺序说明）。 */
    httpd_uri_t u_wifi = { .uri = "/wifi", .method = HTTP_GET,  .handler = h_wifi };
    httpd_uri_t u_api  = { .uri = "/api",  .method = HTTP_GET,  .handler = h_api_page };
    httpd_uri_t u_apis = { .uri = "/api",  .method = HTTP_POST, .handler = h_api_save };
    /* 探测路径统一回 h_portal（200+网页）；其余未知路径走 h_redirect（302）。
       ★ 后缀 * 现在**真的**是通配了（见上面 uri_match_fn）—— 手机在 /generate_204
       后面拼随机 UUID 也能兜住（2026-09-19 实机日志里出现过 `/generate_204_<uuid>`）。 */
    httpd_uri_t u_g1 = { .uri = "/generate_204*",         .method = HTTP_GET, .handler = h_portal };  /* Android */
    httpd_uri_t u_g2 = { .uri = "/hotspot-detect.html*",  .method = HTTP_GET, .handler = h_portal };  /* iOS */
    httpd_uri_t u_g3 = { .uri = "/hotspot_detect.html*",  .method = HTTP_GET, .handler = h_portal };  /* iOS 旧版（下划线） */
    httpd_uri_t u_g4 = { .uri = "/connecttest.txt*",      .method = HTTP_GET, .handler = h_portal };  /* Windows */
    httpd_uri_t u_g5 = { .uri = "/ncsi.txt*",             .method = HTTP_GET, .handler = h_portal };  /* Windows */
    httpd_uri_t u_g6 = { .uri = "/redirect*",             .method = HTTP_GET, .handler = h_portal };  /* Windows 11 */
    httpd_uri_t u_any = { .uri = "/*",                    .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(srv, &u_root);
    httpd_register_uri_handler(srv, &u_save);
    httpd_register_uri_handler(srv, &u_scan);
    httpd_register_uri_handler(srv, &u_stat);
    httpd_register_uri_handler(srv, &u_wifi);   /* ★2026-10-19 */
    httpd_register_uri_handler(srv, &u_api);
    httpd_register_uri_handler(srv, &u_apis);
    /* ★ 2026-10-12 取名功能 */
    httpd_uri_t u_name = { .uri = "/name",     .method = HTTP_GET,  .handler = h_name };
    httpd_uri_t u_nsav = { .uri = "/namesave", .method = HTTP_POST, .handler = h_namesave };
    httpd_uri_t u_nskp = { .uri = "/nameskip", .method = HTTP_GET,  .handler = h_nameskip };
    httpd_register_uri_handler(srv, &u_name);
    httpd_register_uri_handler(srv, &u_nsav);
    httpd_register_uri_handler(srv, &u_nskp);
    httpd_register_uri_handler(srv, &u_g1);
    httpd_register_uri_handler(srv, &u_g2);
    httpd_register_uri_handler(srv, &u_g3);
    httpd_register_uri_handler(srv, &u_g4);
    httpd_register_uri_handler(srv, &u_g5);
    httpd_register_uri_handler(srv, &u_g6);
    httpd_register_uri_handler(srv, &u_any);
    return srv;
}

/* ★「配网页长摁 BOOT 跳过」——————————————————————————————————————
   来由（两批用户反馈叠加）：
     ① 「假如用户没有配置 WiFi 的条件，岂不是用都用不了」—— 进了配网页就只能干等超时，
        所以要有逃生通道。
     ② ★ 2026-09-19 用户订正：**手势是「长摁 BOOT」**，不是我先前实现成的短摁 ——
        设计稿 `D_配网/配网引导UI.png` 底部那个按钮上写的就是「**长摁Boot跳过配网流程**」，
        实现成短摁会和屏上文案自相矛盾。
        语义也顺：长按 = 有意的操作（配网是件正经事，不该被手里随便一碰就跳过去）。

   ★ 为什么在这里直接读 GPIO、而不是复用 main.c 的按键扫描：
     ws_provision() 跑在 app_main 里、是**阻塞**的，main.c 那个 20ms 按键扫描
     循环此时还没开始（要等它出来才轮到）。所以这里自己读电平。
     BOOT=GPIO0，在更早的 board 初始化里已经 gpio_config 成上拉输入，读电平是安全的。 */
#define PROV_PIN_BOOT     0
/* ★R192：POWER 键（与 main.c 的 PIN_BTN_PWR 同源 = GPIO18，按下为 0）。
   这里单独定义是因为 net_prov.c 不 include main.c 的私有引脚宏。 */
#define PROV_PIN_PWR     18
#define PROV_SKIP_HOLD_MS 500    /* 长按判定：与 main.c「BOOT 长按 0.5–2.0s」同一档
                                   （★0928 曾随全局下沿 1.5→1.0；★2026-10-03 全局删死区改 0.5） */
/* ★ 2026-09-30 新增（配合 spec §4.4d「只有真连上才算配网成功」）：
   PROV_VERIFY_MS    = 提交后试连本条的等待上限。给 12s：家用路由器 DHCP 一般 3~6s，
                       弱信号/2.4G 拥挤时给到 12s 足够，又不至于让用户在手机前干等太久。
   PROV_FAIL_HOLD_MS = 试连失败后，让手机页面把「连接失败」看清的最短停留（之后才复位等重填）。 */
#define PROV_VERIFY_MS    12000
#define PROV_FAIL_HOLD_MS 12000

static bool prov_boot_down(void)
{
    return gpio_get_level((gpio_num_t)PROV_PIN_BOOT) == 0;
}

/* ★R192：POWER 键电平（按下 = 0）。取名页用它做「确定」。 */
static bool prov_pwr_down(void)
{
    return gpio_get_level((gpio_num_t)PROV_PIN_PWR) == 0;
}

/* ★★ 2026-10-05（触屏版，用户订正）：配网页「跳过」不许**点一下就走** ——
   必须**按住底部那颗按钮 3 秒**。原因与完整口径见 net_sync.h 的 `ws_prov_set_touch_probe`。
   3 秒这个值：比「手抖/不小心蹭到」长一个数量级，又不至于让人按到手酸；
   与全局长按档位（BOOT 0.5s 返回 / 2s 回首页 / POWER 2s 关机）拉开明显层级 ——
   越是不可逆的动作，门槛越长。 */
#define PROV_TOUCH_SKIP_HOLD_MS 3000

static prov_touch_hold_fn s_touch_probe_fn = NULL;
void ws_prov_set_touch_probe(prov_touch_hold_fn fn) { s_touch_probe_fn = fn; }

/* ★R206：配网页的关机执行体（main.c 注册 = power_off，内部不返回）。 */
static prov_poweroff_fn s_poweroff_fn = NULL;
void ws_prov_set_poweroff_fn(prov_poweroff_fn fn) { s_poweroff_fn = fn; }

/* ★R206（用户令）：「关机在任何页面都要能用」。配网页阻塞在 ws_provision 里，
   主循环/按键任务都不跑，所以 POWER 只能在这里轮询 —— 按住 ≥2s（与全站同档）即关机，
   且**到点即触发**（不等松手，和问卷页口径一致）。返回 true = 已调用关机（不返回）。 */
static bool prov_check_poweroff(void)
{
    static int64_t pwr_dn = -1;
    if (!prov_pwr_down()) { pwr_dn = -1; return false; }
    const int64_t now = esp_timer_get_time();
    if (pwr_dn < 0) pwr_dn = now;
    const int held = (int)((now - pwr_dn) / 1000);
    if (held < 2000) return false;
    ESP_LOGW(TAG, "★ 配网页 POWER 按住 %dms（≥2000）→ 关机", held);
    fflush(stdout);
    if (s_poweroff_fn) s_poweroff_fn();          /* 不返回 */
    return false;   /* 没注册执行体（理论上不会有）→ 继续配网 */
}

/* ★ 长按 BOOT 的跳过判定抽成一个函数：配网页等待循环 / 试连失败的回填等待循环都要用。
   ★2026-10-05：再加一条**触屏按住**判据（底部跳过按钮 ≥3s），两条走同一出口。 */
static bool prov_check_skip(int *hold_ms)
{
    prov_check_poweroff();                       /* ★R206：先看 POWER 长按（任一页都能关机）*/
    if (s_touch_probe_fn) {
        int tms = s_touch_probe_fn();            /* 0 = 没按住 / 松手（计时由 main.c 负责清零） */
        if (tms >= PROV_TOUCH_SKIP_HOLD_MS) {
            ESP_LOGW(TAG, "★ 触屏「跳过」按钮按住 %dms（≥%d） → 跳过配网，直接进主页（随机天气/地点）",
                     tms, PROV_TOUCH_SKIP_HOLD_MS);
            fflush(stdout);
            return true;
        }
    }
    /* ★2026-10-06 核查修（严重）：原来是 `*hold_ms += 50` —— 一个**按调用次数累加**的
       计数器，却被两个**不同节拍**的循环调用：配网页等待循环 50ms 一拍（0.5s 生效，正确），
       而「试连失败」停留窗口是 **500ms 一拍**（1141 行）⇒ 攒够 500ms 要 10 拍 =
       **真实按住 5 秒**，且窗口总共才 12 秒 —— 用户在失败页按 0.5s 毫无反应，以为死机。
       ⇒ 改成时间戳法：按下记起点，时长与调用周期**无关**。 */
    static int64_t t_down_us = -1;          /* BOOT 按下起点（-1 = 未按） */
    if (prov_boot_down()) {
        int64_t now_us = esp_timer_get_time();
        if (t_down_us < 0) t_down_us = now_us;
        *hold_ms = (int)((now_us - t_down_us) / 1000);
        if (*hold_ms >= PROV_SKIP_HOLD_MS) return true;
    } else {
        t_down_us = -1;                     /* 松手就重新计 */
        *hold_ms = 0;
    }
    return false;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ★★★ 2026-10-19 API 配置模式（用户令 · API 前端批次）
 * ---------------------------------------------------------------------------
 * 入口：设备端「功能页 2/2 → API接口」——
 *   · 开关（**未配置**时）→ api_toggle() 内部跳这里；
 *   · 底部「去配置 / 重新配置」（**无论是否已配**）→ 跳这里。
 * 两者都是 `prov_again_set_goal(PROV_AGAIN_API)` + `esp_restart()` → 冷启动时
 * main.c 判 `prov_again_get() == 2` → 调本函数（**不走** WiFi 表单那套流程）。
 * 流程：开热点（无密码 + DHCP option 114）→ 手机连上后 h_root 看到目标=API
 *   → **直接出 API 配置页**（POST /api 保存到 NVS）→ 等保存或长按 BOOT 跳过
 *   → 留 2s 让手机看完「已保存」→ 收热点（keep_sta=true，**保留设备原有 STA**）。
 * ⚠ 与 ws_provision 的区别：**不试连、不写 WiFi 列表** —— 设备本来就有 WiFi，
 *   这次只是去改 API 的 key/url，原有网络与 NVS 配置一个字节不动。
 * ═══════════════════════════════════════════════════════════════════════════ */
bool ws_api_provision(void)
{
    s_api_saved = false;
    softap_start();                          /* 复用常态：开热点 + DHCP option 114 */
    httpd_handle_t srv = prov_http_start();
    xTaskCreate(dns_task, "dns_hijack", 6144, NULL, 5, NULL);

    ESP_LOGW(TAG, "★ API 配置模式：热点已开 —— 手机连上后**直接弹出 API 配置页**");
    ESP_LOGW(TAG, "★ 手机没自动弹窗就手动开浏览器进 http://192.168.4.1/");
    fflush(stdout);

    /* 冷启动时用户可能正按着 BOOT，先等它松开（同 ws_provision 口径） */
    for (int i = 0; i < 60 && prov_boot_down(); ++i) vTaskDelay(pdMS_TO_TICKS(50));

    /* 等手机保存（s_api_saved）或长按 BOOT 跳过（既有逃生通道） */
    int hold_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (s_api_saved) break;
        if (prov_check_skip(&hold_ms)) {
            ESP_LOGW(TAG, "★ 长摁 BOOT %dms → 退出 API 配置（未保存）", hold_ms);
            fflush(stdout);
            prov_portal_stop(srv, true);
            return false;
        }
    }
    /* 让手机把「已保存」页看完（约 2s）再收热点，否则页面会显示「连接已断开」 */
    for (int i = 0; i < 4; ++i) vTaskDelay(pdMS_TO_TICKS(500));
    prov_portal_stop(srv, true);             /* keep_sta=true：设备原有 STA 不动 */
    ESP_LOGW(TAG, "★ API 配置完成 → 收热点（设备原有 WiFi 不受影响）");
    fflush(stdout);
    return true;
}

/* ★R192：取名默认名（跟随设备语言）。中 猫猫 / 英 Mio / 日 ミオ / 繁 貓貓。
   ⚠ 必须**逐个判等**，不能写 `g_lang != LANG_ZH`：那样日文态会被英文支抢走。
   三个默认名都已在字库内（改默认名务必先过 zf_has_title_glyph()，否则上屏是方框）。 */
static const char *name_default(void)
{
    return (g_lang == LANG_JA)  ? "ミオ"
         : (g_lang == LANG_EN)  ? "Mio"
         : (g_lang == LANG_ZHT) ? "貓貓" : "猫猫";
}

bool ws_provision(ws_config_t *cfg)
{
    s_submitted  = false;
    s_prov_state = 0;
    s_dns_stop   = false;                   /* ★ 允许「同一会话里再进一次配网」（设置里重新配网） */
    s_got_ssid[0] = s_got_pass[0] = s_got_city[0] = 0;
    s_got_land = LAND_PEARL;

    softap_start();
    httpd_handle_t srv = prov_http_start();
    /* ★ 栈 3072→6144：第十一版重写后 dns_task 栈上有 buf[512] + resp[512] = 1KB，
       再叠 lwIP 的 recvfrom/sendto 帧，3072 太贴边（溢出 = panic 重启）。 */
    xTaskCreate(dns_task, "dns_hijack", 6144, NULL, 5, NULL);

    /* ★ 冷启动时用户可能正按着 BOOT（开机那一下就按着），先等它松开再开始判长按，
       否则刚进配网页就会被「上一个按下的手指」直接跳过去。 */
    for (int i = 0; i < 60 && prov_boot_down(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* ★★ 2026-09-19 用户订正配网逻辑（第三版口径）：
       · 有记忆（NVS 有 WiFi 配置）→ 自动连接，不进配网页 —— 在 main.c 里判，不在这；
       · 没记忆 → 配网页**不会自动跳过**（旧版 180 秒超时即跳过 = 用户看到了「没配网
         却直接进主页」的现象，已删）；
       · 出路：① 手机提交成功**且设备真的连上**（返回 true）② 长摁 BOOT ≥1s 跳过；
       · 跳过之后 = 随机天气 + 随机地点（main.c 里掷骰子），想改到设置里重新配网。

    ★★ 2026-09-30 修复（spec §4.4d：**只有设备真的连上**才进成功页）：
       ── 旧实现：h_save 里只要表单有 ssid 就 s_submitted=true，ws_provision 看到标志
          立刻 return true —— **从不验证能不能连上**。密码填错也会显示「配网成功」并把
          错凭据写进 wifi_add；而 main.c:6161 那行注释明文写着要验证。注释与实现互相矛盾。
       ── 新实现：拿到表单后在 **APSTA** 下用 ws_wifi_switch() 同步试连一次
          （手机仍挂在热点上，不会断线）：连上 → 成功；连不上 → 把失败结果发给手机
          （页面轮询 /status），**留在配网页继续等重填**，不写 NVS。 */
    int  hold_boot_ms = 0;
    bool connected    = false;
    for (;;) {
        /* ① 等手机提交（期间可长按 BOOT 跳过） */
        while (!s_submitted) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (prov_check_skip(&hold_boot_ms)) {
                ESP_LOGW(TAG, "★ 长摁 BOOT %dms → 跳过配网，直接进主页（随机天气/地点）",
                         hold_boot_ms);
                fflush(stdout);
                prov_portal_stop(srv, false);      /* ★ 收热点/DNS/HTTP，别再挂着耗电 */
                return false;
            }
        }

        /* ② 试连本条凭据（这是「配网成功」的唯一判据） */
        int gen = s_submit_gen;
        ESP_LOGW(TAG, "★ 收到配网表单 → 试连「%s」验证凭据（最多 %d ms，手机不掉线）…",
                 s_got_ssid, PROV_VERIFY_MS);
        fflush(stdout);
        connected = ws_wifi_switch(s_got_ssid, s_got_pass, PROV_VERIFY_MS);
        if (connected) {
            /* ★2026-10-23 修（凭据竞态）：试连期间用户重填提交会覆盖 s_got_*，
               若此刻 break 会把"未验证的新凭据"写进 NVS（连上的是旧凭据）——
               gen 变了就回循环顶部用新凭据重试。 */
            if (s_submit_gen != gen) { s_prov_state = 3; continue; }
            s_prov_state = 2; break;
        }

        /* ③ 连不上：告诉手机「失败」，留在配网页等重填 */
        s_prov_state = 3;
        ESP_LOGE(TAG, "★ 试连「%s」失败 → 不写 NVS、不进成功页；回配网页继续等", s_got_ssid);
        fflush(stdout);
        hold_boot_ms = 0;
        for (int i = 0; i < PROV_FAIL_HOLD_MS / 500; ++i) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_submit_gen != gen) break;        /* 用户已重填提交 → 立刻再试 */
            if (prov_check_skip(&hold_boot_ms)) {
                ESP_LOGW(TAG, "★ 长摁 BOOT %dms → 跳过配网", hold_boot_ms);
                fflush(stdout);
                prov_portal_stop(srv, false);
                return false;
            }
        }
        /* ★2026-10-06 核查修：原来把 s_prov_state 从 3（失败）退回 0（等待中）——
           手机此时刷新/重开结果页会看到「正在连接…」并空转到 40 次后**静默停摆**。
           保留 3（失败）即可：用户重填提交时 h_save 会把它设回 1。 */
        if (s_submit_gen == gen) {                 /* 没人重填 → 复位，等下一次提交 */
            s_submitted  = false;
        }
    }

    /* 成功后先让手机那一轮 /status 轮询拿到 ok（最多 3s），再收热点 —— 否则页面只会看到连接断开 */
    for (int i = 0; i < 6; ++i) vTaskDelay(pdMS_TO_TICKS(500));

    /* ══ 2026-10-12 取名子流程（挂载点：连网成功、NVS 落盘前）══
       设备屏先画「命名等待屏」(scr_B)；手机在 /name 填表提交 /namesave 通过校验即采用；
       手机页另有「用默认名猫猫」按钮（等同提交「猫猫」）与「暂不取名」链接（GET /nameskip）；
       设备 BOOT 短按 → 进「默认名二次确认」(scr_B2)，再短按=用默认名「猫猫」、长按=返回等待屏；
       退出判定（任一满足即「暂不取名」，不写 pet_name，状态页显示 -）：
         · 手机点「暂不取名」→ s_name_skip（★ 2026-10-12 修：原来手机页 href='/' 只导航不回设备）
         · 180s 硬上限
         · 90s 内无任何 HTTP 请求（心跳超时）
         · 收过请求后连续 15s 无请求（手机离热点）
       退出后把名字（或默认名）写进 NVS pet_name；named=false 则不写。
       ★★ 2026-10-14 R44 修（用户实机：「已经给猫取过名了，连 WiFi 还会再次取名」）：
         原实现挂在配网成功路径上**无条件进入** —— 老设备重配网（换路由器/重置 WiFi）
         会被迫再取一次名，而 /name 页自己写着「取名后不可更改，除非重置设备」⇒ 口径自相矛盾。
         现加门控：NVS petns:pet_name 已有非空名字 → **整段跳过**。想重新取名 =
         factory_reset_all（全清）或测试页 pet_name_clear()（只清名），之后配网会照常弹出。 */
    bool r44_have_name = false;
    {
        nvs_handle_t h0;
        if (nvs_open("petns", NVS_READONLY, &h0) == ESP_OK) {
            char cur[16] = {0};
            size_t cl = sizeof(cur);
            /* ★ "-" 是「未取名」的哨兵值，不算名字（原来 cur[0] 非空就当成有名字了）。 */
            if (nvs_get_str(h0, "pet_name", cur, &cl) == ESP_OK && cur[0] && strcmp(cur, "-") != 0)
                r44_have_name = true;
            /* ★★ 2026-10-17（用户）：「我已经给我的猫取过名字了，但还会弹出取名的后续页面」。
               光看 pet_name 有个洞：命名流程若以「暂不取名」退出（或手机那头没提交成功），
               NVS 里就没有 pet_name ⇒ 下次配网**再问一遍**，体感就是「取过名了还一直让我取」。
               另加一个一次性闸 named_once：**取名成功时**置位。
               ★★ R94 订正：旧注释写的「流程收尾时无条件置位（取了名/暂不取/超时都算跑过）」
               正是这次用户 bug 的病根 —— 取名失败也置位，名字永远 "-" 却再也不问。
               现在① 只有真取到名字才置位（写在 h_namesave 落盘处）；② 本处判据只看 pet_name，
               named_once 不再参与"要不要弹取名页"的判断（重新取名的正规路径仍是
               恢复出厂 / 测试页清名 / 改名卡）。 */
            int32_t once = 0;
            (void)once;
            nvs_close(h0);
        }
    }
    if (r44_have_name) {
        ESP_LOGW(TAG, "★ 已有名字 → 跳过取名子流程（取名后不可更改；重置设备/清名后才再弹）");
        fflush(stdout);
    }
    if (!r44_have_name) {
    pet_draw_naming_wait();
    s_name_saved = false; s_name_skip = false; s_name_buf[0] = 0;
    s_name_act_ms = esp_timer_get_time() / 1000;
    {
        bool named = false;
        char nm[16]; nm[0] = 0;
        int64_t t0 = esp_timer_get_time() / 1000;
        int64_t press_start = 0, pwr_start = 0;
        bool boot_prev = false, pwr_prev = false, confirming = false;
        /* ★R192 用户令（按键版口径）：**BOOT 短按 = 选中（按钮反白）/ 切换到下一项，
           POWER 短按 = 确定**。scr_B 只有一个「暂不取名」，选中后 POWER 即跳过；
           scr_B2 两颗（左返回 / 右确定），BOOT 来回切、POWER 执行。
           （原来 BOOT 短按直接进确认页、再短按=默认名 —— 与全站「BOOT 选 / POWER 定」不一致。）*/
        int sel = -1;          /* scr_B：>=0 = 按钮选中（反白）*/
        int csel = -1;         /* scr_B2：-1 无 / 0 左返回 / 1 右确定 */
        /* ★R97（用户拍板）：LEAVE_MS 15s → **30s** —— 手机那边要人自己点「给猫猫取名」，
           15 秒太赶（结果页拿到 ok 后就不再轮询，一超时就静默收热点）。页面倒计时跟着
           改成 29s（比这里早 1 秒留余量），见 NAME_GONE_* 上方的说明。 */
        const int64_t NAMING_CAP_MS = 180000, HEART_MS = 90000, LEAVE_MS = 30000;
        const int64_t SHORT_MS = 250, LONG_MS = 800;
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(50));
            int64_t now = esp_timer_get_time() / 1000;
            int64_t el = now - t0;
            if (s_name_saved) {
                strncpy(nm, s_name_buf, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
                named = true; break;
            }
            if (s_name_skip) break;   /* ★ 手机点「暂不取名」→ 立即退出（named=false，不写 NVS） */
            /* ★R192：**触屏**（用户实报「点『用默认名』没反应」「卡死在取名页出不去」）。
               这段循环原来只读 BOOT 按键；触屏版必须自己轮询 tp_poll，否则整页是死页。 */
            {
                const int ntk = pet_naming_touch_probe(confirming ? 1 : 0);
                if (ntk & PET_NAME_TOUCH_SKIP) {
                    ESP_LOGW(TAG, "★ 取名页：点「暂不取名」/ 左上返回 → 跳过（pet_name 保持 -，下次配网再问）");
                    fflush(stdout);
                    break;                      /* named=false → 不写 pet_name */
                }
                if (ntk & PET_NAME_TOUCH_CANCEL) {
                    ESP_LOGW(TAG, "★ 取名确认页：点「返回」→ 回等待屏");
                    fflush(stdout);
                    confirming = false; pet_draw_naming_wait();
                } else if (ntk & PET_NAME_TOUCH_OK) {
                    strncpy(nm, name_default(), sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
                    ESP_LOGW(TAG, "★ 取名确认页：点「确定」→ 用默认名「%s」", nm);
                    fflush(stdout);
                    named = true; break;
                }
            }
            /* ══ 按键（两版共用；触屏版同样可用实体键）══
               BOOT 短按 = 选中/切换（按钮反白）；BOOT 长按 = scr_B 进确认页 / scr_B2 回等待屏；
               POWER 短按 = 确定当前选中项。 */
            const bool down    = prov_boot_down();
            const bool p_down  = prov_pwr_down();
            if (down && !boot_prev) press_start = now;            /* BOOT 上升沿 */
            if (p_down && !pwr_prev) pwr_start = now;             /* POWER 上升沿 */
            /* ★R206（用户令「关机在任何页面都要能用」+「关机不顺畅」）：
               取名页最长停留 180s、且阻塞在 ws_provision 里（主循环/按键任务都不跑）
               ⇒ POWER 长按只能在这里自己判：**到点即关**（按住满 2s 当场关机，不等松手）。 */
            if (p_down && pwr_prev && pwr_start && now - pwr_start >= 2000 && s_poweroff_fn) {
                ESP_LOGW(TAG, "★ 取名页 POWER 按住 %lldms（≥2000）→ 关机【到点即触发】",
                         (long long)(now - pwr_start));
                fflush(stdout);
                s_poweroff_fn();                          /* 不返回 */
            }
            if (!down && boot_prev) {                             /* BOOT 释放边 */
                const int64_t dur = now - press_start;
                if (dur >= SHORT_MS && dur < LONG_MS) {           /* 短按 = 选中 / 切换 */
                    if (!confirming) {
                        sel = (sel < 0) ? 0 : -1;
                        ESP_LOGW(TAG, "★ 取名页：BOOT 短按 → %s", sel >= 0 ? "选中「暂不取名」（反白）" : "取消选中");
                        pet_draw_naming_wait_sel(sel);
                    } else {
                        csel = (csel < 0) ? 1 : (csel == 0 ? 1 : 0);
                        ESP_LOGW(TAG, "★ 取名确认页：BOOT 短按 → 选中「%s」", csel == 1 ? "确定" : "返回");
                        pet_draw_naming_confirm_sel(csel);
                    }
                } else if (dur >= LONG_MS) {                      /* 长按 = 换屏 */
                    if (!confirming) {
                        confirming = true; csel = -1;
                        ESP_LOGW(TAG, "★ 取名页：BOOT 长按 → 默认名确认页");
                        pet_draw_naming_confirm();
                    } else {
                        confirming = false; sel = -1;
                        ESP_LOGW(TAG, "★ 取名确认页：BOOT 长按 → 回取名页");
                        pet_draw_naming_wait();
                    }
                }
                fflush(stdout);
            }
            if (!p_down && pwr_prev) {                            /* POWER 释放边 = 确定 */
                const int64_t dur = now - pwr_start;
                if (dur >= SHORT_MS && dur < LONG_MS) {
                    if (!confirming) {
                        if (sel >= 0) {                           /* 选中「暂不取名」→ 跳过 */
                            ESP_LOGW(TAG, "★ 取名页：POWER 确定「暂不取名」→ 跳过（pet_name 保持 -）");
                            fflush(stdout);
                            break;
                        }
                    } else if (csel == 1) {                       /* 右侧「确定」= 用默认名 */
                        strncpy(nm, name_default(), sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
                        ESP_LOGW(TAG, "★ 取名确认页：POWER 确定 → 默认名「%s」", nm);
                        fflush(stdout);
                        named = true; break;
                    } else if (csel == 0) {                       /* 左侧「返回」= 回等待屏 */
                        confirming = false; sel = -1;
                        ESP_LOGW(TAG, "★ 取名确认页：POWER 确定「返回」→ 回取名页");
                        fflush(stdout);
                        pet_draw_naming_wait();
                    }
                }
            }
            boot_prev = down;
            pwr_prev  = p_down;
            int64_t idle = now - s_name_act_ms;
            if (el >= NAMING_CAP_MS) break;
            /* ★2026-10-23 修（M13）：区分"还没连上"与"连上后离开"——
               子流程开始即把 s_name_act_ms 置 now，若从未通信则 idle≈el；一旦收到请求
               s_name_act_ms 被刷新 ⇒ idle 明显小于 el。据此：从未通信给 HEART_MS(90s) 等
               手机连上来；通信过后才用 LEAVE_MS(30s) 判离开。原写法两条同时生效 ⇒ 实际窗口
               仅 ~15s，宣称的 90s 心跳永不生效。 */
            if (idle >= HEART_MS) break;                              /* 从未通信：90s 首连宽限 */
            if (el > idle + 1000 && idle >= LEAVE_MS) break;          /* 已通信：30s 无新请求才离 */
        }
        if (named) {
            nvs_handle_t h2;
            if (nvs_open("petns", NVS_READWRITE, &h2) == ESP_OK) {
                nvs_set_str(h2, "pet_name", nm);
                /* ★R94：只有真取到名字才落盘。★R171 全量检查：named_once 这道
                   一次性闸早已停用（门控只看 pet_name），全工程 0 处读取 → 不再写。 */
                nvs_commit(h2);
                nvs_close(h2);
            }
            ESP_LOGW(TAG, "★ 取名子流程：已取名「%s」", nm);
        } else {
            ESP_LOGW(TAG, "★ 取名子流程：本轮没取名（pet_name 保持 -，下次配网仍会再问 —— 与 /name 页文案一致）");
        }
        fflush(stdout);
    }
    }   /* ★R44：闭合上方 if (!r44_have_name) —— 已有名字时整段取名流程不进入 */

    /* 写进 cfg 并落 NVS */
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->ssid, s_got_ssid, sizeof(cfg->ssid) - 1);
    strncpy(cfg->pass, s_got_pass, sizeof(cfg->pass) - 1);
    strncpy(cfg->city, s_got_city, sizeof(cfg->city) - 1);
    cfg->land = s_got_land;

    /* ★ lat/lon 先放「出厂兜底」（上海）：配网时设备跑在 AP 模式、**没有外网**。
       真正生效的坐标在第一次连上 WiFi 后由 ws_resolve_city() 查
       city_coords.h（离线烘焙的「省+市 → 经纬度」表）得到并落 NVS。
       时区同理 —— 第一次取天气时用 `timezone=auto` 回填。 */
    cfg->lat = 31.23f;
    cfg->lon = 121.47f;
    cfg->tz_offset_sec = 8 * 3600;
    cfg->valid = true;

    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", cfg->ssid);
        nvs_set_str(h, "pass", cfg->pass);
        nvs_set_str(h, "city", cfg->city);
        nvs_set_i32(h, "land", cfg->land);
        nvs_set_str(h, "scenery", ws_land_name(cfg->land));
        nvs_set_i32(h, "lat", (int32_t)(cfg->lat * 10000.0f));
        nvs_set_i32(h, "lon", (int32_t)(cfg->lon * 10000.0f));
        /* ★★★ 2026-10-10 修（隐患 · 配网就写死「时区 = +8」并声明"已知"）：
           原来这里 `nvs_set_i32("tz", 8*3600);` **并 `nvs_set_i32("tzok", 1)`** ——
           等于断言「本机时区就是 +8」。可配网是在 **AP 模式、没有外网** 下做的，
           设备此刻**根本不知道这台机器在哪个时区**（用户可能整机在 UTC+1）。
           `tzok=1` 的危害不只是"默认值不对"，它把上面那句 `8*3600` 从"临时兜底"
           **升级成了"用户的既成事实"**：`ws_load_config()` 从此按 `tzok==1` 采纳 +8，
           只要第一次取天气失败（或用户跳过配网），**永远回不到"未知"状态** ——
           时间 / 时段 / 月相 / 明信片上的时间全错，且**没有任何自愈路径**。
           ⇒ 新口径：配网只写「我配过网」这一件事，**时区留空**（不写 tz / 不写 tzok）。
             真值由第一次成功联网时 `timezone=auto` 的 `utc_offset_seconds` 回填
             （net_sync.c 的 ws_save_config(cfg, true) 会同时写 tz 和 tzok）。
             `ws_load_config()` 对"没有 tzok"的设备一律给 +8 兜底 ⇒
             **对 +8 用户行为与改前完全一致**；对非 +8 用户，从"永久错"变成
             "第一次联网前临时错、之后自动纠正"。 */
        /* （原 `nvs_set_i32(h, "tz", …)` + `nvs_set_i32(h, "tzok", 1)` 已删 —— 别再加回来，
             要加必须同时给出"为什么此刻的 tz 已可信"的理由。） */
        /* ★ 城市可能变了 → 必须擦掉上次的地理编码记录，
           否则 ws_resolve_city 会以为「同城、坐标没变」而沿用旧坐标。 */
        nvs_erase_key(h, "geo_city");
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "★ 配网完成：SSID=%s 城市=%s 窗外风景=%s（坐标待联网后解析）",
             cfg->ssid, cfg->city, ws_land_name(cfg->land));
    fflush(stdout);

    /* ★★ 2026-09-30 修复：原来这里是 `httpd_stop(srv)` 紧跟 `vTaskDelay(1500)`，
       顺序与注释「让响应先发出去」正好相反 —— 响应还没发完就把 HTTP 服务停了。
       现在统一走 prov_portal_stop()：先停 HTTP + DNS 任务，再把 WiFi 从 APSTA 降成 STA
       （热点下线、DHCP 停、功耗归零），**保留刚试连成功的 STA** 给紧接着的 ws_sync 用。
       手机那轮 /status 的轮询窗口已在上面留过（6×500ms）。 */
    prov_portal_stop(srv, true);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 * ★★ 2026-10-19 网页升级门户（ws_upgrade_open / close / ap_ssid）
 * ──────────────────────────────────────────────────────────────────────
 * 与配网门户**复用同一套 SoftAP 底座**（softap_start：PetCat-XXXX 无密码，APSTA），
 * 路由：GET /（上传页）+ POST /upgrade（固件流，★2026-10-19 起直写非运行槽）+ 手机探测路径。
 *   ★2026-10-19：原 GET /tf（卡状态预检）随「落卡中转」一并删除 —— 双槽后网页升级不再需要 TF 卡。
 *   · ★★ 2026-10-19 修：**开 DNS 劫持 + 挂探测路由** —— 原来只起 HTTP、不起 DNS，
 *     手机连上热点也**不会自动弹升级页**（用户报「连了它的 WiFi 却不跳网页」）。
 *     现在与配网门户同款：DNS 把任意域名解析到 192.168.4.1，探测路径回 200+meta-refresh。
 *   · 万一没弹窗，仍可手动开浏览器进 http://192.168.4.1/（设备屏上照旧写着这句）。
 *   · 上传流走 ota_up.c 的 ota_web_begin/feed/finalize（同步跑在 httpd 任务里），
 *     写完即切启动分区，esp_ota_end 校验不过则不切（旧固件照常）。
 *   · 热点名给 main.c 的 PG_OTA_WEB 页上屏（ws_upgrade_ap_ssid，与这里同源）。
 * ══════════════════════════════════════════════════════════════════════ */
#include "ota_up.h"

static httpd_handle_t s_upg_srv = NULL;
static char s_upg_ssid[33] = {0};      /* 本次打开的热点名（未开 = 空串） */

void ws_upgrade_ap_ssid(char *out, int cap)
{
    if (!out || cap <= 0) return;
    snprintf(out, (size_t)cap, "%s", s_upg_ssid);
}

/* 上传页（中文单页；file 选择 + XHR 上传带进度条 + 结果提示）
   ★★★ 2026-10-19（双槽恢复后 · 用户令）：点「开始上传」= **直接 POST /upgrade**，
     整包流式写**非运行槽**；不再有 /tf 预检、TF 卡中转、无卡变砖风险确认层
     （写非运行槽 ⇒ 中断/断电不变砖，见 ota_up.h 说明）。 */

/* ★★ R177（2026-11-10 用户报）：手机在内置浏览器（微信 / QQ / 部分定制浏览器）里
   **点「点击这里选择固件文件」没反应** —— 这类 WebView 根本不实现 <input type=file>
   的文件选择器，页面代码再怎么写都调不出来。修法只有一条：引导用户换系统浏览器。

   ⚠ 为什么这段写在 C 里、而不是改 webui.bin 里那页 HTML：
     OTA 网页升级只写 **app 分区**，assets 分区（含 webui.bin）不跟着走。
     改在资源里 ⇒ 用户 OTA 升上来永远看不到这段提示；改在 C 里才跟着固件走。
   ⚠ 为什么按服务端 UA 判断「手机 / 电脑」而不是纯前端：
     电脑上这段完全没用（能直接选文件），插进去只是噪音；服务端判一次最省事。

   三档文案：
     · 微信 / QQ 内置浏览器 → 只能指引「右上角 ··· → 在浏览器打开」（intent 会被微信拦）
     · 其他 Android     → 直接给 intent:// 按钮，拉起默认浏览器
     · iOS              → Safari 不允许网页跳浏览器，只能指引「分享 → 在 Safari 中打开」 */

/* 有限长 memmem：webui 页**不是 NUL 结尾**，不能直接 strstr（会越界读到下一页） */
static const char *np_mem_find(const char *hay, int n, const char *needle)
{
    const int m = (int)strlen(needle);
    if (m <= 0 || n < m) return NULL;
    for (int i = 0; i + m <= n; ++i)
        if (hay[i] == needle[0] && memcmp(hay + i, needle, (size_t)m) == 0) return hay + i;
    return NULL;
}

/* ★R180（用户反馈）：这条提示**沿用明信片导入页那套视觉** —— 同一张白卡片
   （1px #d3d1c7 / 圆角 12），同一颗「用浏览器打开」胶囊按钮（.extbtn：白底 + 1px 紫边 +
   12px 字 + 同一个外链图标），不再另造一版紫底通栏大按钮。 */
#define UPG_EXT_ICON \
 "<svg viewBox='0 0 24 24'><path d='M14 3h7v7'/><path d='M21 3l-9 9'/>" \
 "<path d='M19 14v5a2 2 0 01-2 2H5a2 2 0 01-2-2V7a2 2 0 012-2h5'/></svg>"

#define UPG_SYSBAR_CSS \
 "<style>#sysbar{background:#fff;border:1px solid #d3d1c7;border-radius:12px;padding:14px 16px;" \
 "font-size:13px;line-height:1.7;color:#5f5e5a;max-width:430px;margin:0 auto 14px}" \
 "#sysbar b{font-size:14px;font-weight:500;color:#2c2c2a}" \
 "#sysbar .extbtn{display:inline-flex;align-items:center;gap:5px;margin-top:10px;font-size:12px;" \
 "padding:5px 11px;border:1px solid #534ab7;border-radius:8px;background:#fff;color:#534ab7;" \
 "text-decoration:none;font-weight:500}" \
 "#sysbar .extbtn:active{background:#f1effc}" \
 "#sysbar .extbtn svg{width:15px;height:15px;stroke:#534ab7;fill:none;stroke-width:1.8;" \
 "stroke-linecap:round;stroke-linejoin:round}" \
 "#sysbar .sm{font-size:12px;color:#888780;margin-top:8px}" \
 "#sysbar .u{font-family:ui-monospace,Menlo,monospace;background:#f5f4f0;padding:1px 6px;" \
 "border-radius:4px;color:#2c2c2a}</style>"

/* ★R177 另一个更常见的「选不了」原因：页面里 `<input accept='.bin,application/octet-stream'>`。
   Android 的文件选择器按 MIME 过滤，`.bin` 不在系统 MIME 库里 ⇒ 打开后**整列表是灰的 / 空**，
   用户体感就是"点了没反应"。运行时把 accept 摘掉，让选择器显示全部文件，用户自己挑那个 bin。
   （同理 iOS 的 UIDocumentPicker 也会被未知扩展名过滤。）

   ★R180（用户报「上传之后没反应」）：补两段**上传反馈**（改在 C 里，跟着固件走）：
     ① 一点「开始上传」立刻把文案改成「让我稍等片刻…」并把进度条亮出来 —— 原来只有
        xhr.upload.onprogress 才会动，某些浏览器/弱网下这条事件迟迟不来，页面看着像死了；
     ② 设备写完就重启，最后一个响应常被掐断 ⇒ onerror/abort 时不保留页面那句
        「网络错误，请重试」，延迟 600ms（让页面自己的 onerror 先跑完）改成
        「设备正在重启…」。真失败（HTTP 500 verify failed）仍由页面自己的 onload 显示。 */
#define UPG_SYSBAR_JS \
 "<script>(function(){" \
 "function fix(){var f=document.getElementById('f');" \
 "if(f&&f.hasAttribute('accept')){f.removeAttribute('accept');}}" \
 "if(document.readyState!=='loading'){fix();}else{document.addEventListener('DOMContentLoaded',fix);}" \
 "window.addEventListener('load',fix);" \
 "function T2(t){var m=document.getElementById('msg');if(m)m.textContent=t;}" \
 "document.addEventListener('click',function(e){var t=e.target;" \
 "while(t&&t!==document){if(t.id==='go'){if(t.disabled)return;var b=document.getElementById('bar');" \
 "if(b)b.style.display='block';T2('让我稍等片刻…');return;}t=t.parentNode;}},true);" \
 "var OP=XMLHttpRequest.prototype.open;XMLHttpRequest.prototype.open=function(){" \
 "var x=this;var D=function(){setTimeout(function(){" \
 "T2('设备正在重启… 1 分钟后重连热点看版本');},600);};" \
 "x.addEventListener('error',D);x.addEventListener('abort',D);" \
 "return OP.apply(x,arguments);};" \
 "})();</script>"

#define UPG_SYSBAR_MM UPG_SYSBAR_CSS \
 "<div id='sysbar'><b>选择不了文件？</b><br>" \
 "你现在是用 App 内置浏览器打开的，它一般不给选文件的窗口。<br>" \
 "点右上角 <b>···</b> → 选「在浏览器打开」，再回来点一次就行。" \
 "<div class='sm'>也可以在系统浏览器里直接输 <span class='u'>192.168.4.1</span></div></div>" UPG_SYSBAR_JS

#define UPG_SYSBAR_ANDROID UPG_SYSBAR_CSS \
 "<div id='sysbar'><b>选择不了文件？</b><br>" \
 "换成系统浏览器就好：" \
 "<a class='extbtn' href='intent://192.168.4.1#Intent;scheme=http;category=android.intent.category.BROWSABLE;action=android.intent.action.VIEW;end'>" \
 UPG_EXT_ICON "用浏览器打开</a>" \
 "<div class='sm'>按钮没反应就手动开浏览器，地址填 <span class='u'>192.168.4.1</span></div></div>" UPG_SYSBAR_JS

#define UPG_SYSBAR_IOS UPG_SYSBAR_CSS \
 "<div id='sysbar'><b>选择不了文件？</b><br>" \
 "点下方的分享按钮（或右上角 <b>···</b>），选「在 Safari 中打开」，再回来选一次。" \
 "<div class='sm'>也可以直接打开 Safari，地址填 <span class='u'>192.168.4.1</span></div></div>" UPG_SYSBAR_JS

static esp_err_t h_upgrade_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    int wl = 0;
    const char *wp = webui_page(WEBUI_UPG_PAGE, &wl);
    if (!wp || wl <= 0) { ESP_LOGE(TAG, "★ 升级页资源缺失"); return httpd_resp_send_404(r); }

    /* 只有手机才插那条提示 —— 电脑能正常选文件，插了纯属噪音 */
    char ua[192] = {0};
    const bool got_ua = (httpd_req_get_hdr_value_str(r, "User-Agent", ua, sizeof(ua) - 1) == ESP_OK);
    const bool mobile = got_ua &&
        (strstr(ua, "Mobile") || strstr(ua, "Android") || strstr(ua, "iPhone") || strstr(ua, "iPad"));
    if (!mobile) return httpd_resp_send(r, wp, wl);

    const bool wechat = got_ua && (strstr(ua, "MicroMessenger") || strstr(ua, "QQ/"));
    const bool ios    = got_ua && (strstr(ua, "iPhone") || strstr(ua, "iPad") || strstr(ua, "iOS"));
    const char *bar = wechat ? UPG_SYSBAR_MM : (ios ? UPG_SYSBAR_IOS : UPG_SYSBAR_ANDROID);

    /* 插在「上传卡片」之前：先看到提示，再看到选择框。
       锚点找不到就原样吐整页 —— 宁可少一条提示，也不能把页面拼坏。 */
    const char *anchor = np_mem_find(wp, wl, "<div class='card'>");
    if (!anchor) {
        ESP_LOGW(TAG, "★ 升级页找不到 card 锚点 → 原样发送（提示未插入）");
        return httpd_resp_send(r, wp, wl);
    }
    const int head = (int)(anchor - wp);
    esp_err_t e = httpd_resp_send_chunk(r, wp, head);
    if (e == ESP_OK) e = httpd_resp_send_chunk(r, bar, -1);
    if (e == ESP_OK) e = httpd_resp_send_chunk(r, anchor, wl - head);
    if (e == ESP_OK) e = httpd_resp_send_chunk(r, NULL, 0);
    ESP_LOGI(TAG, "升级页：检测到%s → 已注入「用系统浏览器打开」提示", wechat ? "微信/QQ 内置浏览器" : (ios ? "iOS 浏览器" : "Android 浏览器"));
    return e;
}

/* POST /upgrade：整包流式收 → ota_web_* **直写非运行槽**（校验不过不切，旧固件照常）
   ★2026-10-19：不再有 TF 卡中转 / 风险确认分支（双槽后中断也不变砖，见 ota_up.h 说明）。 */
static esp_err_t h_upgrade_post(httpd_req_t *r)
{
    int remaining = r->content_len;
    ESP_LOGI(TAG, "★ /upgrade 收到上传：%d B", remaining);
    if (remaining <= 0 || remaining > 2 * 1024 * 1024) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_OK;
    }
    if (!ota_web_begin(NULL, remaining)) {
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "device busy / rejected");
        return ESP_OK;
    }
    char *buf = malloc(4096);
    if (!buf) { ota_web_abort(); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "nomem"); return ESP_OK; }
    bool ok = true;
    int64_t t0 = esp_timer_get_time();
    while (remaining > 0) {
        int n = httpd_req_recv(r, buf, remaining > 4096 ? 4096 : remaining);
        if (n <= 0) { ok = false; break; }                    /* 手机断开 / 超时 */
        if (!ota_web_feed((const uint8_t *)buf, (size_t)n)) { ok = false; break; }
        remaining -= n;
        if ((esp_timer_get_time() - t0) / 1000000 >= 120) {   /* ★2026-10-23 修（M12）：OTA 上传总时限 120s（2MB 正常网速足够），防慢速客户端逐字节拖死唯一 httpd 任务 */
            ESP_LOGW(TAG, "★ /upgrade 上传超时（120s）→ 中止");
            ok = false; break;
        }
    }
    free(buf);
    if (!ok) {
        ota_web_abort();
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "upload aborted");
        return ESP_OK;
    }
    if (!ota_web_finalize()) {   /* esp_ota_end 校验没过 → 不切分区 */
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "verify failed");
        return ESP_OK;
    }
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_send(r, "OK", 2);
    return ESP_OK;
}

/* ★R202（全量复查）：门户开失败时把热点也收掉。
   四个门户（升级 / 导入 / 加好友 / 改名卡）都是「先 softap_start() 再 httpd_start()」，
   原来 httpd 起不来只把 s_*_srv 置 NULL 就 return false —— 调用方的「已开」标志因此不会
   置位 ⇒ 对应的 *_close() 永远不会被调用 ⇒ SoftAP + DHCP 常驻到下次重启（耗电 + 手机
   一直看到 PetCat-XXXX）。这里按各 close() 同样的收法回退：停 DNS 任务 + APSTA → STA。 */
static void portal_ap_rollback(void)
{
    s_dns_stop = true;
    for (int i = 0; i < 20 && s_dns_alive; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_alive && s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_APSTA) esp_wifi_set_mode(WIFI_MODE_STA);
}

bool ws_upgrade_open(void)
{
    if (s_upg_srv) return true;                 /* 已开着（防重复进） */
    s_dns_stop = false;                         /* ★ 允许 DNS 劫持任务重新起来（同会话可再进） */
    softap_start();                             /* 配网同款热点（PetCat-XXXX · 无密码 · APSTA） */
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_upg_ssid, sizeof(s_upg_ssid), PROV_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;  /* / · /upgrade · 6 条探测 · 通配兜底 = 9（留余量） */
    cfg.stack_size = 8192;      /* ota_web_* 在本任务里写 flash（1.4MB 包），别用 4KB 默认 */
    /* ★★★ 2026-10-19：收包超时 5s → **20s** —— 写 flash 与主循环刷屏并行时，收包会有停顿，
       默认 5s 容易判超时 → 上传中断（必现的直接原因之一）。只放宽**升级门户**，配网门户不动。 */
    cfg.recv_wait_timeout = 20;
    /* ★★ 2026-10-19 修：探测路径带 `*` 后缀 —— 必须**通配匹配**才生效
       （IDF 默认是精确匹配，`*` 不起作用；同 prov_http_start 的坑，见那边注释）。 */
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_upg_srv, &cfg) != ESP_OK) {
        s_upg_srv = NULL;
        ESP_LOGE(TAG, "★ 升级门户 httpd_start 失败 → 热点回退");
        portal_ap_rollback();
        return false;
    }
    httpd_uri_t u_root = { .uri = "/",        .method = HTTP_GET,  .handler = h_upgrade_root };
    httpd_uri_t u_upg  = { .uri = "/upgrade", .method = HTTP_POST, .handler = h_upgrade_post };
    /* ★★ 2026-10-19 修：手机连通性探测路径 —— 与配网门户同款（h_portal 回 200+meta-refresh
       跳到 192.168.4.1，未知路径 h_redirect 302）。不挂这些，手机连上热点不会自动弹升级页。 */
    httpd_uri_t u_g1 = { .uri = "/generate_204*",        .method = HTTP_GET, .handler = h_portal };  /* Android */
    httpd_uri_t u_g2 = { .uri = "/hotspot-detect.html*", .method = HTTP_GET, .handler = h_portal };  /* iOS */
    httpd_uri_t u_g3 = { .uri = "/hotspot_detect.html*", .method = HTTP_GET, .handler = h_portal };  /* iOS 旧版 */
    httpd_uri_t u_g4 = { .uri = "/connecttest.txt*",     .method = HTTP_GET, .handler = h_portal };  /* Windows */
    httpd_uri_t u_g5 = { .uri = "/ncsi.txt*",            .method = HTTP_GET, .handler = h_portal };  /* Windows */
    httpd_uri_t u_g6 = { .uri = "/redirect*",            .method = HTTP_GET, .handler = h_portal };  /* Windows 11 */
    httpd_uri_t u_any = { .uri = "/*",                   .method = HTTP_GET, .handler = h_redirect };
    /* 注册顺序 = 匹配顺序：无 `*` 的精确路径必须排在前，否则会被通配兜底路由抢走 */
    httpd_register_uri_handler(s_upg_srv, &u_root);
    httpd_register_uri_handler(s_upg_srv, &u_upg);
    httpd_register_uri_handler(s_upg_srv, &u_g1);
    httpd_register_uri_handler(s_upg_srv, &u_g2);
    httpd_register_uri_handler(s_upg_srv, &u_g3);
    httpd_register_uri_handler(s_upg_srv, &u_g4);
    httpd_register_uri_handler(s_upg_srv, &u_g5);
    httpd_register_uri_handler(s_upg_srv, &u_g6);
    httpd_register_uri_handler(s_upg_srv, &u_any);
    /* ★★ 2026-10-19：DNS 劫持任务 —— 任意域名 → 192.168.4.1，手机系统探测才判定为门户自动弹窗。
       栈 6144：dns_task 栈上有 buf[512]+resp[512]（见 ws_provision 同款注释）。 */
    xTaskCreate(dns_task, "dns_hijack", 6144, NULL, 5, NULL);
    ESP_LOGW(TAG, "★ 升级热点已开：%s → 手机连上**自动弹升级页**；没弹就开浏览器进 http://192.168.4.1/", s_upg_ssid);
    fflush(stdout);
    return true;
}

void ws_upgrade_close(void)
{
    if (s_upg_srv) { httpd_stop(s_upg_srv); s_upg_srv = NULL; }
    /* ★★ 2026-10-19 修：DNS 劫持任务也要收（否则 UDP:53 + 6KB 栈常驻耗电；
       也避免下次开热点时上一次的劫持任务还挂着抢 53 端口）。收法同 prov_portal_stop。 */
    s_dns_stop = true;
    for (int i = 0; i < 20 && s_dns_alive; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_alive && s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_STA);       /* 热点下线，保留已连上的 STA（取消升级/中断后设备仍在线） */
    }
    s_upg_ssid[0] = 0;
    ESP_LOGW(TAG, "★ 升级门户已收（HTTP/DNS 停 · 热点下线 · 保留 STA）");
    fflush(stdout);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * ★2026-10-20 照片明信片**导入门户**（仿上面升级门户 ws_upgrade_* 同款底座）
 * ------------------------------------------------------------------------------
 * 复用配网 SoftAP（PetCat-XXXX 无密码 APSTA）+ DNS 劫持：手机连上热点**自动弹导入页**，
 *   不用二维码、不用手输网址（连通性探测路径同款，见 ws_upgrade_open 注释）。
 * 路由：GET /（返回 postcard_page.h 的导入页 HTML）· POST /upload（收 2268B → 回调存 mypost）。
 * ⚠ 收图走**回调** ws_post_set_recv_cb：mypost_add 在 main.c（static），借此解耦，避免反向依赖。
 *   回调返回 ≥0 = 存入槽号，<0 = 满/失败。
 * ⚠ 与升级门户互斥（同占 SoftAP 门户）：同一时刻只开一个，由 main.c 入口保证。
 * ══════════════════════════════════════════════════════════════════════════════ */
/* postcard_page.h 已迁 assets AID_WEBUI（重构 2e）；源文件保留作生成参考 */
#define POST_UPLOAD_BYTES 2268
/* 背面文字上限 = main_api.h 的 MYPOST_TEXT_MAX（本文件曾另立 POST_TEXT_MAX=180，零引用且双份口径，已删）*/
/* ★2026-10-22 C 项载荷上限：2268 图 + 1(0xFF 哨兵) + 1+tl(≤180) + 1+tal(≤30 标题) +
   1+aal(≤36 地址) = 2518。与 main.c MYPOST_PAYLOAD_MAX 同口径（两边各定义，解耦不互包）。 */
#define POST_UPLOAD_MAX   2518
static httpd_handle_t s_post_srv = NULL;
static char s_post_ssid[33] = {0};
static int (*s_post_recv)(const uint8_t *data, uint32_t len) = NULL;

__attribute__((unused))
void ws_post_set_recv_cb(int (*cb)(const uint8_t *data, uint32_t len))
{ s_post_recv = cb; }

__attribute__((unused))
void ws_post_ap_ssid(char *out, int cap)
{ if (!out || cap <= 0) return; snprintf(out, (size_t)cap, "%s", s_post_ssid); }

static esp_err_t h_post_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    { int wl; const char *wp = webui_page(WEBUI_POST_PAGE, &wl);
      return httpd_resp_send(r, wp, wl); }
}

/* POST /upload：收 2268B 纯图 / 2269+tl 旧文字格式 / ★2026-10-22 0xFF 哨兵新格式
   （图+文字+标题+地址，≤2518B）→ 回调存 mypost。回 "OK" / "FULL" / "ERR" / "BAD_SIZE"。 */
static esp_err_t h_post_upload(httpd_req_t *r)
{
    const int remaining = r->content_len;
    const int buf_cap = POST_UPLOAD_MAX;
    ESP_LOGI(TAG, "★ /upload 收到上传：%d B", remaining);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    /* ★2026-10-22 合法长度三种：2268 / 2269..2449（旧文字）/ 2270..2518（哨兵新格式，
       具体字段边界由回调 mypost_add 深度校验，这里只拦总量） */
    /* ★R212 修（全量审计）：原来写成 `remaining > POST_UPLOAD_BYTES + 1`
       —— 恰好 2269B（图 2268 + 文字长度字节 0，旧格式 tl=0）被判 BAD_SIZE，
       与本函数注释「2269..2449 合法」正好相反。改成 > POST_UPLOAD_BYTES。 */
    if (remaining != POST_UPLOAD_BYTES &&
        !(remaining > POST_UPLOAD_BYTES && remaining <= buf_cap)) {
        httpd_resp_send(r, "BAD_SIZE", 8); return ESP_OK;
    }
    uint8_t *buf = malloc(buf_cap);
    if (!buf) { httpd_resp_send(r, "ERR", 3); return ESP_OK; }
    int got = 0;
    int64_t t0 = esp_timer_get_time();
    while (got < remaining) {
        int n = httpd_req_recv(r, (char *)buf + got, remaining - got);
        if (n <= 0) break;
        got += n;
        if ((esp_timer_get_time() - t0) / 1000000 >= 10) {   /* ★2026-10-23 修（M12）：上传总时限 10s，防慢速客户端拖死唯一 httpd 任务 */
            ESP_LOGW(TAG, "★ /upload 上传超时（10s）→ 中止");
            break;
        }
    }
    const char *resp = "ERR";
    if (got == remaining) {
        int slot = s_post_recv ? s_post_recv(buf, (uint32_t)got) : -1;
        resp = (slot >= 0) ? "OK" : "FULL";
        ESP_LOGI(TAG, "mypost 存入：槽 %d → %s", slot, resp);
    } else {
        ESP_LOGW(TAG, "/upload 收不满（%d/%d）", got, remaining);
    }
    free(buf);
    httpd_resp_send(r, resp, (int)strlen(resp));
    return ESP_OK;
}

__attribute__((unused))
bool ws_post_open(void)
{
    if (s_post_srv) return true;
    s_dns_stop = false;
    softap_start();
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_post_ssid, sizeof(s_post_ssid), PROV_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 20;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_post_srv, &cfg) != ESP_OK) {
        s_post_srv = NULL;
        ESP_LOGE(TAG, "★ 导入门户 httpd_start 失败 → 热点回退");
        portal_ap_rollback();
        return false;
    }
    httpd_uri_t u_root = { .uri = "/",       .method = HTTP_GET,  .handler = h_post_root };
    httpd_uri_t u_up   = { .uri = "/upload", .method = HTTP_POST, .handler = h_post_upload };
    httpd_uri_t u_g1 = { .uri = "/generate_204*",        .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g2 = { .uri = "/hotspot-detect.html*", .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g3 = { .uri = "/hotspot_detect.html*", .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g4 = { .uri = "/connecttest.txt*",     .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g5 = { .uri = "/ncsi.txt*",            .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g6 = { .uri = "/redirect*",            .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_any= { .uri = "/*",                    .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(s_post_srv, &u_root);
    httpd_register_uri_handler(s_post_srv, &u_up);
    httpd_register_uri_handler(s_post_srv, &u_g1);
    httpd_register_uri_handler(s_post_srv, &u_g2);
    httpd_register_uri_handler(s_post_srv, &u_g3);
    httpd_register_uri_handler(s_post_srv, &u_g4);
    httpd_register_uri_handler(s_post_srv, &u_g5);
    httpd_register_uri_handler(s_post_srv, &u_g6);
    httpd_register_uri_handler(s_post_srv, &u_any);
    xTaskCreate(dns_task, "dns_hijack", 6144, NULL, 5, NULL);
    ESP_LOGW(TAG, "★ 照片导入热点已开：%s → 手机连上**自动弹导入页**；没弹就开浏览器进 http://192.168.4.1/", s_post_ssid);
    fflush(stdout);
    return true;
}

__attribute__((unused))
void ws_post_close(void)
{
    if (s_post_srv) { httpd_stop(s_post_srv); s_post_srv = NULL; }
    s_dns_stop = true;
    for (int i = 0; i < 20 && s_dns_alive; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_alive && s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) == ESP_OK && m == WIFI_MODE_APSTA) esp_wifi_set_mode(WIFI_MODE_STA);
    /* ★★2026-10-21（用户报「从照片信件页返回后它又跳连接 WiFi 页」）：
       原来只把 APSTA 降成 STA 就撒手 —— STA 还留着记忆里的 SSID，门户一收就**自动连网**，
       用户从这页返回时屏上就冒出「正在连接 XXX」页（这页根本不该联网）。
       ⇒ 改成**彻底停掉 WiFi 栈**（与配网跳过那条路同口径），本页用完即走。
       下次需要联网时 ws_sync → ws_wifi_connect 会自己 esp_wifi_start（它本来就写得
       「按需启动、容忍 NOT_STOPPED」，见 net_sync.c），不影响 30 分钟重同步 / 唤醒联网。 */
    esp_wifi_stop();
    s_post_ssid[0] = 0;
    ESP_LOGW(TAG, "★ 导入门户已收（HTTP/DNS 停 · 热点下线 · WiFi 栈已停，不做任何连接）");
    fflush(stdout);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * ★2026-10-22 朋友功能「添加朋友」门户（仿 ws_post_* 同款底座）
 * ------------------------------------------------------------------------------
 * 流程（用户拍板：设备显示二维码、手机扫）：
 *   ① B 设备点「添加朋友」→ 开 SoftAP + httpd（本门户）
 *   ② B 的手机连上热点 → 扫 A 设备上的二维码
 *   ③ 二维码载荷 = http://192.168.4.1/add?f=<A的好友码>
 *      —— 手机此刻正连着 B 的热点，所以这个网址落到 **B 的门户**，码自动填入
 *   ④ 点「添加好友」→ POST /addsave（正文 `CODE|昵称`）→ 回调存进 B 的 NVS
 * 互加 = 两边各扫一次；全程零服务器、零账号。
 * ══════════════════════════════════════════════════════════════════════════════ */
static httpd_handle_t s_fr_srv = NULL;
static char s_fr_ssid[33] = {0};
static int (*s_fr_add_cb)(const char *code, const char *name) = NULL;

static const char FR_ADD_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>添加朋友</title><style>"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,'PingFang SC',sans-serif;margin:0;padding:24px 18px;background:#f5f4f0;color:#2c2c2a}"
".card{max-width:430px;margin:0 auto;background:#fff;border:1px solid #d3d1c7;border-radius:12px;padding:18px}"
"h2{margin:0 0 6px;font-size:20px;font-weight:500}p{color:#5f5e5a;font-size:13px;margin:14px 0 6px}"
"input{width:100%;box-sizing:border-box;font-family:inherit;font-size:15px;padding:11px 12px;border:1px solid #b4b2a9;border-radius:8px;background:#fff;color:#2c2c2a;outline:none}"
"input:focus{border-color:#534ab7}"
"button{display:block;width:100%;box-sizing:border-box;margin-top:22px;padding:13px;font-family:inherit;font-size:16px;font-weight:500;border:0;border-radius:8px;background:#534ab7;color:#fff;cursor:pointer;-webkit-appearance:none;appearance:none}"
"button:active{background:#3c3489}"
"#r{margin-top:14px;font-size:14px;color:#0a7a3d;min-height:20px}"
"</style></head><body><div class='card'>"
"<h2>添加朋友</h2>"
"<p>输入对方的 6 位好友码</p>"
"<input id='f' maxlength='6' placeholder='6位好友码' autocapitalize='characters'>"
"<p>给他起个名字</p>"
"<input id='n' maxlength='12' placeholder='好友'>"
"<button type='button' onclick='go()'>添加好友</button><div id='r'></div>"
"<p style='font-size:12px;color:#888780;margin-top:16px'>两人都在场时，也可用相机扫对方二维码，本页会自动填入</p>"
"</div><script>"
"const q=new URLSearchParams(location.search);"
"const fe=document.getElementById('f');"
"fe.value=(q.get('f')||'').toUpperCase();"
"async function go(){const c=fe.value.trim().toUpperCase();"
"const n=(document.getElementById('n').value||'').trim();"
"const r=document.getElementById('r');"
"if(c.length!==6){r.textContent='好友码应为6位';return;}"
"r.textContent='正在添加…';"
"const x=await fetch('/addsave',{method:'POST',body:c+'|'+n});"
"const t=await x.text();"
"r.textContent=(t==='OK')?'已保存，可以返回设备了':('失败：'+t);}"
;   /* ★R92：尾部 `</script></body></html>` 已拆到 FR_ADD_TAIL ——
       编辑模式要往 **`</script>` 之前**注入一段预填脚本（码/名字/标题/按钮文案）。 */

/* ★R92：网页尾部——两个模式共用，单独发是为了中间能插注入段 */
static const char FR_ADD_TAIL[] = "</script></body></html>";

/* ★R92：编辑目标（>= 0 = 改第 i 位好友）。与 friend.c 的 s_edit 同步，
   只用来给网页预填；真正的「改还是加」由 friend.c 的回调分派。 */
static int s_fr_edit = -1;

__attribute__((unused))
void ws_friend_set_edit(int idx) { s_fr_edit = idx; }

__attribute__((unused))
void ws_friend_set_add_cb(int (*cb)(const char *code, const char *name))
{ s_fr_add_cb = cb; }

__attribute__((unused))
void ws_friend_ap_ssid(char *out, int cap)
{ if (!out || cap <= 0) return; snprintf(out, (size_t)cap, "%s", s_fr_ssid); }

static esp_err_t h_fr_add_page(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    /* ★R92：分块发 —— 前端共用同一份页，编辑模式在 </script> 前插一段预填脚本。
       为什么不在设备上改：本机只有 BOOT/POWER 两个键，6 位码 + 昵称没法敲
       ⇒「修改」= 开同一个热点，手机打开**已预填**的网页改完提交。 */
    httpd_resp_send_chunk(r, FR_ADD_HTML, -1);

    const friend_info_t *f = (s_fr_edit >= 0) ? friend_at(s_fr_edit) : NULL;
    if (f) {
        char nm[80] = {0};
        char *p = nm;
        for (const char *q = f->name; *q && p < nm + sizeof(nm) - 8; ++q) {
            if (*q == '\n' || *q == '\r') continue;       /* 换行会把 JS 字符串截断 */
            if (*q == '\'' || *q == '\\') *p++ = '\\';    /* 单引号/反斜杠转义 */
            *p++ = *q;
        }
        *p = 0;
        char js[600];
        snprintf(js, sizeof(js),
                 "fe.value='%s';"
                 "document.getElementById('n').value='%s';"
                 "document.querySelector('h2').textContent='修改朋友';"
                 "document.querySelector('button').textContent='保存修改';"
                 "var p0=document.querySelector('.card p');"
                 "if(p0)p0.textContent='改完点保存，设备上这条好友会被更新';",
                 f->code, nm);
        httpd_resp_send_chunk(r, js, -1);
    }

    httpd_resp_send_chunk(r, FR_ADD_TAIL, -1);
    httpd_resp_send_chunk(r, NULL, 0);          /* 结束分块传输 */
    return ESP_OK;
}

/* POST /addsave：正文 `CODE|昵称`（昵称可空 / UTF-8 直传，不做 URL 编码） */
static esp_err_t h_fr_addsave(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    const int n = r->content_len;
    if (n <= 0 || n > 64) { httpd_resp_send(r, "BAD", 3); return ESP_OK; }

    char buf[72] = {0};
    int got = 0;
    int64_t t0 = esp_timer_get_time();
    while (got < n) {
        const int k = httpd_req_recv(r, buf + got, n - got);
        if (k <= 0) break;
        got += k;
        if ((esp_timer_get_time() - t0) / 1000000 >= 10) {   /* ★2026-10-23 修（M12）：上传总时限 10s */
            ESP_LOGW(TAG, "★ /addsave 上传超时（10s）→ 中止");
            break;
        }
    }
    if (got != n) { httpd_resp_send(r, "SHORT", 5); return ESP_OK; }

    char *sep = strchr(buf, '|');
    if (!sep) { httpd_resp_send(r, "FMT", 3); return ESP_OK; }
    *sep = 0;
    const char *code = buf;
    const char *name = sep + 1;
    if ((int)strlen(code) != 6) { httpd_resp_send(r, "CODE", 4); return ESP_OK; }

    const int idx = s_fr_add_cb ? s_fr_add_cb(code, name) : -1;
    if (idx >= 0) httpd_resp_send(r, "OK", 2);
    else          httpd_resp_send(r, "ERR", 3);
    ESP_LOGI(TAG, "★ /addsave：code=%s name=%s → %d", code, name, idx);
    return ESP_OK;
}

__attribute__((unused))
bool ws_friend_open(void)
{
    if (s_fr_srv) return true;
    s_dns_stop = false;
    softap_start();
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_fr_ssid, sizeof(s_fr_ssid), PROV_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 20;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_fr_srv, &cfg) != ESP_OK) {
        s_fr_srv = NULL;
        ESP_LOGE(TAG, "★ 添加朋友门户 httpd_start 失败 → 热点回退");
        portal_ap_rollback();
        return false;
    }
    httpd_uri_t u_add  = { .uri = "/add",     .method = HTTP_GET,  .handler = h_fr_add_page };
    httpd_uri_t u_root = { .uri = "/",        .method = HTTP_GET,  .handler = h_fr_add_page };
    httpd_uri_t u_save = { .uri = "/addsave", .method = HTTP_POST, .handler = h_fr_addsave };
    httpd_uri_t u_g1 = { .uri = "/generate_204*",        .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g2 = { .uri = "/hotspot-detect.html*", .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g3 = { .uri = "/hotspot_detect.html*", .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g4 = { .uri = "/connecttest.txt*",     .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g5 = { .uri = "/ncsi.txt*",            .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g6 = { .uri = "/redirect*",            .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_any= { .uri = "/*",                    .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(s_fr_srv, &u_add);
    httpd_register_uri_handler(s_fr_srv, &u_root);
    httpd_register_uri_handler(s_fr_srv, &u_save);
    httpd_register_uri_handler(s_fr_srv, &u_g1);
    httpd_register_uri_handler(s_fr_srv, &u_g2);
    httpd_register_uri_handler(s_fr_srv, &u_g3);
    httpd_register_uri_handler(s_fr_srv, &u_g4);
    httpd_register_uri_handler(s_fr_srv, &u_g5);
    httpd_register_uri_handler(s_fr_srv, &u_g6);
    httpd_register_uri_handler(s_fr_srv, &u_any);
    xTaskCreate(dns_task, "dns_hijack", 6144, NULL, 5, NULL);
    ESP_LOGW(TAG, "★ 添加朋友热点已开：%s → 手机连上后扫对方二维码（http://192.168.4.1/add?f=XXXXXX）", s_fr_ssid);
    fflush(stdout);
    return true;
}

__attribute__((unused))
void ws_friend_close(void)
{
    if (s_fr_srv) { httpd_stop(s_fr_srv); s_fr_srv = NULL; }
    s_dns_stop = true;
    for (int i = 0; i < 20 && s_dns_alive; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_alive && s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    esp_wifi_stop();                 /* 与导入门户同口径：用完即走，不自动连网 */
    s_fr_ssid[0] = 0;
    s_fr_edit = -1;                  /* ★R92：门户一收，编辑态即清（下次进来默认新增） */
    ESP_LOGW(TAG, "★ 添加朋友门户已收（HTTP/DNS 停 · 热点下线 · WiFi 栈已停）");
    fflush(stdout);
}

/* ══════════════════════════════════════════════════════════════════════
 * ★2026-10-26 改名卡：一次性「取名门户」（复用配网 /name、/namesave 页与校验）
 * 背景：正式取名只在配网流程里跑一次，之后没有"只改名"的入口。改名卡用掉后：
 *   ① 立刻把 pet_name 置成 "-"（屏上显示横杠）；
 *   ② 开 SoftAP + captive DNS，手机连上自动弹 /name；
 *   ③ 取名成功 → h_namesave 走 standalone 分支直接落 NVS；没取 → 保持 "-"，
 *      且 named_once 已被清 ⇒ 下次配网会再弹取名（不会静默永久丢名）。
 * 门户 3 分钟无人操作自动收（看门任务），离页/超时都不影响设备继续运行。
 * ══════════════════════════════════════════════════════════════════════ */
static httpd_handle_t s_nm_srv = NULL;
static char s_nm_ssid[33];

/* ★R153（全量检阅 P1-2）：go_page() 离页收口要判断门户是否还开着，
   s_nm_srv / s_name_standalone 都是本文件 static，故给一个只读 getter。 */
bool ws_name_portal_open(void)
{
    return s_nm_srv != NULL || s_name_standalone;
}

void ws_name_close(void)
{
    /* ★R153：先记下「这次调用是否真的收了一个开着的门户」。
       看门任务 180s 超时后也会调本函数；若门户已被 go_page 离页收掉，就不该再
       esp_wifi_stop() —— 那时后台 ws_sync 可能正用着 WiFi，会被误停。 */
    const bool was_open = ws_name_portal_open();
    if (s_nm_srv) { httpd_stop(s_nm_srv); s_nm_srv = NULL; }
    s_dns_stop = true;
    for (int i = 0; i < 20 && s_dns_alive; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_alive && s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    if (was_open) esp_wifi_stop();
    s_nm_ssid[0] = 0;
    s_name_standalone = false;
    ESP_LOGW(TAG, "★ 改名卡门户已收（HTTP/DNS 停 · 热点下线%s）",
             was_open ? "" : " · 门户已收过，不动 WiFi");
    fflush(stdout);
}

static void name_watch_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1800 && !s_name_saved && !s_name_skip; ++i)   /* 180s 上限 */
        vTaskDelay(pdMS_TO_TICKS(100));
    ws_name_close();
    vTaskDelete(NULL);
}

bool ws_name_open(void)
{
    if (s_nm_srv) return true;
    s_name_saved = false;
    s_name_skip  = false;
    s_name_act_ms = esp_timer_get_time() / 1000;
    s_name_standalone = true;
    s_dns_stop = false;
    softap_start();
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_nm_ssid, sizeof(s_nm_ssid), PROV_SSID_PREFIX "%02X%02X", mac[4], mac[5]);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 20;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_nm_srv, &cfg) != ESP_OK) {
        s_nm_srv = NULL;
        s_name_standalone = false;
        ESP_LOGE(TAG, "★ 改名卡门户 httpd_start 失败 → 热点回退");
        portal_ap_rollback();
        return false;
    }
    httpd_uri_t u_name = { .uri = "/name",     .method = HTTP_GET,  .handler = h_name };
    httpd_uri_t u_root = { .uri = "/",         .method = HTTP_GET,  .handler = h_name };
    httpd_uri_t u_save = { .uri = "/namesave", .method = HTTP_POST, .handler = h_namesave };
    httpd_uri_t u_skip = { .uri = "/nameskip", .method = HTTP_GET,  .handler = h_nameskip };
    httpd_uri_t u_g1 = { .uri = "/generate_204*",        .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g2 = { .uri = "/hotspot-detect.html*", .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g3 = { .uri = "/hotspot_detect.html*", .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g4 = { .uri = "/connecttest.txt*",     .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g5 = { .uri = "/ncsi.txt*",            .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_g6 = { .uri = "/redirect*",            .method = HTTP_GET, .handler = h_portal };
    httpd_uri_t u_any= { .uri = "/*",                    .method = HTTP_GET, .handler = h_redirect };
    httpd_register_uri_handler(s_nm_srv, &u_name);
    httpd_register_uri_handler(s_nm_srv, &u_root);
    httpd_register_uri_handler(s_nm_srv, &u_save);
    httpd_register_uri_handler(s_nm_srv, &u_skip);
    httpd_register_uri_handler(s_nm_srv, &u_g1);
    httpd_register_uri_handler(s_nm_srv, &u_g2);
    httpd_register_uri_handler(s_nm_srv, &u_g3);
    httpd_register_uri_handler(s_nm_srv, &u_g4);
    httpd_register_uri_handler(s_nm_srv, &u_g5);
    httpd_register_uri_handler(s_nm_srv, &u_g6);
    httpd_register_uri_handler(s_nm_srv, &u_any);
    xTaskCreate(dns_task, "dns_hijack", 6144, NULL, 5, NULL);
    xTaskCreate(name_watch_task, "nm_watch", 3072, NULL, 4, NULL);
    ESP_LOGW(TAG, "★ 改名卡门户已开：%s → 手机连上后自动打开取名页（http://192.168.4.1/name）", s_nm_ssid);
    fflush(stdout);
    return true;
}
