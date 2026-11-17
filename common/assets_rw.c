/* assets_rw.c —— assets 数据分区读取层实现
 * 2026-11-11（C1 前置）：从 main.c 的「assets 读取核心」**逐字搬出**，行为不变。
 * 唯一改动：TAG 用本模块自己的（main.c 的 TAG 仍归 main.c），anim_rle_decode
 * 改名 rle1_decode 并导出（同一套编码，动画与区域压缩共用）。
 * 布局真源 = scripts/_tools/pack_assets.py；区域 ID 顺序见 assets.h。 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_heap_caps.h"     /* heap_caps_malloc / MALLOC_CAP_SPIRAM */
#include "esp_timer.h"         /* ★R196 观测口：量开机解压耗时 */
#include "miniz.h"             /* ★R196：ESP32-S3 ROM 现成的 tinfl_decompress_mem_to_mem() */
#include "sdkconfig.h"         /* ★R197 观测口：CONFIG_ESP_MAIN_TASK_STACK_SIZE */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"     /* ★R197：uxTaskGetStackHighWaterMark(NULL) */

#include "assets.h"
#include "assets_rw.h"

static const char *TAG = "pet_assets";

uint32_t a_rd32(const uint8_t *p)   /* 按字节拼，避免非对齐访问（xtensa 不保证）*/
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 区域目录（开机由 assets_dir_set 填充；读失败全部走 false，调用方兜底）*/
static uint32_t s_aoff[AID_N];
static uint32_t s_alen[AID_N];
/* ★2026-11-12 修：该区在 flash 上是 ZLZ1/RLZ1 压缩流（**含解压失败**的）。
   s_alen[id] 对这类区记的是压缩流长度，不是上层期望的原始长度 —— 必须区分。 */
static bool s_compressed[AID_N];

/* ★R196：区域级压缩**影子缓冲** —— 见 pack_assets.py 的 zlib_wrap()。
   现在所有「4KB 对齐后真省页」的区都整区压（明信片/字库/UI 容器/动画容器/
   i18n/网页/窗景…），开机解到 PSRAM，assets_region_read() 优先从影子 memcpy
   ⇒ 上层调用一行不用改，也不用碰 UIRL / ZFNT / RLE1 这些自带容器的解析层。
   ⚠ 本板 PSRAM 8MB；影子总占用约 3.3MB（原来 2.0MB），开机一次分配、常驻。 */
static uint8_t *s_shadow[AID_N];
static uint32_t s_shadow_len[AID_N];

void assets_dir_set(int id, uint32_t off, uint32_t len)
{
    if (id < 0 || id >= AID_N) return;
    s_aoff[id] = off;
    s_alen[id] = len;
}

/* 取区域 off/len（没登录目录 → false）。len 对压缩区返回**解压后长度**，
   这样所有调用方（按 len 做边界/推导条数）看到的仍是原始语义。
   ★2026-11-12 修：压缩区若没建起 PSRAM 影子（分配失败 / tinfl 失败 / 流损坏），
     绝不能把 s_alen 的**压缩流长度**当 len 交出去 —— 上层会按这个错分母通过
     边界检查，随后 assets_region_read() 回退直读 flash，把 deflate 字节当位图渲染
     （AID_LOOT 实测 88/185 张图标会乱码）。这类区直接返回 false，让上层走兜底页。 */
bool assets_region(int id, uint32_t *off, uint32_t *len)
{
    if (id < 0 || id >= AID_N || s_alen[id] == 0) return false;
    if (s_compressed[id] && !s_shadow[id]) return false;
    if (off) *off = s_aoff[id];
    if (len) *len = s_shadow[id] ? s_shadow_len[id] : s_alen[id];
    return true;
}

/* 区域内**相对偏移**读（带边界；任何失败返回 false，绝不越界）*/
bool assets_region_read(int id, uint32_t rel, void *dst, size_t n)
{
    uint32_t off, len;
    if (!assets_region(id, &off, &len)) return false;
    if ((uint64_t)rel + n > len) return false;
    if (id >= 0 && id < AID_N && s_shadow[id]) {   /* ★R182：压缩区走 PSRAM 影子 */
        memcpy(dst, s_shadow[id] + rel, n);
        return true;
    }
    return assets_read(off + rel, dst, n);
}

/* RLE 数据流（字节级）：
     控制字节 c & 0x80 → 游程：n = (c & 0x7F) + 1，后跟 **1 字节** 值，重复 n 次
     控制字节 c < 0x80 → 字面：n = c + 1，后跟 **n 字节** 原文
     （编码端游程阈值 = 3：run==2 时"控制+值"2 字节 == 原文 2 字节，不赚，只存字面）
   解压结果长度必须**正好等于** dst_len，否则 -1（区域压缩与动画帧共用本解码器）。 */
int rle1_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len)
{
    size_t si = 0, di = 0;
    while (si < src_len) {
        uint8_t c = src[si++];
        if (c & 0x80u) {                       /* 游程 */
            size_t n = (size_t)(c & 0x7Fu) + 1u;
            if (si >= src_len || di + n > dst_len) return -1;
            memset(dst + di, src[si++], n);
            di += n;
        } else {                               /* 字面 */
            size_t n = (size_t)c + 1u;
            if (si + n > src_len || di + n > dst_len) return -1;
            memcpy(dst + di, src + si, n);
            si += n;
            di += n;
        }
    }
    return (di == dst_len) ? 0 : -1;          /* 长度必须正好等于一帧 */
}

/* ★R196：把所有压缩区整区解到 PSRAM。**非致命**：某区解不开就跳过，
   该区之后直接读 flash（上层会按格式自检失败 → 走各自的兜底页，不会崩）。
   遍历全部 AID 而不是维护白名单：未压缩区 / 16B stub / UIRL 等自带容器的
   头 4 字节都不等于 ZLZ1/RLZ1，会自然跳过；以后新增区域也不用改这里。
   ★兼容：'RLZ1' 是 R182 老格式，必须继续认 —— 网页 OTA 只写 app 不写 assets，
     设备上的旧 assets 在新固件里要照常能读。 */
void assets_shadow_load_all(void)
{
    const int64_t t0 = esp_timer_get_time();
    /* ★R197 只读栈观测口（★2026-11-12 修正口径）：high-water 是**自任务启动以来**
       的历史最小剩余栈（单调不增），不是「本次解压」的快照。若此前已有更深调用链，
       「进入时 vs 解压后」的差值会恒为 0，不能当解压开销的证据。
       ⇒ 下面只报历史最低余量与峰值用量，不再报那个差值。 */
    uint32_t total_raw = 0, total_comp = 0;
    int n_zlz = 0, n_rlz = 0;
    for (int id = 0; id < AID_N; ++id) {
        /* ★2026-11-12 修：这里要的是 **flash 上压缩流的长度**，不能走 assets_region()
           （它对压缩区返回解压后长度、影子没建好时直接 false）。 */
        const uint32_t off = s_aoff[id], clen = s_alen[id];
        if (clen < 8) continue;
        uint8_t hdr[8];
        if (!assets_read(off, hdr, 8)) continue;
        const uint32_t magic = a_rd32(hdr);
        if (magic != ZLZ_MAGIC && magic != RLZ_MAGIC) continue;   /* 未压缩 / stub / 自带容器 */
        s_compressed[id] = true;    /* 无论解压成败都标记：此后该区只认影子 */
        const uint32_t raw = a_rd32(hdr + 4);
        if (raw < 8 || raw > 4u * 1024u * 1024u) {
            ESP_LOGE(TAG, "★ assets 区 %d 压缩头异常（magic=0x%08X raw=%u）→ 跳过",
                     id, (unsigned)magic, (unsigned)raw);
            continue;
        }
        uint8_t *tmp = (uint8_t *)heap_caps_malloc(clen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint8_t *buf = (uint8_t *)heap_caps_malloc(raw,  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp || !buf) {
            ESP_LOGE(TAG, "★ assets 区 %d 解压缓冲分配失败（%u+%u B）→ 跳过", id,
                     (unsigned)clen, (unsigned)raw);
            free(tmp); free(buf);
            continue;
        }
        bool ok = assets_read(off, tmp, clen);
        if (ok && magic == ZLZ_MAGIC) {
            /* ROM tinfl：一次性整块解，带 zlib 头 + adler32 校验。
               ★2026-11-12 更正：MINIZ_NO_MALLOC 下**不另申请 32KB LZ 字典** ——
               输出缓冲 buf 本身就当字典（IDF miniz.h:39 / TINFL_LZ_DICT_SIZE）。
               真正吃栈的是栈上 sizeof(tinfl_decompressor) ≈ 10,989 B，
               R197 因此把 main 栈 12288 → 24576；详见 main.c:20121 的更正。 */
            size_t got = tinfl_decompress_mem_to_mem(
                buf, raw, tmp + 8, clen - 8, TINFL_FLAG_PARSE_ZLIB_HEADER);
            ok = (got != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED && got == (size_t)raw);
            if (ok) n_zlz++;
        } else if (ok && magic == RLZ_MAGIC) {
            ok = rle1_decode(tmp + 8, clen - 8, buf, raw) == 0;
            if (ok) n_rlz++;
        }
        free(tmp);
        if (!ok) {
            ESP_LOGE(TAG, "★ assets 区 %d 解压失败（%s %u → %u B）→ 跳过", id,
                     magic == ZLZ_MAGIC ? "ZLZ1" : "RLZ1", (unsigned)clen, (unsigned)raw);
            free(buf);
            continue;
        }
        s_shadow[id] = buf;
        s_shadow_len[id] = raw;
        total_raw += raw;
        total_comp += clen;
    }
    if (total_raw) {
        ESP_LOGI(TAG, "★ assets 影子：ZLZ1×%d RLZ1×%d｜%u B 原图 ← %u B 压缩流"
                 "（省 %u B，占 PSRAM %u B）· 解压耗时 %d ms",
                 n_zlz, n_rlz, (unsigned)total_raw, (unsigned)total_comp,
                 (unsigned)(total_raw - total_comp), (unsigned)total_raw,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    /* ★R196 只读内存观测口：与 main.c::assets_boot_check() 末尾那条配对，
       差值 = 猫表/食盆/字库索引/UI/i18n 各装载器吃掉的量。不申请任何内存。 */
    ESP_LOGI(TAG, "★[MEM] 影子装完：DRAM free=%u B（最大块 %u B）｜PSRAM free=%u B（最大块 %u B）",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    const uint32_t hw_deep = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    /* ★2026-11-12 修：high-water 是历史最小值，不是本次解压快照 —— 去掉
       「进入时 / 解压后差值 = 解压额外吃栈」这个不成立的字段，只报能成立的量。 */
    ESP_LOGI(TAG, "★[STACK] main 栈 %u B｜历史最低余 %u B｜峰值用量 %u B",
             (unsigned)CONFIG_ESP_MAIN_TASK_STACK_SIZE,
             (unsigned)hw_deep,
             (unsigned)(CONFIG_ESP_MAIN_TASK_STACK_SIZE > hw_deep
                        ? CONFIG_ESP_MAIN_TASK_STACK_SIZE - hw_deep : 0));
}
