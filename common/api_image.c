/* api_image.c — 图像通道核心件 v1。口径见 api_image.h。 */
#include "api_image.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>            /* ★R159：powf（灰度提亮的 gamma 查表）*/
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_random.h"
#include <time.h>
#include "cat_sprites.h"     /* ★R158：猫精灵（5 姿态，坐标与 photo buffer 1:1） */
#include <netdb.h>           /* ★2026-11-12：getaddrinfo（图床 DNS 诊断）*/
#include <arpa/inet.h>       /* ★2026-11-12：inet_ntoa */

/* ★R189：标记落箱来源（定义在 post_system.c）。这里**不 include post_system.h** ——
   那个头会拉进 main_api.h，而本文件有自己的 `static const char *TAG`，会撞名。 */
void mypost_src_hint(uint8_t src);

static const char *TAG = "apiimg";

#define AI_URL_DEF     "https://dashscope.aliyuncs.com"
#define AI_MODEL_DEF   "wanx2.1-t2i-turbo"
/* ★R157：多服务商适配 —— 只按「图像接口地址」自动识别，不加任何配网页字段（WEBUI 在 assets 里）。
   · dashscope / aliyuncs → 阿里云百炼异步（提交 task_id → 轮询 → 下载，原逻辑保留）
   · image.pollinations.ai → GET 直出 JPEG，零 key、零轮询
   · 其它（智谱 cogview-3-flash / 任意 OpenAI 风格 /images/generations）→ 一次 POST 同步拿 data[0].url
   模型名只作为请求参数透传，「用同一把 key」时请在配网页把模型改掉（默认 wanx 只适用于百炼）。 */
typedef enum { AI_P_DASHSCOPE = 0, AI_P_OPENAI_IMG, AI_P_URL_GET } ai_prov_t;
static ai_prov_t ai_provider(const char *base)
{
    if (strstr(base, "pollinations")) return AI_P_URL_GET;
    if (strstr(base, "dashscope") || strstr(base, "aliyuncs")) return AI_P_DASHSCOPE;
    return AI_P_OPENAI_IMG;
}
static const char *ai_prov_name(ai_prov_t p)
{
    return p == AI_P_URL_GET ? "GET直出图" :
           p == AI_P_OPENAI_IMG ? "同步images/generations" : "百炼异步";
}
static void ai_url_enc(const char *s, char *out, size_t cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else {
            out[o++] = '%'; out[o++] = HEX[c >> 4]; out[o++] = HEX[c & 15];
        }
    }
    out[o] = 0;
}
#define AI_POLL_TIMES  30          /* 轮询 30 次 × 3s = 90s 上限 */
#define AI_DL_MAX      (1024u * 1024u)   /* 下载上限 1MB */
/* ★R119：落箱字段上限，必须与 main_api.h 的 MYPOST_TEXT_MAX/TITLE_MAX/ADDR_MAX 一致。
   （不直接 include main_api.h：那里 extern TAG 与本源文件的 static TAG 冲突） */
#define AI_POST_TEXT_MAX   180
#define AI_POST_TITLE_MAX  30
#define AI_POST_ADDR_MAX   36

static api_image_sink_fn s_sink = NULL;
void api_image_set_sink(api_image_sink_fn fn) { s_sink = fn; }

static void ai_keys(char *key, int kcap, char *base, int bcap, char *model, int mcap)
{
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
        /* ★R207 修（复查）：`api_same_key=1`（用户勾「与文字通道用同一把 Key 和地址」）
           时，必须先**忽略**存量的 api_img_key / api_img_url —— 原实现无条件先读它们，
           勾了同一把 key 后仍拿旧 key 去鉴权 ⇒ 401，而页面显示"已保存"。
           与 main_api.h / api_image.h 的口径（same=1 → 用 api_key/api_url）对齐。 */
        int32_t same = 0;
        nvs_get_i32(h, "api_same_key", &same);
        if (same != 1) {
            size_t l = kcap;  nvs_get_str(h, "api_img_key",  key,  &l);
            l = bcap;         nvs_get_str(h, "api_img_url",  base, &l);
        }
        if (!key[0])  { size_t l = kcap; nvs_get_str(h, "api_key", key,  &l); }   /* 兜底/同 key */
        if (!base[0]) { size_t l = bcap; nvs_get_str(h, "api_url", base, &l); }
        size_t lm = mcap; nvs_get_str(h, "api_img_model", model, &lm);
        nvs_close(h);
    }
    if (!base[0])  snprintf(base,  bcap,  "%s", AI_URL_DEF);
    if (!model[0]) snprintf(model, mcap, "%s", AI_MODEL_DEF);
    /* ★R158：pollinations 文字与图片**不在同一子域**（text. / image.），
       共用地址时要把它换算成图片子域，否则拼出 text.pollinations.ai/openai/prompt/... 必 404。 */
    if (strstr(base, "text.pollinations.ai"))
        snprintf(base, bcap, "%s", "https://image.pollinations.ai");
    /* base 去尾斜杠（拼接 /api/v1/...） */
    int bl = (int)strlen(base);
    while (bl > 0 && base[bl - 1] == '/') base[--bl] = 0;   /* ★R216：原来 bl 是 const 且不自减，只去掉一个 / */
}

bool api_image_ready(void)
{
    /* ★2026-10-24 深夜 bug 修：原来 base/model 传 8 字节缓冲去接 NVS 的长 URL
       （nvs_get_str 报长度错不写入还算万幸，紧跟着的 snprintf(base, 8, AI_URL_DEF)
       会把 28 字节默认值写进 8 字节栈缓冲 = 栈越界写）。给足尺寸。 */
    char key[160] = {0}, base[160] = {0}, model[64] = {0};
    ai_keys(key, sizeof(key), base, sizeof(base), model, sizeof(model));
    return key[0] != 0;
}

/* ── 简化 HTTP 工具 ─────────────────────────────────────────────── */
/* ★R207 修（复查 · 真 bug）：http_get 原来 perform 之后才 read —— perform 内部已把响应体
   读完并**丢弃**（没挂 event_handler），随后 read 恒为 0 ⇒ 百炼异步轮询拿到空串，
   永远拿不到 url（90 秒后判失败）。与 http_post_json 同一修法：挂 ON_DATA 收 body。
   ⚠ 所以 http_sink_t / http_on_data 必须定义在 http_get **之前**。 */
typedef struct { char *buf; int cap; int len; } http_sink_t;

static esp_err_t http_on_data(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        http_sink_t *s = (http_sink_t *)e->user_data;
        int n = e->data_len;
        if (s->len + n > s->cap - 1) n = s->cap - 1 - s->len;
        if (n > 0 && s->buf) { memcpy(s->buf + s->len, e->data, (size_t)n); s->len += n; }
    }
    return ESP_OK;
}

static bool http_get(const char *url, const char *auth, char *out, int cap, int *code)
{
    http_sink_t sink = { out, cap, 0 };
    if (out && cap > 0) out[0] = 0;
    esp_http_client_config_t hc = {
        .url = url, .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .event_handler = http_on_data,
        .user_data = &sink,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) return false;
    if (auth) esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_header(cli, "Accept-Encoding", "identity");   /* 别给 gzip，cJSON 解析不了 */
    esp_err_t err = esp_http_client_perform(cli);
    *code = esp_http_client_get_status_code(cli);
    int got = sink.len;
    if (got < 0) got = 0;
    if (got > cap - 1) got = cap - 1;
    if (out) out[got] = 0;
    esp_http_client_cleanup(cli);
    return err == ESP_OK && *code == 200;
}

/* ★R159：把「HTTP 状态码」和「Retry-After」带回给调用方。
   免费图源（pollinations 匿名档）按公网 IP 限流：**同时 1 个、间隔 ≥45 秒**，
   超了回 402 + Retry-After。402 不是故障，不能算进熔断 —— 否则邻居/同 CGNAT
   出口的用户一撞限流，点三次就把今天废了。 */
static uint8_t *http_get_bin(const char *url, const char *auth,
                             size_t *out_len, int *out_code, int *out_wait)
{
    *out_len = 0;
    if (out_code) *out_code = 0;
    if (out_wait) *out_wait = 0;
    esp_http_client_config_t hc = {
        .url = url, .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        /* ★2026-11-12：智谱图床（UCloud maas-watermark-*.ufileos.com）在设备上 TLS 握不上
           （err=0x7002，同一域名在电脑上完全正常）。先跳过证书 CN 校验试一次；
           图片本身不是敏感数据，这条只作用于**下载图片**。 */
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) return NULL;
    /* ★R160：GET 直出图原来**不发 Authorization** —— 用户就算在配网页填了自己的
       pollinations token，匿名额度还是按公网 IP 算，白填。真 token 一并发过去才生效
       （匿名档占位用的 "test" 不发，避免带个假 Bearer 反被 401）。 */
    if (auth && auth[0] && strcmp(auth, "Bearer test") != 0)
        esp_http_client_set_header(cli, "Authorization", auth);
    /* ★★★2026-11-12（实机 err=0x7002 连不上智谱图床 maas-watermark-prod-new.cn-wlcb.ufileos.com，
       但同一域名在电脑上 DNS/TCP/HTTPS 全通）：先把域名解析一次打出来，
       区分「DNS 解析不出」「解析出来但 TCP 连不上」「TCP 通但 TLS 握手失败」三种情况。 */
    {
        const char *hs = strstr(url, "://"); hs = hs ? hs + 3 : url;
        char host[96]; int hn = 0;
        while (hs[hn] && hs[hn] != '/' && hs[hn] != ':' && hn < (int)sizeof(host) - 1) { host[hn] = hs[hn]; ++hn; }
        host[hn] = 0;
        struct addrinfo hints; memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *ai = NULL;
        const char *port = (strncmp(url, "https://", 8) == 0) ? "443" : "80";
        const int gr = getaddrinfo(host, port, &hints, &ai);
        if (gr == 0 && ai) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ai->ai_addr;
            ESP_LOGW(TAG, "下载: DNS %s → %s", host, inet_ntoa(sa->sin_addr));
            freeaddrinfo(ai);
        } else {
            ESP_LOGW(TAG, "下载: **DNS 解析失败** %s err=%d", host, gr);
        }
    }

    /* ★2026-11-12 诊断：智谱这条通道已经打通到「拿到 url」，卡在下载建连。
       原来 open 失败只打四个字，看不出是 DNS、TLS 还是 URL 被截断 ——
       这里把 esp_err_t 和实际 URL 一起打出来（URL 最长 150 字符，够看清域名）。 */
    const esp_err_t oe = esp_http_client_open(cli, 0);
    if (oe != ESP_OK) {
        ESP_LOGW(TAG, "下载: open 失败 err=%d(0x%X) url=%.150s", (int)oe, (unsigned)oe, url);
        esp_http_client_cleanup(cli);
        /* ★2026-11-12 第二道兜底：https 握不上就换 http 再试一次（UCloud 通常 80/443 同时开）。
           只对**下载图片**生效；url 已经是 http:// 时不会再递归，保证只换一次。 */
        if (strncmp(url, "https://", 8) == 0) {
            /* ★2026-11-12：放 static（BSS → PSRAM）不放栈 —— 这里会递归调用自己，
               两层 768B 叠在已经背着 http + jpeg 解码的调用链上，本项目吃过栈溢出的亏。
               函数不重入（httpd 单任务串行），递归只一层，且递归后不再读这块。 */
            static EXT_RAM_BSS_ATTR char alt[768];
            snprintf(alt, sizeof(alt), "http://%s", url + 8);
            ESP_LOGW(TAG, "下载: https 失败 → 改用 http 重试 %.110s", alt);
            return http_get_bin(alt, auth, out_len, out_code, out_wait);
        }
        return NULL;
    }
    const int clen = esp_http_client_fetch_headers(cli);
    const int hcode = esp_http_client_get_status_code(cli);
    if (out_code) *out_code = hcode;
    if (hcode != 200) {
        /* 402/429 = 限流；别的码 = 真错（403 权限、404 地址拼错、5xx 服务端） */
        char *ra = NULL;
        if ((hcode == 402 || hcode == 429) && esp_http_client_get_header(cli, "Retry-After", &ra) == ESP_OK && ra) {
            /* ★2026-11-12 修：atoi 对 "12abc" 静默取 12、对非数字返回 0。
               改 strtol + endptr 全串校验，尾部有杂质一律不采信。 */
            char *end = NULL;
            long w = strtol(ra, &end, 10);
            if (end && end != ra && *end == '\0' && w > 0 && w <= 120 && out_wait)
                *out_wait = (int)w;
        }
        ESP_LOGW(TAG, "下载: http=%d（非 200，不是图片）%s", hcode,
                 (hcode == 402 || hcode == 429) ? " = 限流" : "");
        esp_http_client_close(cli); esp_http_client_cleanup(cli); return NULL;
    }
    /* ★R157：免费图源常用 chunked（无 Content-Length），旧写法 clen<=0 直接放弃。
       改成「有长度按长度读、没长度按上限流式读」，返回实际字节数。 */
    if (clen > (int)AI_DL_MAX) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return NULL; }
    const int want = (clen > 0) ? clen : (int)AI_DL_MAX;
    uint8_t *buf = heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { esp_http_client_close(cli); esp_http_client_cleanup(cli); return NULL; }
    int got = 0, n;
    while (got < want && (n = esp_http_client_read(cli, (char *)buf + got, want - got)) > 0) got += n;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    if (got == 0 || (clen > 0 && got != clen)) { free(buf); return NULL; }
    *out_len = (size_t)got;
    return buf;
}

/* ★★★2026-11-12（实机铁证：`同步生图无 url：http=200 len=0 resp=`）：
   **响应体必须在 event_handler 里收，不能 perform 之后再 read。**
   esp_http_client_perform() 内部会把整个响应体读完；没有 event_handler 时这些数据
   直接被丢弃，之后再调 esp_http_client_read() 只能拿到 0 ⇒ cJSON 解析空串 ⇒
   「http 明明 200 却无 url」，表现为「转很久然后失败」。pollinations 那条路是
   GET 直接下 JPEG（另一段代码），不受影响，所以这个坑一直没暴露。
   这里挂一个 ON_DATA 回调把 body 抄进 out；顺带要求 identity 编码，
   免得对端返回 gzip 时收到的是压缩字节（cJSON 同样解析不了）。
   ★R207：http_sink_t / http_on_data 已上移到 http_get 之前（两处共用同一份）。 */

static bool http_post_json(const char *url, const char *auth, const char *body,
                           char *out, int cap, bool dashscope_async, int *out_code)
{
    http_sink_t sink = { out, cap, 0 };
    if (out && cap > 0) out[0] = 0;
    esp_http_client_config_t hc = {
        .url = url, .timeout_ms = 15000, .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
        .event_handler = http_on_data,
        .user_data = &sink,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&hc);
    if (!cli) return false;
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_header(cli, "Accept-Encoding", "identity");   /* 别给 gzip，cJSON 解析不了 */
    if (dashscope_async)                                            /* ★R157：仅百炼需要 */
        esp_http_client_set_header(cli, "X-DashScope-Async", "enable");
    esp_http_client_set_post_field(cli, body, (int)strlen(body));
    const esp_err_t err = esp_http_client_perform(cli);
    const int code = esp_http_client_get_status_code(cli);
    if (out_code) *out_code = code;      /* ★R157：修「失败时日志恒打 http=0」 */
    int got = sink.len;
    if (got < 0) got = 0;
    if (got > cap - 1) got = cap - 1;
    out[got] = 0;
    esp_http_client_cleanup(cli);
    return err == ESP_OK && (code == 200 || code == 201);
}

/* ── JPEG → 164×108 1-bit（Bayer 4×4 抖动，1=黑，行 21B —— 与 post.bin 同格式）── */
static const uint8_t BAYER[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
#define POST_W 164
#define POST_H 108
#define POST_ROW 21
#define POST_BYTES (POST_ROW * POST_H)          /* 2268 */

/* ★R166：把源窗口 (wx,wy,ww,wh) 缩到 164×108 灰度（取景 / 白框收窄后都要重跑）
   ★★★2026-11-12（用户报「线条有点不连贯，看不清生成了什么」）：
   原实现是**点采样**（每个输出像素只取源图对应位置的一个点）。cogview 出的是线描，
   源图 1024×1024 缩到 164×108 是缩小 6 倍多 ⇒ 一条 1px 的线在 6 个格子里只有 1 个
   会被采到，剩下 5 个直接跳过 ⇒ 线断成一截一截。
   改成「取该格子内**最暗**的像素」（形态学腐蚀的等价效果）：线再细也不会被跳过，
   线描会连起来。照片类图会整体偏暗一点，但后面 post_quantize 有分位拉伸 + gamma
   提亮，能被拉回去。 */
static void post_sample(const uint8_t *rgb, int srcW, int wx, int wy, int ww, int wh, uint8_t *grey)
{
    for (int y = 0; y < POST_H; ++y) {
        const int sy0 = wy + y * wh / POST_H;
        int sy1 = wy + (y + 1) * wh / POST_H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < POST_W; ++x) {
            const int sx0 = wx + x * ww / POST_W;
            int sx1 = wx + (x + 1) * ww / POST_W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int mn = 255, sum = 0, cnt = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t *row = rgb + (size_t)sy * (size_t)srcW * 3u;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint8_t *p = row + (size_t)sx * 3u;
                    const int v = (p[0] * 3 + p[1] * 6 + p[2]) / 10;
                    if (v < mn) mn = v;
                    sum += v; ++cnt;
                }
            }
            /* ★2026-11-12 复核补：纯 min 采样对**照片类**图会把暗部按 6×9 倍放大
               （等价形态学腐蚀）⇒ 整图发灰发暗、层次塌掉；而线描的线本身是纯黑，
               混入平均后仍然是深色，线不会断。
               ⇒ min:avg = 2:1 —— 线描保线、照片不失层次。 */
            grey[y * POST_W + x] = (uint8_t)((mn * 2 + (cnt ? sum / cnt : mn)) / 3);
        }
    }
}

/* ★R167：这条源像素列是不是「白框」——判据用**暗点占比**，不是亮度阈值。
   实测（pollinations 出图，256 宽、只看水印带以上 204 行）：
     · _real_11 左边 0..7 列：暗点 0 个，但「亮度≥236 的占比」只有 75%~84%
       ⇒ R166 那版要求 95% 亮像素，于是**一列都没判成白框**，白边原样上屏（用户当场否掉）。
     · 同一张右边 248..254 列暗点 0，第 255 列有 18 个噪点（8.8%）撑住包围盒。
   ⇒ 改为「暗点 ≤ 10% 且 平均亮度 ≥ 228」：白框必满足，浅色天空（如 _t_guilin
     左列暗点 5%~10%、亮度 237~251）也会被判掉，但那本来就是一整片白，删掉无害；
     而 _real_22 左列暗点 54%、_t_en 左列 72% 立即停手，不会误裁构图。 */
static bool post_col_is_blank(const uint8_t *rgb, int srcW, int rows, int x)
{
    int ink = 0;
    long sum = 0;
    for (int y = 0; y < rows; ++y) {
        const uint8_t *p = rgb + ((size_t)y * srcW + x) * 3;
        const int v = (p[0] * 3 + p[1] * 6 + p[2]) / 10;
        sum += v;
        if (v < 224) ++ink;
    }
    return (long)ink * 10 <= (long)rows && sum >= (long)rows * 228;
}

/* ★★2026-11-12 用户令（实机报「上左右都有黑色的部分」）：
   模型有时会自作主张画一圈**黑色画框/暗角**。白框裁剪只认「整列都亮」，黑框永远裁不掉，
   于是 164:108 的取景把上、左、右三边黑框留在画面上（下边被裁掉了，所以只看到三边）。
   判据反过来：整列/整行 90% 以上是近黑（<64）⇒ 这是边框，不是画面内容。
   限制由调用方给（单侧最多裁 1/6），避免把真正的深色夜景整片裁掉。 */
static bool post_col_is_dark(const uint8_t *rgb, int srcW, int rows, int x)
{
    int dark = 0;
    for (int y = 0; y < rows; ++y) {
        const uint8_t *p = rgb + ((size_t)y * srcW + x) * 3;
        if ((p[0] * 3 + p[1] * 6 + p[2]) / 10 < 64) ++dark;
    }
    return (long)dark * 10 >= (long)rows * 9;
}

static bool post_row_is_dark(const uint8_t *rgb, int srcW, int x0, int x1, int y)
{
    if (x1 < x0) return false;
    int dark = 0, n = 0;
    for (int x = x0; x <= x1; ++x) {
        const uint8_t *p = rgb + ((size_t)y * srcW + x) * 3;
        if ((p[0] * 3 + p[1] * 6 + p[2]) / 10 < 64) ++dark;
        ++n;
    }
    return n > 0 && (long)dark * 10 >= (long)n * 9;
}

static void post_quantize(const uint8_t *grey, uint8_t *out2268);

/* ★R159/R163：灰度 → 直方图自适应拉伸 + 反推 gamma + Bayer 抖动（1=墨） */
static void post_quantize(const uint8_t *grey, uint8_t *out2268)
{
    uint32_t hist[256] = {0};
    for (int i = 0; i < POST_W * POST_H; ++i) ++hist[grey[i]];
    const uint32_t total = (uint32_t)(POST_W * POST_H);
    const uint32_t lo_q = total * 2u / 100u, hi_q = total * 98u / 100u;
    int lo = 0, hi = 255;
    uint32_t acc = 0;
    for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc >= lo_q) { lo = i; break; } }
    acc = 0;
    for (int i = 0; i < 256; ++i) { acc += hist[i]; if (acc >= hi_q) { hi = i; break; } }
    if (hi - lo < 24) { lo = 0; hi = 255; }        /* 本来就平/纯色 → 不硬拉，避免放大噪点 */
    uint32_t sum = 0;                              /* 拉伸后的平均亮度（用直方图算，不另遍历） */
    for (int i = 0; i < 256; ++i) {
        int v = (int)(((long)(i - lo) * 255) / (hi - lo));
        if (v < 0) v = 0; else if (v > 255) v = 255;
        sum += (uint32_t)hist[i] * (uint32_t)v;
    }
    const float mean = (float)sum / (float)total;
    const float target = 189.0f;                   /* 纸面目标 ≈ 74% 白 */
    float gamma = 1.0f;
    if (mean > 4.0f && mean < target)
        gamma = logf(target / 255.0f) / logf(mean / 255.0f);
    if (gamma < 1.0f) gamma = 1.0f;
    if (gamma > 2.0f) gamma = 2.0f;
    ESP_LOGW(TAG, "调色：lo=%d hi=%d 均值=%.0f gamma=%.2f", lo, hi, mean, gamma);

    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
        float v = (float)(i - lo) * 255.0f / (float)(hi - lo);
        if (v < 0.0f) v = 0.0f; else if (v > 255.0f) v = 255.0f;
        v = 255.0f * powf(v / 255.0f, 1.0f / gamma);           /* 提亮纸白、压深线 */
        lut[i] = (uint8_t)(v + 0.5f);
    }
    memset(out2268, 0, POST_BYTES);
    uint32_t ink = 0;
    for (int y = 0; y < POST_H; ++y) {
        for (int x = 0; x < POST_W; ++x) {
            const int g = lut[grey[y * POST_W + x]];
            const int th = (BAYER[(y & 3) * 4 + (x & 3)] + 1) * 16 - 8;   /* 8..248 */
            if (g < th) { out2268[y * POST_ROW + (x >> 3)] |= 0x80 >> (x & 7); ++ink; }
        }
    }
    ESP_LOGW(TAG, "抖动：墨点 %.1f%%%s", ink * 100.0f / (float)total,
             (ink * 100u > total * 78u) ? "（偏深：画面可能过暗）" : "");
    /* ★2026-11-12 兜底：万一 RGB 那层判据没命中（底片带一层灰、或对比度被拉过），
       1bit 这一层再兜一次。阈值取 **80%**（不是 70%）：实测线描墨点 8~18%、
       写实照片 30~60%、深色夜景照片 65~75% —— 70% 会把夜景误翻成负片，
       80% 只可能出现在「整张近乎全黑」这种真异常上。 */
    if (ink * 100u > total * 80u) {
        for (int i = 0; i < POST_BYTES; ++i) out2268[i] = (uint8_t)~out2268[i];
        ESP_LOGW(TAG, "★ 墨点超 80%% → 判定整张反相，已翻正为 %.1f%%",
                 (float)(total - ink) * 100.0f / (float)total);
        ink = total - ink;
    }
}

static bool jpg_to_post_1bit(const uint8_t *jpg, size_t jlen, uint8_t *out2268)
{
    esp_jpeg_image_cfg_t cfg = { 0 };
    esp_jpeg_image_output_t out = { 0 };
    cfg.indata = (uint8_t *)jpg;
    cfg.indata_size = (uint32_t)jlen;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;
    /* ★R170 全检：原来只判 width==0。height==0 或超大尺寸会让 width*height*3
       在 uint32 上溢出成一个小数，分配过小的缓冲后解码越界写。 */
    if (esp_jpeg_get_image_info(&cfg, &out) != ESP_OK ||
        out.width == 0 || out.height == 0 || out.width > 4096 || out.height > 4096) {
        ESP_LOGW(TAG, "jpg 头解析失败 / 尺寸非法（%u x %u）",
                 (unsigned)out.width, (unsigned)out.height);
        return false;
    }
    const uint64_t need64 = (uint64_t)out.width * out.height * 3u;
    if (need64 > (uint64_t)AI_DL_MAX * 3u) { ESP_LOGW(TAG, "jpg 解码缓冲需求过大"); return false; }
    const uint32_t need = (uint32_t)need64;
    uint8_t *rgb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb) { ESP_LOGW(TAG, "解码缓冲 %luKB 分配失败", (unsigned long)(need / 1024)); return false; }
    cfg.outbuf = rgb;
    cfg.outbuf_size = need;
    esp_jpeg_image_output_t dec = { 0 };
    const esp_err_t e = esp_jpeg_decode(&cfg, &dec);
    if (e != ESP_OK || dec.width == 0 || dec.height == 0) { free(rgb); ESP_LOGW(TAG, "jpg 解码失败 %d", (int)e); return false; }

    /* ★R163：内容自适应取景（用户实机报「左侧一条明显白边」）——
       免费源经常在画布边上留白框（实测 256 边上左 9px / 右 5px），先前按全宽一刀切，
       白边就被原样带上屏。这里先量墨迹包围盒，再按 164:108 取一个刚好包住内容的窗口：
       白边自然裁掉、主体变大、比例仍不变形；内容本来就满幅的图，行为与旧版等价。
       ⚠ 量包围盒时**排除底部 20%**：那是 pollinations 的水印带，不能因为水印把取景框
         往下带（否则刚修掉的水印又回来了）。 */
    const int srcW = (int)dec.width, srcH = (int)dec.height;

    /* ★★2026-11-12 用户令（实机报「图片反色了，左右两边白块变黑块」）：
       模型有时不按 `white paper` 出图，而是交一张**黑底白线**的底片式线描。
       后面整条链路（抖动按 1=墨 → 落箱前统一取反）是给「白底黑线」设计的，
       底片进来就会被原样反着送上屏。这里在**取景之前**先判一次极性并翻正，
       好处是后面的白边裁剪 / 取景 / 抖动全部照常走，不用在 1bit 层另补一套。
       判据（底片式线描 vs 暗调照片的区别，实测口径）：
         · 近黑像素（<64）占绝对多数 —— 底片里本该是「纸」的地方是黑的；
         · 同时有 3%~45% 的亮像素（>200）—— 那些是白色的线条；
         · 夜景/暗调照片的亮像素通常 <2%，或中间灰阶丰富 ⇒ 不满足，不会被误翻。 */
    {
        long dark = 0, light = 0, tot = 0;
        for (int y = 0; y < srcH; y += 3) {
            for (int x = 0; x < srcW; x += 3) {
                const uint8_t *p = rgb + ((size_t)y * srcW + x) * 3;
                const int v = (p[0] * 3 + p[1] * 6 + p[2]) / 10;
                if (v < 64) ++dark; else if (v > 200) ++light;
                ++tot;
            }
        }
        if (tot > 0 && dark * 100 > tot * 55 && light * 100 > tot * 3 && light * 100 < tot * 45) {
            ESP_LOGW(TAG, "★ 出图是黑底白线（底片式）：暗 %.0f%% / 亮 %.0f%% → 整张翻正",
                     dark * 100.0 / (double)tot, light * 100.0 / (double)tot);
            const size_t npix3 = (size_t)srcW * (size_t)srcH * 3u;
            for (size_t i = 0; i < npix3; ++i) rgb[i] = (uint8_t)(255 - rgb[i]);
        }
    }

    const int wm_lim = srcH * 80 / 100;             /* 底部 20% = 水印带，不参与取景 */

    /* ① 左右白框：从最外列往里推，整列都没有暗点就判白框并收掉（单侧最多 1/4 宽）。
       ★R167：横向边界**不再用墨迹包围盒** —— 包围盒会被边缘一两个噪点顶到画布边
       （_real_11 第 255 列 18 个暗点 ⇒ bx1=255），白框因此整条留下。 */
    int x0 = 0, x1 = srcW - 1;
    const int xlim = srcW / 4;
    while (x0 < xlim && post_col_is_blank(rgb, srcW, wm_lim, x0)) ++x0;
    while (x1 > srcW - 1 - xlim && post_col_is_blank(rgb, srcW, wm_lim, x1)) --x1;
    if (x1 - x0 + 1 < srcW / 2) {                   /* 收得离谱（全白图/纯色图）→ 作废 */
        x0 = 0; x1 = srcW - 1;
    }

    /* ①-b 左右**黑**边框（模型自作主张画的画框/暗角）：整列近黑就裁，单侧最多 1/6 宽。
       这是 2026-11-12 实机那条「上左右都有黑色的部分」的直接对策 —— 白框判据只认亮列，
       黑框永远裁不掉，会原样留在 164:108 的取景窗口里。 */
    {
        const int dlim = srcW / 6;
        const int bx0 = x0, bx1 = x1;
        while (x0 < bx0 + dlim && post_col_is_dark(rgb, srcW, wm_lim, x0)) ++x0;
        while (x1 > bx1 - dlim && post_col_is_dark(rgb, srcW, wm_lim, x1)) --x1;
        if (x1 - x0 + 1 < srcW / 2) { x0 = bx0; x1 = bx1; }   /* 裁得离谱 → 回滚 */
        if (x0 != bx0 || x1 != bx1)
            ESP_LOGW(TAG, "取景：裁掉左右黑框 左%d 右%d 列", x0 - bx0, bx1 - x1);
    }

    /* ② 垂直范围：仍用墨迹包围盒（上下是天空/地面，不能按白框删，只用来定位主体） */
    int by0 = srcH, by1 = -1;
    for (int y = 0; y < wm_lim; y += 2) {
        for (int x = x0; x <= x1; x += 2) {
            const uint8_t *p = rgb + ((size_t)y * srcW + x) * 3;
            if ((p[0] * 3 + p[1] * 6 + p[2]) / 10 < 224) {
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
    }

    /* ②-b 上下**黑**边框（同左、右那条：画框的上下两条边）。黑框本身是"暗"，会被算进
       墨迹包围盒，于是 by0 顶到 0 ⇒ 取窗时上边框留在画面里、下边框被比例裁掉 ——
       这正是用户看到的「上、左、右有黑、底下没有」。 */
    if (by1 > by0) {
        const int dlim = srcH / 6;
        const int oy0 = by0, oy1 = by1;
        while (by0 < oy0 + dlim && by0 < wm_lim && post_row_is_dark(rgb, srcW, x0, x1, by0)) ++by0;
        while (by1 > oy1 - dlim && by1 > by0 && post_row_is_dark(rgb, srcW, x0, x1, by1)) --by1;
        if (by1 <= by0) { by0 = oy0; by1 = oy1; }         /* 裁空了 → 回滚 */
        else if (by0 != oy0 || by1 != oy1)
            ESP_LOGW(TAG, "取景：裁掉上下黑框 上%d 下%d 行", by0 - oy0, oy1 - by1);
    }

    /* ③ 取景窗口：宽度 = 内容宽（绝不包含左右白框），高度按 164:108 换算 */
    int wx, wy, ww, wh;
    const int cw = x1 - x0 + 1;
    ww = cw;
    wh = (int)((long)ww * POST_H / POST_W);
    if (wh > wm_lim) { wh = wm_lim; ww = (int)((long)wh * POST_W / POST_H); }
    if (ww > srcW)   { ww = srcW;  wh = (int)((long)ww * POST_H / POST_W); }
    wx = (x0 + x1) / 2 - ww / 2;
    /* ★R167：窗口必须**完全落在内容区内**。只居中是不够的 —— 居中后左边界常落到
       x0-1 那一列（白框最后一列，暗点 0），抖完还剩 1 列纯白，用户照样看得见。 */
    if (wx < x0) wx = x0;
    if (wx + ww > x1 + 1) wx = x1 + 1 - ww;
    if (by1 > by0) {
        wy = (by0 + by1) / 2 - wh / 2 - wh / 16;   /* 略上偏：主体（塔/楼/山）多在上半 */
    } else {
        wy = (wm_lim - wh) / 2;
    }
    if (wy > wm_lim - wh) wy = wm_lim - wh;        /* 底边不许压进水印带 */
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    if (wx + ww > srcW) wx = srcW - ww;
    if (wy + wh > srcH) wy = srcH - wh;
    ESP_LOGW(TAG, "取景：白框左%d右%d → 内容 x%d..%d（宽%d）→ 窗口 %d,%d %dx%d（原图 %dx%d）",
             x0, srcW - 1 - x1, x0, x1, cw, wx, wy, ww, wh, srcW, srcH);
    memset(out2268, 0, POST_BYTES);

    /* ★R159：先落一张 164×108 灰度图，再「拉伸 + 自适应提亮」，最后才抖动。
       用户实机报「整张明信片和猫都反白了」= 满屏网点；真因是免费源出的图对比度很窄
       （实测 p3=21 / 中位=205 / p97=227），Bayer 抖动把整片天空抖成 50% 网点。
       ★R163：提亮改为**按图自适应** —— 以前写死 gamma 1.3，换个提示词/换台模型就失配；
       现在先按 2%/98% 分位拉伸，再用拉伸后的平均亮度反推 gamma，把纸面推到 ~74%，
       暗图多提亮、亮图不洗白。用户担心「换成别的图会不会出问题」就是靠这条兜。 */
    uint8_t *grey = heap_caps_malloc(POST_W * POST_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!grey) grey = malloc(POST_W * POST_H);
    if (!grey) { free(rgb); ESP_LOGW(TAG, "灰度缓冲分配失败"); return false; }
    post_sample(rgb, srcW, wx, wy, ww, wh, grey);
    post_quantize(grey, out2268);

    free(rgb);
    free(grey);
    return true;
}

/* ── ★R158：把猫精灵贴到 AI 风景上 ─────────────────────────────────────────
   坐标口径见 cat_sprites.h：精灵图是 200×200（与明信片页同尺寸），照片区
   x18..181 / y46..153 正好是 164×108 = photo buffer，故 (ox,oy) 直接可用，
   **不缩放、不位移**（用户 2026-11-09 原话：「你不用对它进行任何的处理，只要把猫
   合成到现有的明信片体系里面去，它这个其实是 200×200，像素都是对应的」）。

   ★R164/R165 猫出现在明信片上的频率 = **按亲密度分档**（用户 2026-11-09 口径）：
     · 亲密度低 → 基本只有纯风景（不放猫），背影还有一点，正面/全身/特写**完全不出**；
     · 亲密度高 → 纯风景仍是多数，全身多一点点，特写**偶尔**来一张。
   R165 订正：R164 最高档「有猫」66%（全身 38 + 背影 16 + 特写 12）用户嫌太高
   （「100 的情况下有猫控制在 40% 左右」），且低档背影 30% 也偏高（「20% 或 15%」）。
   档位权重（合计 100 / 有猫合计=后三列之和）：

       亲密度     纯风景   背影   全身   特写    有猫
       20–39        82     18      0      0     18%   ← 刚认识：几乎只有风景
       40–69        70     16     14      0     30%
       70–84        64     15     17      4     36%
       85–100       60     14     18      8     40%   ← 很黏人：也只有四成带猫

   姿态归类按新一批 13 张的实际内容（2026-11-12 用户重画替换，旧的 5 张全部作废）：
   #1~#4 坐/趴/站立全身 · #5~#6 背对镜头（尾巴朝前）· #7 坐姿大眼 ·
   #8 戴耳机半身 · #9 舔爪 · #10 坐姿 · #11 带问号 · #12 仰面举爪 · #13 贴脸特写。 */
#define CAT_T_LAND  0     /* 纯风景（不放猫） */
#define CAT_T_BACK  1     /* 背影 */
#define CAT_T_FULL  2     /* 全身正面 */
#define CAT_T_FACE  3     /* 特写 */
/* 下标与 CAT_SPRITES 一一对应（顺序 = 生成脚本按文件名排的 1..13） */
static const uint8_t CAT_TIER[] = {
    /* #1~#4 */  CAT_T_FULL, CAT_T_FULL, CAT_T_FULL, CAT_T_FULL,
    /* #5~#6 */  CAT_T_BACK, CAT_T_BACK,
    /* #7    */  CAT_T_FULL,
    /* #8    */  CAT_T_FACE,          /* 戴耳机，半身 */
    /* #9~#11*/  CAT_T_FULL, CAT_T_FULL, CAT_T_FULL,
    /* #12~#13*/ CAT_T_FACE, CAT_T_FACE,
};
_Static_assert(sizeof(CAT_TIER) / sizeof(CAT_TIER[0]) == N_CAT_SPRITES,
               "CAT_TIER 项数必须等于精灵数 —— 换图后要同步改分类（见 gen_cat_sprites.py 的 TIERS）");

static int ai_intimacy(void)
{
    int32_t v = 60;                                  /* 与 ws_load_state 的出厂默认一致 */
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
        int32_t t = 0;
        if (nvs_get_i32(h, "st_inti", &t) == ESP_OK) v = t;   /* ★R164：互动会双写 st_inti/inti */
        nvs_close(h);
    }
    if (v < 20) v = 20;
    if (v > 100) v = 100;
    return (int)v;
}

/* ★R189：亲密度档位 → 四类权重（风景/背影/全身/特写）。用户 2026-11-11 反馈
   「猫的精灵图几乎每张信件都有，我记得以前不高」→ 整体下调「有猫」的总概率
   （原 18% / 30% / 36% / 40% → 8% / 16% / 22% / 28%）。 */
static void cat_weights(int im, int w[4], const char **band)
{
    if (im < 40)      { w[0] = 92; w[1] =  8; w[2] =  0; w[3] = 0; *band = "低"; }
    else if (im < 70) { w[0] = 84; w[1] = 10; w[2] =  6; w[3] = 0; *band = "中"; }
    else if (im < 85) { w[0] = 78; w[1] = 11; w[2] =  8; w[3] = 3; *band = "高"; }
    else              { w[0] = 72; w[1] = 12; w[2] = 11; w[3] = 5; *band = "很高"; }
}

/* 从 0..99 的一次抽签落到档位（CAT_T_*） */
static int cat_tier_roll(const int w[4], uint32_t r)
{
    r %= 100u;
    if (r < (uint32_t)w[0]) return CAT_T_LAND;
    r -= (uint32_t)w[0];
    if (r < (uint32_t)w[1]) return CAT_T_BACK;
    r -= (uint32_t)w[1];
    if (r < (uint32_t)w[2]) return CAT_T_FULL;
    return CAT_T_FACE;
}

static int cat_cand(int tier, int *out)
{
    int n = 0;
    for (int i = 0; i < N_CAT_SPRITES; ++i)
        if (CAT_TIER[i] == tier) out[n++] = i;
    return n;
}

static const char *cat_tier_name(int tier)
{
    return tier == CAT_T_BACK ? "背影" : (tier == CAT_T_FULL ? "全身" : "特写");
}

/* 把某个姿态叠到 164×108 的 1-bit 缓冲区上。
   one_is_ink = true  → 缓冲区约定「1=墨」（AI 生图通道，落箱前再统一取反）
   one_is_ink = false → 缓冲区约定「1=纸白」（assets/AID_POST 与明信片槽，与出图侧同口径） */
static void cat_blit(uint8_t *photo, int idx, bool one_is_ink)
{
    const cat_sprite_t *s = &CAT_SPRITES[idx];
    for (int y = 0; y < s->h; ++y) {
        const int by = s->oy + y;
        if (by >= POST_H) break;                       /* 猫脚可能压到框线，越界就停 */
        const int srow = y * s->row;
        uint8_t *dst = photo + (size_t)by * POST_ROW;
        for (int x = 0; x < s->w; ++x) {
            const int bx = s->ox + x;
            if (bx >= POST_W) continue;
            const int si = srow + (x >> 3);
            const uint8_t sb = (uint8_t)(0x80u >> (x & 7));
            if (!(s->mask[si] & sb)) continue;          /* 透明：保留风景 */
            const uint8_t db = (uint8_t)(0x80u >> (bx & 7));
            const bool ink = (s->ink[si] & sb) != 0;    /* 猫体=墨；眼睛/胸白点=纸 */
            if (ink == one_is_ink) dst[bx >> 3] |= db;
            else                   dst[bx >> 3] &= (uint8_t)~db;
        }
    }
}

/* AI 生图通道：每张新图掷一次骰子（图是刚生成的，不存在"同一张卡变来变去"） */
static void cat_overlay(uint8_t *photo)
{
    if (N_CAT_SPRITES <= 0) return;
    const int im = ai_intimacy();
    int w[4];
    const char *band;
    cat_weights(im, w, &band);
    const int tier = cat_tier_roll(w, esp_random());
    int cand[N_CAT_SPRITES];
    const int nc = cat_cand(tier, cand);
    if (tier == CAT_T_LAND || nc == 0) {            /* 纯风景：不叠猫 */
        ESP_LOGW(TAG, "猫合成：亲密度 %d（%s档 风景%d/背%d/全身%d/特写%d）→ 纯风景·不放猫",
                 im, band, w[0], w[1], w[2], w[3]);
        return;
    }
    const int idx = cand[(int)(esp_random() % (uint32_t)nc)];
    ESP_LOGW(TAG, "猫合成：亲密度 %d（%s档 风景%d/背%d/全身%d/特写%d）→ %s CAT%d",
             im, band, w[0], w[1], w[2], w[3], cat_tier_name(tier), idx + 1);
    cat_blit(photo, idx, true);
}

/* ★R166：**预制明信片也叠猫** —— 用户口径「不是只有 AI 图才有猫，现有的预制片也要加，
   概率规则跟 AI 那套差不多」。

   与 AI 片的唯一区别是**定死**：每张预制卡用卡片自己的 photo 号当种子做一次确定性散列，
   同一张卡在列表 / 详情 / 转发里永远是同一个结果（拿 esp_random 会让同一张卡每次翻页
   变一个样，看着像故障）。亲密度换档时这套判定会整体平移 —— 关系变了，猫的出场也跟着变。
   返回 CAT_SPRITES 下标；-1 = 纯风景（不叠）。 */
int post_sprite_pick(uint32_t seed)
{
    if (N_CAT_SPRITES <= 0) return -1;
    const int im = ai_intimacy();
    int w[4];
    const char *band;
    cat_weights(im, w, &band);
    uint32_t h = seed * 2654435761u + (uint32_t)im * 0x9E3779B9u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;     /* 够用的小散列，无需真随机 */
    const int tier = cat_tier_roll(w, h);
    int cand[N_CAT_SPRITES];
    const int nc = cat_cand(tier, cand);
    if (tier == CAT_T_LAND || nc == 0) return -1;
    h ^= h >> 16; h *= 2246822519u;
    return cand[h % (uint32_t)nc];
}

void post_sprite_draw(uint8_t *photo, int idx)
{
    if (!photo || idx < 0 || idx >= N_CAT_SPRITES) return;
    /* AID_POST 的 1-bit 约定是 **1=墨**（实测：`post.bin` 第 0 张按 1=黑 解码，
       天空是稀疏点、建筑是实心块；反过来解就是底片）—— 与 fb_blit1 的语义一致，
       所以这里直接按「1=墨」画，不需要取反。mypost 槽才是 1=白（那边绘制端自己取反）。 */
    cat_blit(photo, idx, true);
}

/* ── 主流程：提交 → 轮询 → 下载 → 解码 → 落箱 ─────────────────── */
/* ★R157 失败熔断（图片通道）：连续 3 次生成失败 → 本次开机不再试，静默回落。
   普通 static ⇒ 开机/深睡唤醒自动清零；与文字通道分开计数（互不拖累）。 */
#define API_IMG_FUSE_MAX 3
static int s_api_fail_img = 0;
/* ★R159：402 Payment Required / 429 Too Many Requests = **限流**，不是通道坏了。
   免费档按公网 IP 限流（同时 1 个、间隔 ≥45s），国内大量 CGNAT 出口共用同一 IP，
   邻居同时出图就会撞上。这类失败一律不计熔断，否则点三次就把本次开机废掉。 */
static bool api_img_limited(int code) { return code == 402 || code == 429; }
static bool api_img_fail(void)
{
    if (s_api_fail_img < API_IMG_FUSE_MAX) ++s_api_fail_img;
    ESP_LOGW(TAG, "图像通道失败计数 %d/%d", s_api_fail_img, API_IMG_FUSE_MAX);
    return false;
}

bool api_image_gen(const char *prompt, const char *text, const char *title, const char *place)
{
    if (s_api_fail_img >= API_IMG_FUSE_MAX) {
        ESP_LOGW(TAG, "图像通道已熔断 → 本次开机不再试");
        return false;
    }
    if (!s_sink) return api_img_fail();
    /* ★R119 修：三个缓冲必须清零 —— ai_keys() 在 NVS 读失败/键缺失时**不写**，
       原未初始化栈值会被 key[0] 当"已配置"，并把栈垃圾当 Bearer token / URL 发出去。 */
    char key[160] = {0}, base[160] = {0}, model[64] = {0};
    ai_keys(key, sizeof(key), base, sizeof(base), model, sizeof(model));
    if (!key[0]) { ESP_LOGW(TAG, "图像 key 未配置 → 跳过"); return api_img_fail(); }
    char auth[200];
    snprintf(auth, sizeof(auth), "Bearer %s", key);

    /* ① 按服务商提交 / 直接拿图 URL（★R157 多服务商） */
    char body[512], url[256], resp[1024], img_url[768] = {0};
    const ai_prov_t prov = ai_provider(base);
    int code = 0;
    ESP_LOGW(TAG, "图像服务商 = %s（%s）", ai_prov_name(prov), base);

    if (prov == AI_P_URL_GET) {
        /* Pollinations 等：GET 一个 prompt URL 直接回 JPEG，零提交、零轮询。
           ★R159：**必须带随机 seed**。官方默认 seed=42，且「同 prompt + 同 seed 返回同一张」
           （结果还会按 URL 缓存，谁拿到 URL 都同一张）—— 不带 seed 的话，同一城市的用户
           每次出图都是像素级相同的一张，等于只有一张明信片。 */
        char enc[512]; ai_url_enc(prompt, enc, sizeof(enc));
        snprintf(img_url, sizeof(img_url),
                 "%s/prompt/%s?width=256&height=256&nologo=true&seed=%u",
                 base, enc, (unsigned)(esp_random() % 1000000000u));
    } else if (prov == AI_P_OPENAI_IMG) {
        /* 智谱 cogview-3-flash / 任意 OpenAI 风格：一次 POST 同步拿 data[0].url */
        if (strstr(base, "/images/generations")) snprintf(url, sizeof(url), "%s", base);
        else snprintf(url, sizeof(url), "%s/images/generations", base);
        /* ★2026-11-12：截断长度跟着 prompt 一起放宽 —— 风格词加长后（约 140 字节）再用
           240 会把 scene 削掉一半，画面描述不全，出的图更认不出。body[512] 容得下
           360 + model + 固定 JSON 骨架（约 40 字节），snprintf 本身还有 sizeof(body) 兜底。 */
        snprintf(body, sizeof(body),
                 "{\"model\":\"%s\",\"prompt\":\"%.360s\",\"size\":\"1024x1024\",\"n\":1}", model, prompt);
        if (!http_post_json(url, auth, body, resp, sizeof(resp), false, &code)) {
            ESP_LOGW(TAG, "同步生图提交失败 http=%d len=%d resp=%.160s", code, (int)strlen(resp), resp);
            if (api_img_limited(code)) return false;        /* 限流不计熔断 */
            return api_img_fail();
        }
        cJSON *r = cJSON_Parse(resp);
        const cJSON *d  = r ? cJSON_GetObjectItem(r, "data") : NULL;
        const cJSON *it = d ? cJSON_GetArrayItem(d, 0) : NULL;
        const cJSON *u  = it ? cJSON_GetObjectItem(it, "url") : NULL;
        if (cJSON_IsString(u) && u->valuestring[0])
            snprintf(img_url, sizeof(img_url), "%s", u->valuestring);
        if (r) cJSON_Delete(r);
        if (!img_url[0]) {
            /* ★2026-11-12 诊断：实机「等了 9 秒 → 同步生图无 url」，但 resp 打印出来是空的，
               看不出是 401 / 模型不存在 / 参数错。这里把 http 码与响应长度一并打全
               （响应体也印，最长 160 字节 —— 智谱的错误体是 {"error":{"code":...,"message":...}}）。 */
            ESP_LOGW(TAG, "同步生图无 url：http=%d len=%d resp=%.160s",
                     code, (int)strlen(resp), resp);
            return api_img_fail();
        }
    } else {
        /* 阿里云百炼异步：提交 → 轮询（原逻辑） */
        snprintf(body, sizeof(body),
                 "{\"model\":\"%s\",\"input\":{\"prompt\":\"%.200s\"},"
                 "\"parameters\":{\"size\":\"1280*720\",\"n\":1}}", model, prompt);
        snprintf(url, sizeof(url), "%s/api/v1/services/aigc/text2image/image-synthesis", base);
        if (!http_post_json(url, auth, body, resp, sizeof(resp), true, &code)) {
            ESP_LOGW(TAG, "生图任务提交失败 http=%d", code);
            if (api_img_limited(code)) return false;        /* 限流不计熔断 */
            return api_img_fail();
        }
        cJSON *r = cJSON_Parse(resp);
        char task_id[64] = {0};
        if (r) {
            const cJSON *o = cJSON_GetObjectItem(r, "output");
            const cJSON *t = o ? cJSON_GetObjectItem(o, "task_id") : NULL;
            if (cJSON_IsString(t) && t->valuestring[0]) snprintf(task_id, sizeof(task_id), "%s", t->valuestring);
            cJSON_Delete(r);
        }
        if (!task_id[0]) { ESP_LOGW(TAG, "没有 task_id：%.*s", 200, resp); return api_img_fail(); }
        ESP_LOGW(TAG, "生图任务已提交：%s", task_id);

        /* ② 轮询结果 URL */
        snprintf(url, sizeof(url), "%s/api/v1/tasks/%s", base, task_id);
        for (int i = 0; i < AI_POLL_TIMES; ++i) {
            vTaskDelay(pdMS_TO_TICKS(3000));               /* 顺带喂狗 */
            if (!http_get(url, auth, resp, sizeof(resp), &code)) continue;
            r = cJSON_Parse(resp);
            if (!r) continue;
            const cJSON *o = cJSON_GetObjectItem(r, "output");
            const cJSON *st = o ? cJSON_GetObjectItem(o, "task_status") : NULL;
            const char *status = cJSON_IsString(st) ? st->valuestring : "";
            if (strcmp(status, "SUCCEEDED") == 0) {
                const cJSON *res = cJSON_GetObjectItem(o, "results");
                const cJSON *ri = res ? cJSON_GetArrayItem(res, 0) : NULL;
                const cJSON *u = ri ? cJSON_GetObjectItem(ri, "url") : NULL;
                if (cJSON_IsString(u) && u->valuestring[0]) snprintf(img_url, sizeof(img_url), "%s", u->valuestring);
                cJSON_Delete(r);
                break;
            }
            if (strcmp(status, "FAILED") == 0 || strcmp(status, "CANCELED") == 0 ||
                strcmp(status, "UNKNOWN") == 0) {
                ESP_LOGW(TAG, "生图任务失败：%s", status);
                cJSON_Delete(r);
                return api_img_fail();
            }
            cJSON_Delete(r);
        }
        if (!img_url[0]) { ESP_LOGW(TAG, "轮询超时（%ds）", AI_POLL_TIMES * 3); return api_img_fail(); }
    }

    /* ③ 下载
       ★R158：免费图源（pollinations Classic）限流很凶，忙时会连续回 402/503
       （实测本机 curl 三次里两次被拒）。原来一次失败就记一次熔断 ⇒ 用户点三下就废。
       这里对**直出图**这条最便宜的路径重试 2 次、间隔 8 秒；用户明确说过
       「明信片没有即时性，阻塞 30~108 秒都没关系」。 */
    size_t jlen = 0;
    uint8_t *jpg = NULL;
    int hcode = 0, wait_s = 0;
    /* ★R160：实测抓到的失败序列（COM7 R159）：
       `http=503` → 代码走「8 秒后重试」→ `http=402 = 限流`。
       而免费档的硬规则就是**两次请求间隔 ≥45 秒**，第 8 秒重试等于必然再撞 402，
       把本来可能成功的重试白扔掉。⇒ 免费 GET 源一律按 45 秒节奏重试，503（服务端饱和）
       与 402/429（限流）同样当「忙」，都不计熔断。 */
    const bool free_src = (prov == AI_P_URL_GET);
    /* ★2026-11-12：智谱图床（UCloud）首次建连会失败一次（实测 err=0x7002），
       而原写法非 GET 源 dl_try=1 ⇒ 一次失败直接判死。所有通道至少给一次重试。 */
    const int dl_try = free_src ? 3 : 2;
    for (int i = 0; i < dl_try; ++i) {
        jpg = http_get_bin(img_url, auth, &jlen, &hcode, &wait_s);
        if (jpg) break;
        const bool rl   = (hcode == 402 || hcode == 429);
        const bool busy = (hcode == 503 || hcode == 502 || hcode == 504);
        if (free_src && (rl || busy)) {
            if (i + 1 >= dl_try) {
                ESP_LOGW(TAG, "图源仍忙（http=%d）→ 本次放弃（**不计熔断**，过会儿再点即可）", hcode);
                return false;
            }
            int w = (rl && wait_s > 0) ? wait_s : 45;       /* 官方匿名档间隔就是 45s */
            if (w < 45) w = 45;
            ESP_LOGW(TAG, "图源忙 http=%d（第 %d/%d 次）→ 等 %d 秒重试（不计熔断）",
                     hcode, i + 1, dl_try, w);
            vTaskDelay(pdMS_TO_TICKS(w * 1000));
            continue;
        }
        if (i + 1 < dl_try) {
            ESP_LOGW(TAG, "图片下载失败 http=%d（第 %d/%d 次），8 秒后重试", hcode, i + 1, dl_try);
            vTaskDelay(pdMS_TO_TICKS(8000));
        }
    }
    if (!jpg) { ESP_LOGW(TAG, "图片下载失败（共尝试 %d 次，最后 http=%d）", dl_try, hcode); return api_img_fail(); }
    ESP_LOGW(TAG, "图片已下载 %uB", (unsigned)jlen);

    /* ④ 解码 + 抖动 1-bit */
    static EXT_RAM_BSS_ATTR uint8_t photo[POST_BYTES];
    if (!jpg_to_post_1bit(jpg, jlen, photo)) { free(jpg); return api_img_fail(); }
    free(jpg);
    cat_overlay(photo);                  /* ★R158：风景 + 猫精灵合成 */
    /* ★R161：**槽里的 1-bit 约定是 1=白，不是 1=墨**。
       绘制端（main.c draw_mypost_item_page）注释写明：手机端 pack 时 `if(!ink) bit=1`，
       所以读出来要逐字节取反再 `fb_blit1`（它才是 1=墨）。本文件用 1=墨 做内部表示更直观，
       但**落箱前必须取反**，否则 AI 明信片在屏上整张黑白颠倒（用户实机报「图的黑白是反的」）。
       样例图（预设片）走同一绘制端，本来就存 1=白，所以只需在这里对齐一次。 */
    for (int i = 0; i < POST_BYTES; ++i) photo[i] = (uint8_t)~photo[i];

    /* ⑤ 字段块 + 落箱（0xFF 格式，与好友转发同一 sink → mypost_add） */
    uint8_t rest[256];
    int rl = 0;
    /* ★R119 修：标题上限必须与 mypost_add 的校验口径一致（MYPOST_TITLE_MAX=30，
       原写 36 —— 超 30 的标题会让整张卡被 mypost_add 判非法丢弃）。 */
    /* ★R170 全检：按字节夹上限可能把最后一个 3 字节汉字切成半截 → 上屏豆腐块。
       夹完若「被丢掉的第一字节」是 UTF-8 续字节，就一路退到字符起点。 */
    int tl = (int)strlen(text);  if (tl > AI_POST_TEXT_MAX)  tl = AI_POST_TEXT_MAX;
    int tal = (int)strlen(title); if (tal > AI_POST_TITLE_MAX) tal = AI_POST_TITLE_MAX;
    int aal = (int)strlen(place); if (aal > AI_POST_ADDR_MAX)  aal = AI_POST_ADDR_MAX;
    while (tl  > 0 && ((unsigned char)text[tl]  & 0xC0) == 0x80) --tl;
    while (tal > 0 && ((unsigned char)title[tal] & 0xC0) == 0x80) --tal;
    while (aal > 0 && ((unsigned char)place[aal] & 0xC0) == 0x80) --aal;
    rest[rl++] = (uint8_t)tl;  memcpy(rest + rl, text,  (size_t)tl);  rl += tl;
    rest[rl++] = (uint8_t)tal; memcpy(rest + rl, title, (size_t)tal); rl += tal;
    rest[rl++] = (uint8_t)aal; memcpy(rest + rl, place, (size_t)aal); rl += aal;
    const int day = (int)(time(NULL) / 86400);
    mypost_src_hint(1);                  /* ★R189：标记来源 = AI生成（背面左上不再写「手机相册」）*/
    const int k = s_sink(photo, rest, rl, day);
    if (k < 0) { ESP_LOGW(TAG, "落箱失败（信箱满/格式错）"); return api_img_fail(); }
    ESP_LOGW(TAG, "★ AI 静默明信片已入箱（槽 %d）", k);
    s_api_fail_img = 0;                  /* ★R157：成功即清零，熔断按「连续失败」计 */
    return true;
}
