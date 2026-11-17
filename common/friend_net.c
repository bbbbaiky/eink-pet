/* friend_net.c — 好友跨设备链路 v1（免注册公共 MQTT）。设计口径见 friend_net.h 头注释。 */
#include <esp_attr.h>       /* ★R198：EXT_RAM_BSS_ATTR（BSS 外置 PSRAM） */
#include "friend_net.h"
#include "friend.h"
#include "net_sync.h"          /* ★R121：复用 VISIT_MAX_H（串门兜底时长） */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "frnet";

/* 免注册公共 broker（EMQX）：国内接入点为主；连不上可在下面换 broker.emqx.io（海外）。
   （巴法云 bemfa.com:9502 要注册私钥 —— 用户明确要免注册，不用。） */
#define FRNET_BROKER          "broker-cn.emqx.io"
#define FRNET_BROKER_FALLBACK "broker.emqx.io"
#define FRNET_PORT    1883
#define FRNET_PREFIX  "petcat/v1"
#define FRNET_WAIT_CONN_MS   3000    /* ★R156：6s→3s（同步收尾不该压用户几十秒） */
#define FRNET_SEND_WAIT_MS  12000    /* ★2026-10-26：转发发送给更宽的等待窗口（用户手点，值得多等） */
#define FRNET_COLLECT_MS     4000    /* ★R218：1.5s→4s（公共 broker 的 retained 会晚 2~3s，1.5s 常漏） */
#define FRNET_STATE_MAX      20      /* 好友数上限（与 FRIEND_MAX 一致，防 NVS 键失控） */
/* ★★2026-10-27 明信片收不到的第三个根因（实机）：
   esp-mqtt 的收发缓冲默认只有 1024 B，而一张转发明信片的 payload ≈ 2.7 KB
   （`p1|<day>|` + 2268 B 图 + 字段块）⇒ ① 发送 publish 直接超缓冲；
   ② 即使挂上 broker，接收侧也会被拆成多段 DATA 事件、`frnet_store_post` 每段都判「图不完整」。
   ⇒ 所有短连接客户端统一把 buffer 抬到 4 KB（够 2.7 KB + MQTT 头）。 */
#define FRNET_BUF_SZ       4096

/* 待发布的状态（main.c 每轮同步前塞进来） */
static int  s_mood = 0, s_intim = 0, s_day = 0, s_vis = 0;
static time_t s_now_local = 0;              /* ★R121：本地伪 epoch（与 visit_t0 同一套钟） */
static char s_name[24] = {0};               /* ★v3：自己的猫名（UTF-8，≤23B） */
static volatile bool s_connected  = false;
static volatile bool s_conn_err    = false;      /* ★R156：MQTT ERROR → 不再干等满 WAIT_CONN_MS */
static volatile int  s_got_new    = 0;      /* 本轮收到的新状态条数（返回值用） */
static volatile int  s_got_msg    = 0;      /* 本轮收到的留言条数 */
/* ★R121：串门信号收件 → 只置「主页待重绘」标志（后台任务绝不碰 UI，由主循环取走）。 */
static volatile bool s_home_dirty = false;
static unsigned      s_tx_seq     = 0;      /* ★2026-10-27：短连接发送用的唯一 client_id 序号
                                               （与后台同步共用同一 id 会被 broker 互相踢下线） */

/* 码 → 好友表下标（不在表中 = -1） */
static int frnet_find_friend(const char *code)
{
    const int n_fr = friend_count();
    for (int i = 0; i < n_fr && i < FRNET_STATE_MAX; ++i) {
        const friend_info_t *f = friend_at(i);
        if (f && strcmp(f->code, code) == 0) return i;
    }
    return -1;
}

static void frnet_store_visit(esp_mqtt_client_handle_t cli, const char *code,
                              const char *payload, int len);

void friend_net_set_self(int mood, int intim, int day, int vis, time_t now_local)
{
    s_mood = mood; s_intim = intim; s_day = day;
    s_vis  = (vis < 0 || vis > 2) ? 0 : vis;
    if (now_local > 0) s_now_local = now_local;   /* 0 = 调用方没有钟，保留上一次 */
}

/* ★R128：主页渲染前调一次 —— 保证 fn<i>_vend 的到期判断永远用**当前**钟，
   不依赖「本轮是否同步过」（深睡唤醒后 s_now_local 否则为 0）。 */
void friend_net_set_now(time_t now_local)
{
    if (now_local > 0) s_now_local = now_local;
}

/* ★R212 修（全量审计）：按 UTF-8 边界回退 —— 名字按字节截断时不许留下半个汉字。
   对端会用这个名字上屏，半个汉字会让 friend_ui 的步进函数越界/乱码。 */
static void frnet_u8_trim(char *s)
{
    size_t n = strlen(s);
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) --k;   /* 回到首字节 */
    if (k > 0) {
        const unsigned lead = (unsigned char)s[k - 1];
        const size_t need = (lead < 0x80) ? 1u
                          : ((lead & 0xE0) == 0xC0) ? 2u
                          : ((lead & 0xF0) == 0xE0) ? 3u : 4u;
        if (n - (k - 1) < need) s[k - 1] = 0;    /* 尾巴不完整 → 截掉这一字符 */
    }
}

/* ★2026-10-27 v3：自己的猫名 —— 供好友在串门告示牌上显示。
   名字里的 '|' 是 payload 分隔符，必须剔掉；只留 UTF-8 字节，长度 ≤23B。 */
void friend_net_set_self_name(const char *name)
{
    if (!name) { s_name[0] = 0; return; }
    int n = 0;
    for (const char *p = name; *p && n < (int)sizeof(s_name) - 1; ++p)
        if (*p != '|') s_name[n++] = *p;
    s_name[n] = 0;
    frnet_u8_trim(s_name);                    /* ★R212：别切半个汉字 */
}

static void frnet_store_state(const char *code, const char *payload, int len)
{
    /* payload v1 = `v1|<mood>|<intim>|<day>`；v2 末尾多 vis；v3 再末尾多 pet_name。
       逐段 strtol（段缺失/非数字 = 丢弃本条）；旧固件发的 v1/v2 继续收。 */
    if (!code || !payload) return;
    if (strncmp(payload, "v1|", 3) != 0 && strncmp(payload, "v2|", 3) != 0 &&
        strncmp(payload, "v3|", 3) != 0) return;
    char buf[96];
    int n = (len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
    memcpy(buf, payload, (size_t)n);
    buf[n] = 0;
    const char *p = buf + 3;
    char *end = NULL;
    const long mood  = strtol(p, &end, 10);
    if (end == p || *end != '|') return;
    p = end + 1;
    const long intim = strtol(p, &end, 10);
    if (end == p || *end != '|') return;
    p = end + 1;
    long day  = strtol(p, &end, 10);
    if (end == p) return;
    long vis = 0;                            /* ★v2 = ...|day|vis；v1 无 vis 段 = 0 在家 */
    if (*end == '|') {
        p = end + 1;
        vis = strtol(p, &end, 10);
        if (end == p) vis = 0;
    }
    char nm[24] = {0};                       /* ★v3 = ...|vis|name（无该段 = 空） */
    if (*end == '|') {
        p = end + 1;
        if (p < buf + n) {
            strncpy(nm, p, sizeof(nm) - 1);
            nm[sizeof(nm) - 1] = 0;
            frnet_u8_trim(nm);                /* ★R212：入站名字也不许半个汉字 */
        }
    }
    (void)intim;                             /* 亲密度先收下不用 */

    const int idx = frnet_find_friend(code);
    if (idx < 0) { ESP_LOGW(TAG, "收到非好友 %s 的状态，忽略", code); return; }
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) == ESP_OK) {
        char k[16];
        snprintf(k, sizeof(k), "fn%d_md", idx);  nvs_set_i32(h, k, (int32_t)mood);
        snprintf(k, sizeof(k), "fn%d_dy", idx);  nvs_set_i32(h, k, (int32_t)day);
        /* ★R121（用户令：串门按真实窗口持续，不再只有 3 秒）：
           收件侧可能已由 /visit 信号先写了「vis=1 + fn<i>_vend 到期时刻」。
             · 对方亲口报 vis=1 → 以 state 为准，清掉到期时刻（收尾仍交给后续 state）；
             · 对方报 0/2 但信号的到期时刻还没到 → **保留串门**，别让 30 分钟一次的
               同步把刚来的串门猫抹掉；
             · 到期已过 → 照原样写 0/2，并顺手清掉到期键。 */
        int32_t vend = 0;
        snprintf(k, sizeof(k), "fn%d_vend", idx); nvs_get_i32(h, k, &vend);
        if (vis == 1) {
            nvs_set_i32(h, k, 0);                       /* 清 vend（k 此刻仍是 vend 键） */
            snprintf(k, sizeof(k), "fn%d_vis", idx); nvs_set_i32(h, k, 1);
        } else if (vend > 0 && (int32_t)s_now_local < vend) {
            /* 串门信号仍在有效期内 → 保留 1，vend 不动 */
        } else {
            if (vend > 0) nvs_set_i32(h, k, 0);
            snprintf(k, sizeof(k), "fn%d_vis", idx); nvs_set_i32(h, k, (int32_t)vis);
        }
        snprintf(k, sizeof(k), "fn%d_nm", idx);
        if (nm[0]) nvs_set_str(h, k, nm);      /* 没带名字（旧固件）→ 保留上一次的 */
        nvs_commit(h);
        nvs_close(h);
    }
    s_got_new++;
    ESP_LOGW(TAG, "★ 好友 %s 状态：M%ld · 最近在线日序 %ld", code, mood, day);
}

/* ★2026-10-24 夜：好友留言 v1 —— topic `petcat/v1/<码>/msg`（retained = 信箱模式）。
   payload = `m1|<日序>|<文本>`（文本 UTF-8 ≤60B ≈20 汉字）。
   收到 → 存 NVS（fn<i>_msg/_msgdy/_mr=1 未读）→ **立刻回发空 payload 清掉 retained**，
   下轮同步不会再收到同一条（收到即取件）。 */
static void frnet_store_msg(esp_mqtt_client_handle_t cli, const char *code,
                            const char *payload, int len)
{
    if (!code || !payload || strncmp(payload, "m1|", 3) != 0) return;
    char buf[96];
    int n = (len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
    memcpy(buf, payload, (size_t)n);
    buf[n] = 0;
    const char *p = buf + 3;
    char *end = NULL;
    const long day = strtol(p, &end, 10);
    if (end == p || *end != '|' || end - p > 6) return;
    p = end + 1;
    if (!p[0]) return;                       /* 空文本不存 */
    const int idx = frnet_find_friend(code);
    if (idx < 0) { ESP_LOGW(TAG, "收到非好友 %s 的留言，忽略", code); return; }
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) == ESP_OK) {
        char k[16];
        snprintf(k, sizeof(k), "fn%d_msg",   idx); nvs_set_str(h, k, p);
        snprintf(k, sizeof(k), "fn%d_msgdy", idx); nvs_set_i32(h, k, (int32_t)day);
        snprintf(k, sizeof(k), "fn%d_mr",    idx); nvs_set_i32(h, k, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    s_got_msg++;
    ESP_LOGW(TAG, "★ 好友 %s 留言（日序 %ld）：%s", code, day, p);
    /* 取件后清 retained（发布空 payload）—— 信箱清空 */
    char top[64];
    snprintf(top, sizeof(top), "%s/%s/msg", FRNET_PREFIX, code);
    esp_mqtt_client_publish(cli, top, "", 0, 1, 1);
}

/* ★2026-10-24 夜：明信片转发取件 —— payload = `p1|<day>|` + photo2268 + mypost 字段块。
   交给 main.c 注册的回调装进 mypost_add（标题 = 转发者昵称），然后清信箱。 */
static friend_net_post_fn s_post_cb = NULL;
void friend_net_set_post_cb(friend_net_post_fn fn) { s_post_cb = fn; }

/* ★★★R218（2026-11-17 用户报）：**私信被"抢单"** —— 原来 send_post 把信挂到
   *发件人自己* 的码下（`<my>/post`），谁先同步谁取走并清箱，所以 A 发给 B 的信
   可能被同样加了 A 的 C 取走；而 B 若没加 A，则永远不订阅 A 的 topic ⇒ 收不到。
   修法 = 定向投递：发件人把信投到 **收件人自己的信箱** `PREFIX/<收件人码>/inbox`；
   每台设备同步时订阅**自己的 inbox**，取走即清。
   载荷格式：`p2|<发件人码>|<发件人猫名>|<日序>|` + 2268B 位图 + 文本块
   （topic 是收件人自己，所以发件人身份只能放在载荷里 —— 这就是 p1→p2 的原因）。
   ★ 兼容：仍保留订阅好友的 `/post`（老固件还在往那儿发，v1 处理不变）；
     新固件不再往自己 `/post` 发，因此**老固件收不到新固件发的信**（可接受）。 */
static void frnet_store_inbox(esp_mqtt_client_handle_t cli,
                              const char *payload, int len)
{
    if (!payload || len < 8 || strncmp(payload, "p2|", 3) != 0) return;
    const char *q = payload + 3;
    const char *e1 = memchr(q, '|', (size_t)(payload + len - q));
    if (!e1) return;
    const int clen = (int)(e1 - q);
    if (clen != 6) return;                                  /* 发件人码必须是 6 位 */
    char code[8] = {0};
    memcpy(code, q, (size_t)clen);
    const char *q2 = e1 + 1;
    const char *e2 = memchr(q2, '|', (size_t)(payload + len - q2));
    if (!e2) return;
    int nlen = (int)(e2 - q2);
    if (nlen > 23) nlen = 23;
    char name[24] = {0};
    if (nlen > 0) memcpy(name, q2, (size_t)nlen);
    frnet_u8_trim(name);        /* ★R218 复检：硬截 23 字节会切半个汉字 → 按 UTF-8 边界回退 */
    /* ★R218：名字口径 —— 对方在**我的好友表**里就用我给他起的名字（与老路径一致，
       且不会出现对方没取名时的「-」）；不是好友时才用载荷自带的自称。 */
    {
        const int fi = frnet_find_friend(code);
        if (fi >= 0) {
            const friend_info_t *ff2 = friend_at(fi);
            if (ff2 && ff2->name[0]) {
                memset(name, 0, sizeof(name));
                const int l = (int)strnlen(ff2->name, sizeof(ff2->name));
                memcpy(name, ff2->name, (size_t)(l > 23 ? 23 : l));
                frnet_u8_trim(name);                    /* ★同上：别切半个汉字 */
                nlen = (int)strlen(name);
            }
        } else {
            /* ★★★R219（用户令 2026-11-17）：陌生人来信**不显示名字、直接显示对方好友码** ——
               列表行尾那 6 位数字就是这个功能：看到信就知道该把谁加回来。
               原来这里留白（或显示对方自称，未取名时是「-」），用户没法反向加好友。 */
            memset(name, 0, sizeof(name));
            memcpy(name, code, 6);
            name[6] = 0;
            nlen = 6;
        }
    }
    const char *q3 = e2 + 1;
    const char *e3 = memchr(q3, '|', (size_t)(payload + len - q3));
    if (!e3 || e3 - q3 <= 0 || e3 - q3 >= 8) return;
    char d[8] = {0};
    memcpy(d, q3, (size_t)(e3 - q3));
    const long day = strtol(d, NULL, 10);
    const int off = (int)(e3 + 1 - payload);
    if (len - off < 2268) {
        ESP_LOGW(TAG, "★ 来信不完整（payload %d B · 需要 >=%d B）-> 拒收", len, off + 2268);
        return;
    }
    /* 文本块末尾追加 [sl][发件人名字]（与 v1 同款约定），mypost 列表/详情照常显示 */
    {
        const uint8_t *r = (const uint8_t *)payload + off + 2268;
        const int rlen = len - off - 2268;
        static EXT_RAM_BSS_ATTR uint8_t r2[520];
        int rl2 = 0, ok = 0;
        if (rlen > 0 && nlen > 0 && rlen + 1 + nlen <= (int)sizeof(r2)) {
            memcpy(r2, r, (size_t)rlen); rl2 = rlen;
            r2[rl2++] = (uint8_t)nlen;
            memcpy(r2 + rl2, name, (size_t)nlen); rl2 += nlen;
            ok = 1;
        }
        int k = -1;
        if (s_post_cb)
            k = ok ? s_post_cb((const uint8_t *)payload + off, r2, rl2, (int)day)
                   : s_post_cb((const uint8_t *)payload + off, r, rlen, (int)day);
        if (k < 0) { ESP_LOGW(TAG, "★ 来信落箱失败（信箱满/格式错）"); return; }
        s_got_msg++;
        ESP_LOGW(TAG, "★ 收到定向来信：发件人 %s「%s」-> mypost 槽 %d", code, name, k);
    }
    /* 取件即清自己的信箱（retained 空消息） */
    char top[64];
    snprintf(top, sizeof(top), "%s/%s/inbox", FRNET_PREFIX, friend_my_code());
    esp_mqtt_client_publish(cli, top, "", 0, 1, 1);
}
static void frnet_store_post(esp_mqtt_client_handle_t cli, const char *code,
                             const char *payload, int len)
{
    if (!code || !payload || strncmp(payload, "p1|", 3) != 0) return;
    char hdr[24] = {0};
    int hn = 0;
    while (hn < len - 3 && hn < (int)sizeof(hdr) - 1 && payload[3 + hn] != '|') { hdr[hn] = payload[3 + hn]; ++hn; }
    if (hn <= 0 || hn >= 8) return;                        /* 日序字段异常 → 拒 */
    const long day = strtol(hdr, NULL, 10);
    const int off = 3 + hn + 1;                            /* 跳过 "p1|<day>|" */
    if (len - off < 2268) {
        ESP_LOGW(TAG, "★ 收到好友 %s 的明信片但不完整（payload %d B · 需要 ≥%d B）→ 拒收",
                 code, len, off + 2268);
        return;
    }
    const int idx = frnet_find_friend(code);
    if (idx < 0) { ESP_LOGW(TAG, "收到非好友 %s 的明信片，忽略", code); return; }
    int k = -1;
    /* ★★2026-10-27 用户令：收件必须**保留明信片自己的标题**，发送者单独带一程。
       原字段块 [tl][text][tal][title][aal][addr] 一字不动，末尾追加 [sl][sender]
       ⇒ 标题（卡名）与地点都不会丢；mypost 侧把 sender 单存，列表行尾右对齐显示。 */
    {
        const uint8_t *r = (const uint8_t *)payload + off + 2268;
        const int rlen = len - off - 2268;
        static EXT_RAM_BSS_ATTR uint8_t r2[520];
        const friend_info_t *ff = friend_at(idx);
        const int nl = ff ? (int)strnlen(ff->name, sizeof(ff->name)) : 0;
        int rl2 = 0, ok = 0;
        if (rlen > 0 && nl > 0 && nl <= 30 && rlen + 1 + nl <= (int)sizeof(r2)) {
            memcpy(r2, r, (size_t)rlen); rl2 = rlen;      /* 原样保留（含标题/地址） */
            r2[rl2++] = (uint8_t)nl;                      /* [sl] 发送者长度 */
            memcpy(r2 + rl2, ff->name, (size_t)nl); rl2 += nl;
            ok = 1;
        }
        if (s_post_cb)
            k = ok ? s_post_cb((const uint8_t *)payload + off, r2, rl2, (int)day)
                   : s_post_cb((const uint8_t *)payload + off, r, rlen, (int)day);
    }
    if (k < 0) { ESP_LOGW(TAG, "★ 好友 %s 的明信片落箱失败（信箱满/格式错）", code); return; }
    s_got_msg++;
    ESP_LOGW(TAG, "★ 好友 %s 寄来明信片 → mypost 槽 %d", code, k);
    char top[64];
    snprintf(top, sizeof(top), "%s/%s/post", FRNET_PREFIX, code);
    esp_mqtt_client_publish(cli, top, "", 0, 1, 1);        /* 取件 → 清信箱 */
}

/* ★R114 重构：三处「短连接 MQTT」样板合一。
   friend_net_send_msg / send_visit / send_post 原来各自抄了一遍同样 16 行
   （生成 client_id → 填 esp_mqtt_client_config_t → init → 挂回调 → start → 轮询等连上），
   唯一真差异只有「等多久」（同步窗口 FRNET_WAIT_CONN_MS=6s / 手点 FRNET_SEND_WAIT_MS=12s）
   和失败返回码。拆成 open + await 两步，是为了让调用方仍能区分
   「init 失败」与「连不上」两种失败（原来三处各用 -4 / -2 / -1，语义不同，不能合并成一个）。
   ⚠ conn 必须是**调用方自己的局部 volatile 标志**：回调通过 arg 回写它，
     不能用全局 s_connected —— 后台同步也在用那个，会串味（见 frnet_mqtt_cb 的 ★短连接注释）。 */
static void frnet_mqtt_cb(void *arg, esp_event_base_t base, int32_t id, void *data);  /* 定义在本段之后 */

static esp_mqtt_client_handle_t frnet_mqtt_open(volatile bool *conn)
{
    char cid[40];
    snprintf(cid, sizeof(cid), "petcat-%s-tx%u", friend_my_code(), (unsigned)++s_tx_seq);
    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname  = FRNET_BROKER,
        .broker.address.port      = FRNET_PORT,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id    = cid,
        .session.keepalive        = 60,
        .task.stack_size          = 6144,
        .buffer.size              = FRNET_BUF_SZ,   /* ★4KB：否则 2.7KB 明信片收发不了 */
    };
    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&cfg);
    if (!cli) return NULL;
    *conn = false;
    esp_mqtt_client_register_event(cli, MQTT_EVENT_ANY, frnet_mqtt_cb, (void *)conn);
    esp_mqtt_client_start(cli);
    return cli;
}

/* 等 conn 变 true，最多 wait_ms 毫秒。返回是否连上（未连上由调用方 destroy）。 */
static bool frnet_mqtt_await(int wait_ms, volatile bool *conn)
{
    for (int i = 0; i < wait_ms / 50 && !*conn; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    return *conn;
}

static void frnet_mqtt_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        /* ★R119 修：只有**后台同步**（arg==NULL）才写全局 s_connected；
           短连接（手点发消息/串门/寄片）自带局部标志，原无条件写全局会让
           后台 frnet_try() 误判"已连上"而跳过真正同步。 */
        if (!arg) s_connected = true;
        if (arg) *(volatile bool *)arg = true;   /* ★短连接自己的「已连上」标志（别被后台同步串味） */
        break;
    case MQTT_EVENT_ERROR:
        /* ★R156：DNS/TCP 层失败后 broker 不会再送 CONNECTED，别把等待窗口白烧完。 */
        if (!arg) s_conn_err = true;
        /* ★2026-10-24 真机首测连不上 → 把错误类型打全（DNS/TCP/TLS 哪一环一眼定位） */
        if (e && e->error_handle) {
            ESP_LOGW(TAG, "MQTT ERROR: type=%d esp_tls=%d connect_ret=%d",
                     (int)e->error_handle->error_type,
                     (int)e->error_handle->esp_tls_last_esp_err,
                     (int)e->error_handle->connect_return_code);
        }
        break;
    case MQTT_EVENT_SUBSCRIBED:      /* ★R218 诊断：broker 对订阅的确认（有它才说明订阅真的生效） */
        ESP_LOGW(TAG, "★ 订阅已确认 msg_id=%d", e->msg_id);
        break;
    case MQTT_EVENT_DATA: {
        /* topic = petcat/v1/<6位码>/{state|msg}；从 topic 抠出码 + 尾段分派 */
        char top[48] = {0};
        int tn = (e->topic_len < (int)sizeof(top) - 1) ? e->topic_len : (int)sizeof(top) - 1;
        memcpy(top, e->topic, (size_t)tn);
        top[tn] = 0;
        const char *pre = FRNET_PREFIX "/";
        const int  pre_n = (int)strlen(pre);
        if (strncmp(top, pre, (size_t)pre_n) != 0) break;
        /* ★2026-10-27 诊断：把每条收到的 topic/长度打全 —— 转发收不到时一眼判断
           「没订阅到 / 被拆包 / 收到但被拒」。 */
        ESP_LOGW(TAG, "MQTT DATA %s（%d/%d B）", top,
                 (int)e->data_len, (int)e->total_data_len);
        const char *code = top + pre_n;
        char *slash = strchr(code, '/');
        if (!slash || slash - code != 6) break;             /* 不是 6 位码的 topic 不认 */
        *slash = 0;
        const char *tail = slash + 1;                       /* "state" / "msg" / "post" */
        if (strcmp(tail, "msg") == 0)
            frnet_store_msg(e->client, code, e->data, e->data_len);   /* 用事件自带句柄回发清箱 */
        else if (strcmp(tail, "post") == 0)
            frnet_store_post(e->client, code, e->data, e->data_len);
        else if (strcmp(tail, "inbox") == 0)                 /* ★R218：投给我自己的定向来信 */
            frnet_store_inbox(e->client, e->data, e->data_len);
        else if (strcmp(tail, "visit") == 0)
            frnet_store_visit(e->client, code, e->data, e->data_len);
        else if (strcmp(tail, "state") == 0)
            frnet_store_state(code, e->data, e->data_len);
        break;
    }
    default:
        break;    /* DISCONNECTED / ERROR / 断线重连等都交给等待超时收口 */
    }
}

/* ★2026-10-24 真机首测连不上（同路由 PC 通、设备 8s 超时）→ 先手动预解析 DNS 把 IP
   打进日志（区分「解析失败」还是「TCP 连不上」），再交给 esp-mqtt 连。 */
static void frnet_log_dns(const char *host)
{
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    const int rc = getaddrinfo(host, "1883", &hints, &res);
    if (rc != 0 || !res || !res->ai_addr) {
        ESP_LOGW(TAG, "DNS 解析 %s 失败（rc=%d）", host, rc);
    } else {
        char ip[16] = {0};
        const struct sockaddr_in *a = (const struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(a->sin_addr, ip, sizeof(ip));
        ESP_LOGW(TAG, "DNS %s → %s", host, ip);
    }
    if (res) freeaddrinfo(res);
}

/* 对单个 broker 走一轮完整批量（连-发-收-断）。
   返回 1 = 收到好友状态；0 = 连上并发布成功（对方还没挂状态，属正常）；-1 = 连接失败。 */
static int frnet_try(const char *host, int port, int n_fr)
{
    const char *my = friend_my_code();
    s_connected = false;
    s_conn_err  = false;
    s_got_new   = 0;

    char cid[24];
    snprintf(cid, sizeof(cid), "petcat-%s", my);

    esp_mqtt_client_config_t cfg = {
        .broker.address.hostname = host,
        .broker.address.port     = port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.client_id   = cid,
        .session.keepalive       = 60,
        .task.stack_size         = 6144,
        .buffer.size             = FRNET_BUF_SZ,   /* ★4KB：否则 2.7KB 明信片收发不了 */
    };   /* ★2026-10-24：IDF5.5 不在 config 里挂回调 —— 用下面 register_event（漏挂 = 永远等不到 CONNECTED） */
    esp_mqtt_client_handle_t cli = esp_mqtt_client_init(&cfg);
    if (!cli) { ESP_LOGW(TAG, "mqtt init 失败 → 本轮跳过"); return -1; }
    esp_mqtt_client_register_event(cli, MQTT_EVENT_ANY, frnet_mqtt_cb, NULL);
    esp_mqtt_client_start(cli);

    /* 等 broker 连上（连接成功回调里订阅 + 发布，一次做完） */
    for (int i = 0; i < FRNET_WAIT_CONN_MS / 50 && !s_connected && !s_conn_err; ++i)
        vTaskDelay(pdMS_TO_TICKS(50));
    if (!s_connected) {
        ESP_LOGW(TAG, "broker %s:%d 连不上（%d ms）→ 换下一家/放弃（不影响正常同步）",
                 host, port, FRNET_WAIT_CONN_MS);
        esp_mqtt_client_destroy(cli);
        return -1;
    }

    /* 发布自己状态（retained：对方随时来拉都是最新的；消息几十字节，公共 broker 礼仪内）
       ★v3 末尾带猫名（好友串门告示牌要显示「对方猫的名字」）；v2 老固件仍按前 5 段解析。 */
    char top[40], pay[96];
    snprintf(top, sizeof(top), "%s/%s/state", FRNET_PREFIX, my);
    snprintf(pay, sizeof(pay), "v3|%d|%d|%d|%d|%s", s_mood, s_intim, s_day, s_vis, s_name);
    esp_mqtt_client_publish(cli, top, pay, 0, 1, 1);     /* qos1 · retained */

    /* ★R218：订阅**自己的收件箱** —— 定向投递（p2）落在这里，取走即清。
       ★诊断：把码和 subscribe 返回值都打出来 —— 收不到信时第一眼看这里
       （返回 -1 = 未连接/参数错；正常应 >= 0）。 */
    snprintf(top, sizeof(top), "%s/%s/inbox", FRNET_PREFIX, my);
    {
        const int src = esp_mqtt_client_subscribe(cli, top, 1);
        ESP_LOGW(TAG, "★ 好友链：我的码=%s · 订阅 %s → rc=%d", my, top, src);
    }

    /* 逐个订阅好友 topic（状态 + 留言信箱 + 明信片信箱）：retained 消息会立刻推回来 */
    for (int i = 0; i < n_fr; ++i) {
        const friend_info_t *f = friend_at(i);
        if (!f) continue;
        snprintf(top, sizeof(top), "%s/%s/state", FRNET_PREFIX, f->code);
        esp_mqtt_client_subscribe(cli, top, 1);
        snprintf(top, sizeof(top), "%s/%s/msg",   FRNET_PREFIX, f->code);
        esp_mqtt_client_subscribe(cli, top, 1);
        /* ★2026-10-26 修：补订好友的明信片信箱 —— 原来漏了这条，转发永远收不到。 */
        snprintf(top, sizeof(top), "%s/%s/post",  FRNET_PREFIX, f->code);
        esp_mqtt_client_subscribe(cli, top, 1);
        /* ★2026-10-27：串门信号信箱（测试「发给他」用；正常游玩仍走 state 的 vis 变化）。 */
        snprintf(top, sizeof(top), "%s/%s/visit", FRNET_PREFIX, f->code);
        esp_mqtt_client_subscribe(cli, top, 1);
    }

    vTaskDelay(pdMS_TO_TICKS(FRNET_COLLECT_MS));         /* 收 retained 窗口 */
    esp_mqtt_client_destroy(cli);
    ESP_LOGW(TAG, "好友链路同步完成（%s · 订阅 %d 位好友 · 拉到 %d 条好友状态）",
             host, n_fr, s_got_new);
    return (s_got_new > 0) ? 1 : 0;
}

bool friend_net_sync(void)
{
    const int n_fr = friend_count();
    const char *my = friend_my_code();
    if (n_fr <= 0 || n_fr > FRNET_STATE_MAX || !my || strlen(my) != 6) return false;

    frnet_log_dns(FRNET_BROKER);
    const int r = frnet_try(FRNET_BROKER, FRNET_PORT, n_fr);
    if (r >= 0) return (r == 1);        /* 连上即收手 —— 不为「对方还没挂状态」白烧一次无线电 */
    /* ★回落：国内接入点连不上 → 试全球接入点（同一个 EMQX 集群的两扇门） */
    frnet_log_dns(FRNET_BROKER_FALLBACK);
    return frnet_try(FRNET_BROKER_FALLBACK, FRNET_PORT, n_fr) == 1;
}

/* ★R150：删掉 friend_net_state_of()（原 432-449，17 行）—— 全工程零调用。
   好友页的心情/日期读的是 friend_net_* 的另一套取数，本函数是早期接口，
   没有任何调用点（链接期已被 gc）。要恢复：git 里 R149 及之前都有。 */

/* ★R121：串门信号写下的 fn<i>_vend 到期后，vis 自动按 0 处理（不写 NVS，避免每帧磨损）。 */
bool friend_net_vis_of(int i, int *vis)
{
    if (i < 0 || i >= FRNET_STATE_MAX || !vis) return false;
    nvs_handle_t h;
    int32_t v = 0, vend = 0;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return false;
    char k[16];
    snprintf(k, sizeof(k), "fn%d_vis", i);
    const bool ok = (nvs_get_i32(h, k, &v) == ESP_OK);
    snprintf(k, sizeof(k), "fn%d_vend", i);
    nvs_get_i32(h, k, &vend);
    nvs_close(h);
    if (!ok) return false;
    if (v == 1 && vend > 0 && (int32_t)s_now_local >= vend) v = 0;   /* 串门到期 */
    *vis = (int)v;
    return true;
}

/* ★v3：对方猫名（好友串门告示牌用）。没收到过（旧固件 / 从未同步）→ false。 */
bool friend_net_name_of(int i, char *out, int cap)
{
    if (i < 0 || i >= FRNET_STATE_MAX || !out || cap <= 1) return false;
    out[0] = 0;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return false;
    char k[16];
    snprintf(k, sizeof(k), "fn%d_nm", i);
    size_t l = (size_t)cap;
    const bool ok = (nvs_get_str(h, k, out, &l) == ESP_OK && out[0]);
    nvs_close(h);
    return ok;
}

/* ── 留言 v1（信箱模式，见 friend_net.h）────────────────────────────── */

/* ★R151：msg_of 不恢复 —— 留言正文至今没有上屏路径，恢复回来仍是死码。
   但 mark_read 必须回来，它是 fnN_mr 唯一的写 0 路径，见下方说明。 */

bool friend_net_msg_unread(int i)
{
    if (i < 0 || i >= FRNET_STATE_MAX) return false;
    nvs_handle_t h;
    int32_t mr = 0;
    if (nvs_open("petns", NVS_READONLY, &h) != ESP_OK) return false;
    char k[16];
    snprintf(k, sizeof(k), "fn%d_mr", i);
    nvs_get_i32(h, k, &mr);
    nvs_close(h);
    return mr == 1;
}

/* ★★R151（复核发现 P1）：R150 把本函数当「零调用死函数」删掉，实际删掉的是
   fnN_mr 唯一的写 0 路径 —— 之后全仓只剩 frnet_store_msg() 写 1 与
   friend_net_msg_unread() 读，好友页「新留言」一旦亮起就永不消失。
   R150 注释称「已读位由同步流程在收件时统一改写」与代码不符：同步只写 1、不写 0；
   frnet_forget_one() 擦键是删好友时作废缓存，不是标记已读。
   ★R152 更正：R151 的调用点曾落在 go_page() 的 #if TOUCH_DEMO 块内，按键版无效；
   本版已挪到 #endif 之后的公共区，才真正「两版共用」。
   ★R152 检阅 P2：补 NVS 返回值检查；已是 0 就短路，避免每次进详情页都写一次 flash。 */
void friend_net_msg_mark_read(int i)
{
    if (i < 0 || i >= FRNET_STATE_MAX) return;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    snprintf(k, sizeof(k), "fn%d_mr", i);
    int32_t cur = 1;
    if (nvs_get_i32(h, k, &cur) == ESP_OK && cur == 0) { nvs_close(h); return; }
    const esp_err_t es = nvs_set_i32(h, k, 0);
    const esp_err_t ec = nvs_commit(h);
    if (es != ESP_OK || ec != ESP_OK)
        ESP_LOGW(TAG, "mark_read(%d) NVS set=%d commit=%d", i, es, ec);
    nvs_close(h);
}

/* ★★ R92（2026-10-27）：好友被删/被改码时，作废对应的 fn<i>_* 本地缓存。
   为什么必须做：这些键是**按好友下标**存的，删掉中间一条会让后面所有人下标前移
   ⇒ 不清理就会「张三的留言挂到李四名下」。清掉只是丢一次缓存，下次同步会重建
   （留言/未读/心情/在不在家全部会重新从对方设备拉到）。
   键清单与文件上半部读写处一一对应：md / dy / vis / visday / nm / msg / msgdy / mr。 */
static void frnet_forget_one(int i)
{
    if (i < 0 || i >= FRNET_STATE_MAX) return;
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return;
    static const char *SUF[] = { "md", "dy", "vis", "vend", "visday", "nm", "msg", "msgdy", "mr" };
    for (size_t k = 0; k < sizeof(SUF) / sizeof(SUF[0]); ++k) {
        char key[16];
        snprintf(key, sizeof(key), "fn%d_%s", i, SUF[k]);
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

void friend_net_forget_one(int i)
{
    frnet_forget_one(i);
    ESP_LOGW(TAG, "★ 好友 %d 的本地状态缓存已作废（下次同步重建）", i);
}

void friend_net_forget_from(int from)
{
    if (from < 0) from = 0;
    for (int i = from; i < FRNET_STATE_MAX; ++i) frnet_forget_one(i);
    ESP_LOGW(TAG, "★ 好友 %d 及之后的本地状态缓存全部作废（下标已前移）", from);
}


/* ★R150：删掉 friend_net_send_msg()（原 563-589，含注释 27 行）—— 全工程零调用。
   实际发留言/串门/明信片走的是 send_visit / send_post 那两条短连接（它们各自调用
   frnet_mqtt_open/await），本函数是早期的独立发信接口，没有任何调用点（链接期已被 gc）。
   要恢复：git 里 R149 及之前都有。 */

/* ★R121：把「好友 i 的猫正在我家串门」写进本地缓存 —— vis=1 + 到期本地时刻。
   hours<=0 或超上限 → 按 VISIT_MAX_H 兜底。本机时钟没同步（算不出到期）→ 不写。 */
static void frnet_visit_mark(int idx, int hours)
{
    if (idx < 0 || idx >= FRNET_STATE_MAX) return;
    if (hours <= 0 || hours > VISIT_MAX_H) hours = VISIT_MAX_H;
    if (s_now_local <= 0) {
        ESP_LOGW(TAG, "★ 串门信号丢弃：本机时钟未同步，算不出到期时刻");
        return;
    }
    const int32_t vend = (int32_t)(s_now_local + (time_t)hours * 3600);
    nvs_handle_t h;
    if (nvs_open("petns", NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    snprintf(k, sizeof(k), "fn%d_vis", idx);  nvs_set_i32(h, k, 1);
    snprintf(k, sizeof(k), "fn%d_vend", idx); nvs_set_i32(h, k, vend);
    nvs_commit(h);
    nvs_close(h);
    s_home_dirty = true;                     /* 主循环取走后重绘主页 */
    ESP_LOGW(TAG, "★ 好友 %d 的猫来串门：显示 %d 小时（到期 %ld）", idx, hours, (long)vend);
}

/* ★2026-10-27 用户令（测试入口）：主动触发一次串门 —— 把自己的「来串门」信号
   挂到自己码下的 `/visit` 信箱（好友同步时来取）。与明信片链路完全独立。
   ★R121：payload = `w1|<lvl>|<剩余小时>`，剩余小时按本机真实串门窗口算（不在窗口 = 0
   ⇒ 收件侧按 VISIT_MAX_H 兜底）。0 = 成功挂上。 */
int friend_net_send_visit(int friend_idx, int lvl)
{
    const int n_fr = friend_count();
    if (friend_idx < 0 || friend_idx >= n_fr || friend_idx >= FRNET_STATE_MAX) return -1;
    const friend_info_t *f = friend_at(friend_idx);
    if (!f) return -1;

    /* 本机真实串门窗口的剩余小时（visit_t0/visit_h 与 app_self_vis 同一套钟） */
    int remain_h = 0;
    {
        nvs_handle_t h;
        if (nvs_open("petns", NVS_READONLY, &h) == ESP_OK) {
            int32_t vt0 = 0, vh = 0;
            nvs_get_i32(h, "visit_t0", &vt0);
            nvs_get_i32(h, "visit_h",  &vh);
            nvs_close(h);
            if (vt0 > 0 && vh > 0 && s_now_local > 0) {
                const int32_t left = vt0 + vh * 3600 - (int32_t)s_now_local;
                if (left > 0) remain_h = (left + 3599) / 3600;   /* 不足 1h 按 1h */
                if (remain_h > VISIT_MAX_H) remain_h = VISIT_MAX_H;
            }
        }
    }

    volatile bool conn = false;
    esp_mqtt_client_handle_t cli = frnet_mqtt_open(&conn);
    if (!cli) return -4;
    if (!frnet_mqtt_await(FRNET_SEND_WAIT_MS, &conn)) { esp_mqtt_client_destroy(cli); return -2; }

    char top[64], pay[24];
    snprintf(top, sizeof(top), "%s/%s/visit", FRNET_PREFIX, friend_my_code());
    snprintf(pay, sizeof(pay), "w1|%d|%d", (lvl >= 2) ? 2 : 1, remain_h);
    const int pub = esp_mqtt_client_publish(cli, top, pay, 0, 1, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_mqtt_client_destroy(cli);
    ESP_LOGW(TAG, "★ 串门信号已挂到自己信箱（好友同步时来取，rc=%d · lvl=%d · 剩余 %d 小时）",
             pub, lvl, remain_h);
    return (pub >= 0) ? 0 : -3;
}

/* ★R121：收到好友的串门信号 → **直接落主页**（写 vis=1 + 到期时刻），不再弹 3 秒全屏页。 */
static void frnet_store_visit(esp_mqtt_client_handle_t cli, const char *code,
                              const char *payload, int len)
{
    if (!code || !payload || len < 4 || strncmp(payload, "w1|", 3) != 0) return;
    const int idx = frnet_find_friend(code);
    if (idx < 0) { ESP_LOGW(TAG, "收到非好友 %s 的串门信号，忽略", code); return; }
    const int lvl = (payload[3] == '2') ? 2 : 1;
    /* 第三段「剩余小时」可缺省（旧固件只发 w1|<lvl>）→ 0 = 按 8h 兜底 */
    int hours = 0;
    if (len >= 5 && payload[4] == '|') {
        for (int i = 5; i < len && payload[i] >= '0' && payload[i] <= '9'; ++i)
            hours = hours * 10 + (payload[i] - '0');
    }
    frnet_visit_mark(idx, hours);
    ESP_LOGW(TAG, "★ 好友 %s 发来串门信号（lvl=%d · %d 小时）→ 主页直接显示",
             code, lvl, (hours <= 0 || hours > VISIT_MAX_H) ? VISIT_MAX_H : hours);
    char top[64];
    snprintf(top, sizeof(top), "%s/%s/visit", FRNET_PREFIX, code);
    esp_mqtt_client_publish(cli, top, "", 0, 1, 1);       /* 取件 → 清信箱 */
}

void friend_net_local_visit(int i, int hours)
{
    frnet_visit_mark(i, hours);
}

bool friend_net_take_home_dirty(void)
{
    if (!s_home_dirty) return false;
    s_home_dirty = false;
    return true;
}

/* 明信片转发（短连接 ~6s）。photo = 2268B 原始 1-bit；rest = mypost 字段块
   [tl][text][tal][title][aal][addr]（不含 0xFF 哨兵）。0 = 成功挂上对方信箱。 */
int friend_net_send_post(int friend_idx, const uint8_t *photo, const uint8_t *rest, int restlen)
{
    const int n_fr = friend_count();
    if (friend_idx < 0 || friend_idx >= n_fr || friend_idx >= FRNET_STATE_MAX) {
        ESP_LOGW(TAG, "★ 转发失败：好友下标非法（%d/%d）", friend_idx, n_fr);
        return -1;
    }
    const friend_info_t *f = friend_at(friend_idx);
    /* ★R218 复检：好友记录里的码必须正好 6 位，否则会发到 petcat/v1//inbox 这种废 topic */
    if (f && strnlen(f->code, sizeof(f->code)) != 6) {
        ESP_LOGW(TAG, "★ 转发失败：好友码异常（\"%s\"）", f->code);
        return -1;
    }
    if (!f || !photo || !rest || restlen <= 0 || restlen > 400) {
        ESP_LOGW(TAG, "★ 转发失败：参数非法（friend=%p photo=%p rest=%p len=%d）",
                 (const void *)f, (const void *)photo, (const void *)rest, restlen);
        return -1;
    }

    volatile bool conn = false;
    esp_mqtt_client_handle_t cli = frnet_mqtt_open(&conn);
    if (!cli) { ESP_LOGW(TAG, "★ 转发失败：mqtt init 失败"); return -4; }
    if (!frnet_mqtt_await(FRNET_SEND_WAIT_MS, &conn)) {
        ESP_LOGW(TAG, "★ 转发失败：broker %s:%d 连不上（%d ms）", FRNET_BROKER, FRNET_PORT,
                 FRNET_SEND_WAIT_MS);
        esp_mqtt_client_destroy(cli);
        return -2;
    }

    /* ★R218：p2 = **定向投递**（载荷里带发件人码 + 猫名，因为投的是对方信箱）。
       老版 p1 只带日序、靠 topic 认发件人，只能挂在自己码下 → 会被别人抢走。 */
    static EXT_RAM_BSS_ATTR char pay[128 + 2268 + 400];                   /* ~2.4KB：static 防栈爆（mqtt 任务外调用） */
    /* ★R218 复检：未取名时 s_name 就是 "-"，把它当名字发出去毫无意义
       （对方列表会显示一条杠）。发空串 → 收端回退到「好友表里我给他起的名字」。 */
    const char *nm = (s_name[0] && strcmp(s_name, "-") != 0) ? s_name : "";
    const int hd = snprintf(pay, sizeof(pay), "p2|%s|%s|%d|",
                            friend_my_code(), nm, (int)s_day);
    memcpy(pay + hd, photo, 2268);
    memcpy(pay + hd + 2268, rest, (size_t)restlen);
    const int total = hd + 2268 + restlen;

    char top[64];
    /* ★R218：投到**收件人**的收件箱（f->code 来自我自己的好友表）。
       原写法挂在自己 `/post` 下 → 谁先同步谁取走，可能被第三方截走、且对方没加我就永远收不到。 */
    snprintf(top, sizeof(top), "%s/%s/inbox", FRNET_PREFIX, f->code);
    const int pub = esp_mqtt_client_publish(cli, top, pay, total, 1, 1);   /* qos1 · retained */
    vTaskDelay(pdMS_TO_TICKS(500));                      /* 2.4KB qos1 出栈留时间 */
    esp_mqtt_client_destroy(cli);
    ESP_LOGW(TAG, "★ 明信片已投进「%s」的收件箱（%dB，rc=%d）", f->code, total, pub);
    return (pub >= 0) ? 0 : -3;
}
