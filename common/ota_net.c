/* ══════════════════════════════════════════════════════════════════════
 * ota_net.c —— 在线升级（R218 恢复，2026-10-19 曾主动下线）
 *
 * 从公开 URL 拉 version_<variant>.json（{"ver","size","url","notes"}），
 * 比版本 → 发现新版则下载 bin 写非运行槽 → 校验通过才切分区。
 *
 * ★URL 口径：Gitee 公开仓 raw + GitHub raw 双源，**按配网居住地决定先后**：
 *   国内城市 / 未配网 → Gitee 先（国内直连稳）；海外城市 → GitHub 先。
 *   谁不通自动换另一家，清单和固件锁同一个源。
 *   Makefile/CMake 可用 -DOTA_NET_GITEE=... / -DOTA_NET_GITHUB=... 覆盖地址。
 * ★安全：HTTPS + esp_crt_bundle_attach；校验不过绝不切分区；取消/失败旧固件照跑。
 * ★两版各自清单：version_key.json / version_touch.json（variant.h 决定）。
 * ══════════════════════════════════════════════════════════════════════ */
#include "variant.h"
#include <esp_attr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_wifi.h"     /* ★R218：下载期间关 modem sleep（WIFI_PS_NONE） */
#include "esp_crt_bundle.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "cJSON.h"

#include "ota_up.h"
#include "ota_net.h"
#include "main_api.h"        /* ota_ver_newer() */

static const char *NET_TAG = "ota_net";

/* ★R218：多源自动回退 —— 先后顺序由居住地决定（见 ota_net_build_order），
   一源不通自动切下一个。★2026-11-16 用户令：**Gitee 源已删除**，理由实测：
     · gitee.com 这一跳在设备上就是 mbedtls_ssl_handshake -0x3000
       (= MBEDTLS_ERR_X509_FATAL_ERROR，证书链在 mbedTLS 下判死)；
     · 就算修好第一跳，Gitee raw 还会 302 到 raw.giteeusercontent.com，
       而这个 CDN 域名在本网络**直接拒绝连接**（WinError 10061 / ConnectionRefused）
       —— 第二跳不是固件能修的，所以整条路放弃，不再浪费一次 0.3s 的失败尝试。
   现存两家：
     · jsDelivr   —— 套 GitHub 仓的全球 CDN，清单实测 1.1~3.5s 稳定；
                     ⚠ 大文件走 chunked（无 Content-Length），下载处已兼容
     · GitHub raw —— 源站，国外最稳；国内时通时不通（实测一次 2.4s、一次 20s 超时）
   ⚠ 根治方案：接对象存储（腾讯 COS / 阿里 OSS），把 base 换成它的域名即可。 */
/* ★2026-11-16 深夜实测（用户令「就用免费的源」）：GitHub 反代镜像才是国内能用的那条路。
   实测（同一张联通网络，1.8MB 完整包）：
     gh-proxy.com   3.1s · Content-Length 正确  ← 国内首选
     ghfast.top     6.5s · Content-Length 正确
     jsDelivr       清单 1~3s 稳定，但 1.8MB 固件实测 503/404（回源失败），只能当兜底
     GitHub raw     源站，国外最稳；国内时通时不通
   镜像会把整条 raw URL 拼在自己域名后面（ota_net_download 的换源同样按此规则重组）。 */
#ifndef OTA_NET_GHPROXY1
#define OTA_NET_GHPROXY1 "https://gh-proxy.com/https://raw.githubusercontent.com/bbbbaiky/eink-pet/main/ota"
#endif
#ifndef OTA_NET_GHPROXY2
#define OTA_NET_GHPROXY2 "https://ghfast.top/https://raw.githubusercontent.com/bbbbaiky/eink-pet/main/ota"
#endif
#ifndef OTA_NET_JSDELIVR
#define OTA_NET_JSDELIVR "https://cdn.jsdelivr.net/gh/bbbbaiky/eink-pet@main/ota"
#endif
#ifndef OTA_NET_GITHUB
#define OTA_NET_GITHUB "https://raw.githubusercontent.com/bbbbaiky/eink-pet/main/ota"
#endif
/* ★2026-11-17 00:59 实机换序：gh-proxy 本次 <1 KB/s（91 秒没过 10%），
   ghfast 上一轮 199 秒跑完 → 把 ghfast 提到第一，gh-proxy 降为第二。 */
static const char *const OTA_BASES[] = { OTA_NET_GHPROXY2, OTA_NET_GHPROXY1, OTA_NET_JSDELIVR, OTA_NET_GITHUB };
#define OTA_NBASE  (sizeof(OTA_BASES) / sizeof(OTA_BASES[0]))
static int s_base_used = 0;      /* 本次成功拉到清单的源下标（固件也用它，避免跨源版本不一致） */
static int s_ord[OTA_NBASE];     /* 按居住地排好的访问顺序：国内→镜像先，海外→GitHub 源站先 */
static bool s_ord_ready = false;

/* ★R218：按配网时选的居住城市决定先连哪家 ——
   国内（含未配网/空）→ 先试 GitHub 反代镜像（gh-proxy / ghfast）；海外 → GitHub 源站先。
   判据 = NVS(petns).city 的形态：海外存「国家码:城市」如 "AE:Dubai"（见 prov_world_data.h），
   中国城市恒为中文名（"上海市" / "江苏省南京市"），首字符不是 ASCII 大写字母。
   注意：这里**只调顺序**，两家照旧互相兜底，选错城市最多慢一点，不会拉不到包。 */
static bool geo_is_overseas(void)
{
    nvs_handle_t h;
    char city[64];
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(city) - 1;
    esp_err_t e = nvs_get_str(h, "city", city, &len);
    nvs_close(h);
    if (e != ESP_OK || !city[0]) return false;
    return city[0] >= 'A' && city[0] <= 'Z'
        && city[1] >= 'A' && city[1] <= 'Z'
        && city[2] == ':';
}

static void ota_net_build_order(void)
{
    bool os = geo_is_overseas();
    for (int i = 0; i < (int)OTA_NBASE; ++i)
        s_ord[i] = os ? (int)OTA_NBASE - 1 - i : i;   /* 海外 = 倒序（GitHub 先） */
    s_ord_ready = true;
    ESP_LOGI(NET_TAG, "居住地判定: %s → 优先 %s", os ? "海外" : "国内/未知", OTA_BASES[s_ord[0]]);
}

#define OTA_NET_BUF     4096
#define OTA_NET_TO_MS   15000
#define OTA_DL_CHUNK    16384   /* ★R218：4K → 16K，镜像/跨国链路下少一半 TLS 记录往返（实测 199s 的下载靠这个提速） */

static char s_manifest_url[256];
static char s_fw_url[256];
static char s_new_ver[16];
static int  s_expect_size = 0;


/* ── HTTP GET 到内存（上限 cap 字节），返回实际长度；<0 失败 ── */
static int http_get_mem(const char *url, char *buf, int cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_NET_TO_MS,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    esp_err_t e = esp_http_client_open(cli, 0);
    if (e != ESP_OK) { ESP_LOGE(NET_TAG, "open %s 失败: %s", url, esp_err_to_name(e)); esp_http_client_cleanup(cli); return -1; }
    int len = esp_http_client_fetch_headers(cli);
    int code = esp_http_client_get_status_code(cli);
    /* ★R218 修（2026-11-16 实机）：反代镜像（gh-proxy 等）对 JSON 也可能回 chunked，
       fetch_headers 拿不到长度（len<=0）。原来直接判"不通"，白扔一个可用源还多等 6 秒。
       现在：HTTP 200 就继续读，读到 EOF 为止，再用"实收 < cap"兜底。 */
    if (code != 200) {
        ESP_LOGE(NET_TAG, "HTTP %d len=%d", code, len);
        esp_http_client_close(cli); esp_http_client_cleanup(cli); return -1;
    }
    if (len >= cap) {
        ESP_LOGE(NET_TAG, "响应 %d B 超缓冲 %d B", len, cap);
        esp_http_client_close(cli); esp_http_client_cleanup(cli); return -1;
    }
    int got = 0;
    while (got < cap - 1) {
        int r = esp_http_client_read(cli, buf + got, cap - 1 - got);
        if (r <= 0) break;          /* chunked 结尾 / 连接关闭 */
        got += r;
    }
    buf[got > 0 ? got : 0] = 0;
    if (got <= 0) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return -1; }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return got;
}

/* ── 检查线上清单 ── */
void ota_net_check(void)
{
    s_new_ver[0] = 0;
    s_fw_url[0] = 0;
    /* ★★★R218 修（用户 2026-11-16 实报：「点在线升级，先显示读 TF卡，然后马上说两个
       升级源都连不上——我觉得它根本没连」）：
       根因 = 在线这条路**从来没有先联网**。TF 路只读卡，不需要网；在线路直接发 HTTP，
       而设备平时同步完就把 WiFi 关了（省电），此刻 STA 没 IP ⇒ esp_http_client_open
       立刻失败 ⇒ 两个源在几毫秒内轮完 ⇒ 屏上就是「秒弹两个源都连不上」。
       修法：先确保联网再谈源。ws_wifi_connect 自带「已连且有 IP 就立即返回」的早退
       （见 net_sync.c 顶部那段 20 秒假死修复），所以已联网时不会额外等。 */
    if (!g_app.cfg || !g_app.cfg->valid || !g_app.cfg->ssid[0]) {
        ota_net_fail("还没配网");            /* 没配置过 WiFi，先有网再升级 */
        return;
    }
    ESP_LOGI(NET_TAG, "在线升级前先确保联网（已连会立即返回）");
    if (!ws_wifi_connect(g_app.cfg, 15000)) {
        ota_net_fail("没连上 WiFi");         /* 15s 内没拿到 IP，给明确原因 */
        return;
    }
    ESP_LOGI(NET_TAG, "联网 OK，开始查清单");
#if TOUCH_DEMO
    const char *man = "version_touch.json";
#else
    const char *man = "version_key.json";
#endif
    /* 双源回退：依次试 Gitee → GitHub，谁通用谁。清单和固件都锁定同一个源，
       避免「Gitee 清单说 0.218、GitHub 固件还是 0.217」的跨源版本错配。 */
    char *js = (char *)malloc(OTA_NET_BUF);
    if (!js) { ota_net_fail("内存不足"); return; }
    ota_net_build_order();          /* 先按居住地排好源顺序，再逐个试 */
    int n = -1;
    for (int b = 0; b < (int)OTA_NBASE; ++b) {
        snprintf(s_manifest_url, sizeof(s_manifest_url), "%s/%s", OTA_BASES[s_ord[b]], man);
        ESP_LOGI(NET_TAG, "在线检查[%d/%d]: %s", b + 1, (int)OTA_NBASE, s_manifest_url);
        n = http_get_mem(s_manifest_url, js, OTA_NET_BUF);
        if (n > 0) { s_base_used = b; break; }
        ESP_LOGW(NET_TAG, "源 %s 不通，试下一个", OTA_BASES[s_ord[b]]);
    }
    if (n <= 0) { free(js); ota_net_fail("连不上升级服务器"); return; }   /* 网络已通但两家都拉不到清单 */
    ESP_LOGI(NET_TAG, "使用源: %s", OTA_BASES[s_ord[s_base_used]]);

    cJSON *root = cJSON_Parse(js);
    free(js);
    if (!root) { ota_net_fail("清单格式错误"); return; }
    cJSON *jv = cJSON_GetObjectItem(root, "ver");
    cJSON *ju = cJSON_GetObjectItem(root, "url");
    cJSON *jz = cJSON_GetObjectItem(root, "size");
    if (!cJSON_IsString(jv) || !cJSON_IsString(ju) || !cJSON_IsNumber(jz)) {
        cJSON_Delete(root); ota_net_fail("清单缺字段"); return;
    }
    const char *ver = jv->valuestring;
    if (!ota_ver_newer(ver)) {
        ESP_LOGI(NET_TAG, "线上 v%s 不比本机新", ver);
        cJSON_Delete(root);
        ota_net_newest();
        return;
    }
    /* 先截到临时缓冲再拼 v 前缀，避免编译器 -Wformat-truncation 误判 */
    char ver_buf[14];
    snprintf(ver_buf, sizeof(ver_buf), "%s", ver);
    snprintf(s_new_ver, sizeof(s_new_ver), "v%s", ver_buf);
    /* ★R218：不用清单里的绝对 url（那是打包时写死的源）——取文件名 + 当前成功源重组，
       这样 Gitee 拉到清单后，Gitee 固件地址自动对；换成 GitHub 源也自动对。 */
    const char *slash = strrchr(ju->valuestring, '/');
    const char *fname = slash ? slash + 1 : ju->valuestring;
    snprintf(s_fw_url, sizeof(s_fw_url), "%s/%s", OTA_BASES[s_ord[s_base_used]], fname);
    s_expect_size = jz->valueint;
    cJSON_Delete(root);

    if (s_expect_size <= 0 || s_expect_size > 0x1F0000) {
        ota_net_fail("安装包大小异常"); return;
    }
    ota_net_found(s_new_ver, s_expect_size / 1024);
}

/* ── 下载 bin 并写非运行槽 ── */
/* ★★★R218（2026-11-17 08:25）：下载期间**必须关掉 WiFi modem sleep**。
   实测：同一时刻 PC 从 ghfast 拉 1.8MB 只要 2.97s（≈600KB/s），设备却 123~128s
   （≈14KB/s）。代码里从没调过 esp_wifi_set_ps()，走的是 IDF 默认 WIFI_PS_MIN_MODEM
   —— 大包传输时每收一段就要等 AP 的 DTIM 唤醒，吞吐被砍到十几 KB/s。
   这里在整段下载前后成对切换：下载中用 WIFI_PS_NONE（全速），结束恢复 MIN_MODEM（省电）。
   ⚠ 只包 download，不影响待机功耗曲线。 */
static void ota_net_download_body(void);

void ota_net_download(void)
{
    esp_wifi_set_ps(WIFI_PS_NONE);
    ota_net_download_body();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

static void ota_net_download_body(void)
{
    if (!s_fw_url[0]) { ota_net_fail("没有固件地址"); return; }
    if (!s_ord_ready) ota_net_build_order();   /* 兜底：极端情况下顺序表还没建好 */
    /* ★R218：固件也双源回退，但**只在开始写分区之前**换源
       （一旦 ota_net_begin() 拿了 esp_ota_begin 句柄，换源重来会污染 OTA 槽）。 */
    esp_http_client_handle_t cli = NULL;
    const char *slash = strrchr(s_fw_url, '/');
    const char *fname = slash ? slash + 1 : "firmware.bin";
    for (int k = 0; k < (int)OTA_NBASE && !cli; ++k) {
        int b = (s_base_used + k) % (int)OTA_NBASE;   /* 从清单成功的源起，绕回开头再兜底 */
        char url[256];
        snprintf(url, sizeof(url), "%s/%s", OTA_BASES[s_ord[b]], fname);
        ESP_LOGI(NET_TAG, "在线下载[%d/%d]: %s (%d B)", k + 1, (int)OTA_NBASE, url, s_expect_size);
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = OTA_NET_TO_MS,
            .disable_auto_redirect = false,
            .max_redirection_count = 5,
        };
        esp_http_client_handle_t h = esp_http_client_init(&cfg);
        if (!h) continue;
        if (esp_http_client_open(h, 0) != ESP_OK) { esp_http_client_cleanup(h); continue; }
        int clen = esp_http_client_fetch_headers(h);
        /* ★R218：jsDelivr 这类 CDN 对大文件返回 Transfer-Encoding: chunked，
           fetch_headers 拿不到长度（clen < 0）。只要 HTTP 200 就收，
           最终以「实收字节 == 清单 size」为准（下面 while 循环与外层判断都在做这件事）。 */
        bool len_ok = (clen < 0) || (clen == s_expect_size);
        if (!len_ok || esp_http_client_get_status_code(h) != 200) {
            ESP_LOGE(NET_TAG, "源 %s 大小不符: 清单 %d / 实际 %d", OTA_BASES[s_ord[b]], s_expect_size, clen);
            esp_http_client_close(h); esp_http_client_cleanup(h); continue;
        }
        cli = h;   /* 这个源可用，锁定它开始写 */
    }
    if (!cli) { ota_net_fail("两个源都下不到固件"); return; }
    /* 通过 ota_up 导出的流式写入 API（内部校验 magic + esp_ota_end） */
    if (!ota_net_begin(s_expect_size)) {
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        ota_net_fail("OTA 分区准备失败"); return;
    }
    uint8_t *chunk = (uint8_t *)malloc(OTA_DL_CHUNK);
    if (!chunk) {
        esp_http_client_close(cli); esp_http_client_cleanup(cli);
        ota_net_abort(); ota_net_fail("内存不足"); return;
    }
    int total = 0;
    bool ok = true;
    while (total < s_expect_size) {
        int want = s_expect_size - total;
        if (want > OTA_DL_CHUNK) want = OTA_DL_CHUNK;
        int got = esp_http_client_read(cli, (char *)chunk, want);
        if (got <= 0) { ok = false; break; }
        if (!ota_net_feed(chunk, (size_t)got)) { ok = false; break; }
        total += got;
        ota_net_progress(100 * total / s_expect_size);
        /* ★2026-11-17：这里原来每个 chunk 调一次 esp_task_wdt_reset()，但 OTA 任务
           **没有** esp_task_wdt_add() 订阅 → 每调一次就打印
           `task_wdt: esp_task_wdt_reset(707): task not found` 并 fflush，
           1.8MB/16K = 110 次，白耗时间还把日志刷满。未订阅就不喂，删掉。 */
    }
    free(chunk);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (!ok || total != s_expect_size) {
        ota_net_abort(); ota_net_fail("下载中断"); return;
    }
    if (!ota_net_finalize()) { ota_net_fail("固件校验失败"); return; }
    ota_net_done();
}
