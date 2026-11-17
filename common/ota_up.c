/* ══════════════════════════════════════════════════════════════════════
 * OTA 本地升级（fw/common/ota_up.c）—— 2026-10-19 改版
 *
 * ★ 放弃在线 OTA：不再联网下载。改为两个**本地**升级源：
 *   ① TF卡：点「检查版本」→ 扫**卡根目录的 *.bin**，读它**内嵌的 esp_app_desc 版本**比版本 →
 *      发现新版则读该 bin 写 OTA 分区（★2026-10-19：不再需要 version.json，交付物 = 单个 bin 文件）。
 *      （SDIO 1-bit，GPIO39/41/40，1.54G 官方脚）
 *   ② 网页上传：net_prov.c 的 /upgrade 路由收文件 → 调 ota_web_* 写 OTA 分区。
 *
 * 底层共用 local_ota_*（写 OTA 分区）。UI 状态机与页面不变（main.c 照旧驱动）。
 *
 * ★ 硬约束（永远这三条，离线也一样）：
 *   ① 校验不过**绝不切启动分区**（esp_ota_end 内部做 image 校验）；
 *   ② 取消 / 失败 / 拔卡 → **旧固件照常**（不写 otadata = 还在原槽）；
 *   ③ 写入中**每循环喂一次看门狗**，别让 TG0WDT 把板子拽重启。
 * ══════════════════════════════════════════════════════════════════════ */
#include <esp_attr.h>       /* ★R198：EXT_RAM_BSS_ATTR（BSS 外置 PSRAM） */
#include "ota_up.h"
#include "ota_net.h"     /* ★R218：在线升级回调（本文件实现，ota_net.c 调用） */

#include "variant.h"     /* ★ TOUCH_DEMO → 选 firmware_key / firmware_touch */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_task_wdt.h"

#include "cJSON.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include <sys/stat.h>

static const char *TAG = "ota_up";

/* ── TF卡引脚（Waveshare ESP32-S3-ePaper-1.54G 官方 SDIO 1-bit）──────── */
#define SD_CLK_GPIO 39
#define SD_CMD_GPIO 41
#define SD_D0_GPIO  40
#define SD_MOUNT_POINT "/sdcard"

#include <dirent.h>
#include <strings.h>

/* ★★★ 2026-10-19 用户令（原话）：「给到人去更新的东西**不是一个文件夹**，而是一个文件」
   ⇒ TF 路不再依赖「firmware_key/touch.bin + version.json」这一套固定文件名 + 文件夹，
     改为：**扫卡根目录里所有 *.bin**，读**它自己内嵌的 esp_app_desc 版本**（与网页路同源，
     版本真值都来自固件本体），挑第一个「比当前固件新」的用。
   ⇒ 卡根目录放**一个** bin 就行（文件名任意、可带前缀，如 `PetCat_v6.2_touch.bin`），
     要是拷了**文件夹**（bin 在子目录里）→ 扫不到 → 失败页明确说「卡上没有固件文件」。
   ⚠ version.json 就此作废（TF 路不再读它）。 */
static EXT_RAM_BSS_ATTR char s_tf_path[300];        /* 本次选中的固件文件全路径（do_check_sd 写，flash 时读） */

/* 读 bin 内嵌版本：esp_image 头 0xE9 + esp_app_desc magic 0xABCD5432 @0x20、版本串 @0x30。
   成功返回 true 并填 ver（去掉前导 v）与实际字节数。 */
static bool bin_read_ver(const char *path, char *out, size_t cap, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    uint8_t hdr[0x50];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && hdr[0] == 0xE9) {
        const uint32_t mg = (uint32_t)hdr[0x20] | ((uint32_t)hdr[0x21] << 8) |
                            ((uint32_t)hdr[0x22] << 16) | ((uint32_t)hdr[0x23] << 24);
        if (mg == 0xABCD5432u) {
            char v[33];
            memcpy(v, hdr + 0x30, 32);
            v[32] = 0;
            for (char *p = v; *p; ++p) if (*p == ' ' || *p == '\n' || *p == '\r') { *p = 0; break; }
            const char *num = (v[0] == 'v' || v[0] == 'V') ? v + 1 : v;
            if (num[0] >= '0' && num[0] <= '9') {
                /* ★%.*s 限长：版本串字段 32B、目标缓冲可能更小 → 不加精度会被
                   -Werror=format-truncation 拦下（"directive output may be truncated"）。 */
                snprintf(out, cap, "%.*s", (int)(cap > 1 ? cap - 1 : 0), num);
                if (fseek(f, 0, SEEK_END) == 0) { *out_size = ftell(f); ok = true; }
            }
        }
    }
    fclose(f);
    if (!ok) ESP_LOGW(TAG, "跳过 %s（不是带版本号的固件 bin）", path);
    return ok;
}

/* ★2026-10-19：`TF_UPG_TMP`（网页升级落卡中转文件）随「直写非运行槽」一并删除 ——
   网页升级不再碰 TF卡。 */

/* ── 全局状态（UI 只读） ─────────────────────────────────────────────── */
static ota_status_t S;                  /* UI 只读状态 */
static bool s_demo = false;             /* 巡检模式 */
static volatile bool s_cancel = false;
static volatile bool s_busy   = false;  /* 有任务在跑 */
static bool s_web = false;              /* true = 当前为网页上传源（net_prov 驱动） */
static bool s_mounted = false;          /* TF卡是否已挂载 */
static sdmmc_card_t *s_card = NULL;     /* TF卡句柄 */
/* ★2026-10-22 抽公共层：mypost 满仓转存也要用卡（与 OTA 共用同一份挂载态）。
   持有者计数 + 串行化信号量 —— httpd 任务（收图回调）与 OTA 任务都可能调挂载。 */
static int  s_sd_ref = 0;               /* 引用计数（OTA / mypost 各算一份）*/
static StaticSemaphore_t s_sd_mtx_buf;
static SemaphoreHandle_t s_sd_mtx = NULL;

/* ── 底层 OTA 写分区句柄 ────────────────────────────────────────────── */
static const esp_partition_t *s_tgt = NULL;
static esp_ota_handle_t       s_ota_h = 0;
static int  s_written = 0;
static int  s_exp_size = 0;             /* 期望总字节（0 = 未知，只靠 esp_ota_end 校验） */

/* ★ 这些判据在 main.c 持有：ota_batt_low() = 当前低电量（<阈值就拦，语义等价「电量太低」）。 */
extern bool ota_batt_low(void);


/* ══════════════════════════════════════════════════════════════════════
 * 底层：写 OTA 分区（TF 与网页上传共用）
 * ══════════════════════════════════════════════════════════════════════ */
static void set_fail(const char *why)
{
    S.st = OTA_FAILED;
    snprintf(S.reason, sizeof(S.reason), "%s", why);
    ESP_LOGW(TAG, "★ OTA 失败：%s", why);
}

static bool local_ota_begin(int exp_size)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    s_tgt = esp_ota_get_next_update_partition(run);
    if (!s_tgt) { set_fail("找不到备用分区"); return false; }
    if (exp_size > 0 && exp_size > (int)s_tgt->size) {
        ESP_LOGW(TAG, "包 %d B > 目标槽 %u B", exp_size, (unsigned)s_tgt->size);
        set_fail("安装包太大，装不下"); return false;
    }
    s_written = 0; s_exp_size = exp_size;
    /* ★★★ 2026-10-19：用 **OTA_WITH_SEQUENTIAL_WRITES**（不是 OTA_SIZE_UNKNOWN）——
       OTA_SIZE_UNKNOWN 会让 begin **同步擦整片 2MB**（数秒）；网页路在本任务里同步跑，
       这几秒里还没收到一个字节，手机的 POST 干等 ⇒ 收包停顿/超时 ⇒ 必现「上传中断」。
       顺序写入模式 begin 立即返回，由 esp_ota_write 按需增量擦（4KB 一档）。 */
    esp_err_t e = esp_ota_begin(s_tgt, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_h);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_begin 失败 %s", esp_err_to_name(e));
                       set_fail("开写失败"); s_tgt = NULL; return false; }
    return true;
}

static bool local_ota_feed(const uint8_t *d, size_t n)
{
    esp_err_t e = esp_ota_write(s_ota_h, d, n);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_ota_write 失败 %s", esp_err_to_name(e)); return false; }
    s_written += (int)n;
    if (s_exp_size > 0) {
        int p = s_written * 100 / s_exp_size;
        if (p > 100) p = 100;
        if (p > S.pct) S.pct = p;       /* 单调上升，UI 才顺 */
    }
    return true;
}

static bool local_ota_finalize(void)
{
    /* esp_ota_end 内部做 image 校验（魔数 / SHA-256）；不过 → 返回错误且不切分区 */
    esp_err_t e = esp_ota_end(s_ota_h);
    s_ota_h = 0;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 校验失败 %s（**不切启动分区**）", esp_err_to_name(e));
        set_fail("固件校验没通过，已取消"); return false;
    }
    e = esp_ota_set_boot_partition(s_tgt);
    if (e != ESP_OK) { ESP_LOGE(TAG, "切启动分区失败 %s", esp_err_to_name(e));
                       set_fail("切分区失败"); return false; }
    S.pct = 100; S.st = OTA_DONE;
    ESP_LOGI(TAG, "✓ 写入完成并切到新分区 → 等 3 秒自动重启");
    return true;
}

static void local_ota_abort(void)
{
    if (s_ota_h) {
        /* 把不完整的 image 交给 esp_ota_end → 它判定校验失败并丢弃本次写，
           旧固件原槽不动。 */
        esp_ota_end(s_ota_h);
        s_ota_h = 0;
    }
    s_tgt = NULL;
}


/* ══════════════════════════════════════════════════════════════════════
 * TF卡：挂载 / 卸载（SDIO 1-bit，GPIO39/41/40）
 * ★2026-10-22 公共层（引用计数）：OTA 与 mypost 满仓转存共用。
 *   ota_sd_mount()  已挂载 → 直接返 true 并 ref+1；否则真挂载（ref=1）
 *   ota_sd_unmount() ref-1；归零才真卸（ref=0 时调 = no-op）
 *   ota_sd_ready()  只看当前挂载态，**不尝试挂载**（探卡用）
 * ══════════════════════════════════════════════════════════════════════ */
void ota_sd_init(void)
{
    if (!s_sd_mtx) {
        s_sd_mtx = xSemaphoreCreateBinaryStatic(&s_sd_mtx_buf);
        xSemaphoreGive(s_sd_mtx);
    }
}

bool ota_sd_mount(void)
{
    if (!s_sd_mtx) ota_sd_init();
    xSemaphoreTake(s_sd_mtx, portMAX_DELAY);
    if (s_mounted) { s_sd_ref++; xSemaphoreGive(s_sd_mtx); return true; }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();          /* slot 1，20MHz，走 GPIO matrix */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_CLK_GPIO;
    slot.cmd = SD_CMD_GPIO;
    slot.d0  = SD_D0_GPIO;
    slot.d1  = GPIO_NUM_NC;   /* 1-bit 只用 clk/cmd/d0 */
    slot.d2  = GPIO_NUM_NC;
    slot.d3  = GPIO_NUM_NC;
    slot.d4  = GPIO_NUM_NC;
    slot.d5  = GPIO_NUM_NC;
    slot.d6  = GPIO_NUM_NC;
    slot.d7  = GPIO_NUM_NC;
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;      /* 板上无外置上拉时靠内部（1-bit 低速够用） */
    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t e = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SD 挂载失败 %s（未插卡 / 未格式化 / 接触不良？）", esp_err_to_name(e));
        s_card = NULL;
        xSemaphoreGive(s_sd_mtx);
        return false;
    }
    s_mounted = true;
    s_sd_ref = 1;
    ESP_LOGI(TAG, "SD 挂载 OK：%s，容量约 %llu MB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));
    xSemaphoreGive(s_sd_mtx);
    return true;
}

void ota_sd_unmount(void)
{
    if (!s_sd_mtx) return;
    xSemaphoreTake(s_sd_mtx, portMAX_DELAY);
    if (s_sd_ref > 0) s_sd_ref--;
    if (s_sd_ref == 0 && s_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_mounted = false; s_card = NULL;
    }
    xSemaphoreGive(s_sd_mtx);
}

bool ota_sd_ready(void) { return s_mounted; }

const char *ota_sd_mount_point(void) { return SD_MOUNT_POINT; }


/* ══════════════════════════════════════════════════════════════════════
 * ① TF卡：检查版本
 * ══════════════════════════════════════════════════════════════════════ */
/* ★★★ 2026-10-19 重写（用户令：交付物 = 单个文件；且用户实测"6.2 放卡里也报已是最新"）：
   ★病根（两层）：① 交付物原来是**文件夹**（firmware_*.bin + version.json），用户把文件夹整个拷进卡
     ⇒ bin 落在子目录里，而设备只在**卡根目录**找 `firmware_key/touch.bin` → 找不到；
   ② 找不到（或没有 version.json）时旧代码**静默当成「已是最新」** ⇒ 用户完全看不出是"卡没拷对"
     还是"版本不比当前新"（6.0 那次也是同一个静默路径，看着像逻辑对）。
   ★新口径：扫卡根目录所有 `*.bin`（文件名任意，可带前缀），读**它自己内嵌的 esp_app_desc 版本**
     当版本号（与网页路同源：版本真值都来自固件本体）；挑第一个「比当前固件新」的。
     · 卡上一个 bin 都没有（含"只拷了文件夹"）→ **失败页「卡上没有固件文件 拷到根目录」**（不再静默）
     · 有 bin 但都不比当前新 → 已是最新（真值语义不变）
   ⚠ version.json 不再需要（TF 路不读它）。 */
static void do_check_sd(void)
{
    if (!s_mounted && !ota_sd_mount()) { set_fail("没有检测到TF卡"); return; }

    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) { ESP_LOGE(TAG, "打不开 %s", SD_MOUNT_POINT); set_fail("读不到TF卡目录"); return; }

    extern bool ota_ver_newer(const char *ver);
    int  nbin = 0;
    bool newer = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        const size_t L = strlen(nm);
        if (L < 5 || strcasecmp(nm + L - 4, ".bin") != 0) continue;   /* 只认 *.bin */
        char path[300];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", nm);
        char v[16]; long sz = 0;
        if (!bin_read_ver(path, v, sizeof(v), &sz)) continue;
        nbin++;
        if (!ota_ver_newer(v)) {
            ESP_LOGI(TAG, "卡上 %s 是 v%s，不比本固件新 → 跳过", nm, v);
            continue;
        }
        snprintf(s_tf_path, sizeof(s_tf_path), "%s", path);
        /* ★同上：v 是 16B 缓冲、S.new_ver 也是 16B，加 "v" 前缀会超 → 用 %.*s 限长 */
        snprintf(S.new_ver, sizeof(S.new_ver), "v%.*s", (int)sizeof(S.new_ver) - 2, v);
        S.size_kb = (int)(sz / 1024);                  /* ★真值：bin 实际字节 */
        newer = true;
        ESP_LOGI(TAG, "卡上 %s = v%s（%ld KB）→ 可升级", nm, v, sz);
        break;                                        /* 一张卡放一个包：取第一个更新的即可 */
    }
    closedir(d);

    if (!newer) {
        if (nbin == 0) {
            ESP_LOGW(TAG, "卡根目录没有任何 .bin 固件（拷了文件夹不算——文件要在卡根目录）");
            set_fail("卡上没有固件文件，拷到根目录");
            return;
        }
        ESP_LOGI(TAG, "卡上的 bin 都不比本固件新（共 %d 个）→ 已是最新", nbin);
        S.st = OTA_NEWEST;
        return;
    }
    S.pct = 0;
    S.st = OTA_FOUND;                 /* 卡保持挂载，等用户确认后直接读 bin */
}

/* ══════════════════════════════════════════════════════════════════════
 * ① TF卡：把 firmware bin 写进 OTA 分区
 * ══════════════════════════════════════════════════════════════════════ */
/* ★ 2026-10-19 重构：把「文件 → OTA 分区」抽成共用（TF 直读 / 网页中转共用）。
   硬约束全保留：电量由调用方预检；取消=abort 旧固件照常；喂狗；校验不过不切分区。
   写前加两道廉价防线：① 文件非空 ② 文件头 magic 0xE9（esp_image_header_t）——
   「传了个 txt / 半截包」在**写 ota_0 之前**就被拦下，不会污染固件区。 */
static bool flash_from_file(const char *path)
{
    if (!s_mounted && !ota_sd_mount()) { set_fail("未检测到TF卡"); return false; }

    FILE *f = fopen(path, "rb");
    if (!f) { set_fail("找不到固件文件"); return false; }

    uint8_t m[4];
    if (fread(m, 1, 4, f) != 4 || m[0] != 0xE9) {
        fclose(f); set_fail("固件文件头无效"); return false;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); set_fail("固件文件是空的"); return false; }

    if (!local_ota_begin((int)sz)) { fclose(f); return false; }
    S.pct = 0;
    ESP_LOGI(TAG, "%s → 写 OTA 槽 %s（%ld B）", path, s_tgt->label, sz);

    uint8_t *chunk = malloc(4096);
    if (!chunk) { local_ota_abort(); fclose(f); set_fail("内存不足"); return false; }

    /* ★ 2026-10-20 清理：此处原有一个 `int last_pct = -1;` 与
       `if (S.pct >= last_pct + 5) last_pct = S.pct;`（注释写「节流：别按字节刷 UI」）。
       last_pct 只被自己写、从未被任何判据读 ⇒ 死变量；且本函数整个循环**没有任何 UI 刷新**
       （它跑在独立任务里只写 OTA 分区），所谓"节流"既没实现也没必要 ⇒ 注释与代码不符，
       容易误导后来者以为这里已做过节流。进度节流应在**调用方的 UI 轮询**里做。 */
    while (1) {
        if (s_cancel) {                /* 取消 = abort，旧固件照常 */
            ESP_LOGW(TAG, "用户取消 → 中止（旧固件照常）");
            free(chunk); fclose(f); local_ota_abort();
            S.st = OTA_IDLE; return false;
        }
        esp_task_wdt_reset();          /* ③ 喂狗 */
        size_t r = fread(chunk, 1, 4096, f);
        if (r == 0) { if (ferror(f)) { free(chunk); fclose(f); local_ota_abort(); set_fail("读卡出错"); return false; } break; }   /* ★2026-10-23 修：区分 EOF 与读错误，别把 SD IO 错误当正常收尾（否则上屏误报"固件校验没通过"） */
        if (!local_ota_feed(chunk, r)) { free(chunk); fclose(f); local_ota_abort();
                                         set_fail("写入失败"); return false; }
    }
    free(chunk); fclose(f);
    return local_ota_finalize();       /* 魔数+SHA 校验不过/切分区失败 → 内部置 FAILED */
}

static void do_flash_from_sd(void)
{
    if (ota_batt_low()) { set_fail("电量太低，充好再升级"); return; }
    if (!s_tf_path[0]) { set_fail("没有可升级的固件"); return; }   /* ★防御：没选中就别写分区 */
    if (!s_mounted && !ota_sd_mount()) { set_fail("未检测到TF卡"); return; }
    flash_from_file(s_tf_path);          /* ★2026-10-19：用扫描时选中的那个文件（不再写死文件名） */
    if (s_mounted) ota_sd_unmount();
}


/* ══════════════════════════════════════════════════════════════════════
 * ② 网页上传：net_prov.c 的 /upgrade 路由调下面四个函数
 *    （begin → 循环 feed → finalize；任意一步失败 → abort）
 *    这几个函数**同步**在 http 处理任务里跑（不另起任务），写完即切分区。
 * ══════════════════════════════════════════════════════════════════════ */
/* ★★ 2026-10-19（偏移按 bin 实测订正）：上传流的前 0x50 字节就是 app image 头部 ——
   app_desc 结构体在**文件偏移 0x20**（24B esp_image_header + 8B 段头），结构体里
   magic 在 +0（= 0x20）· version[32] 在 +16（= **0x30**）· project_name[32] 在 +48（= 0x50）。
   凑够 0x50 字节即可解析真版本号覆盖占位名，上屏的「已升级到 vX」才是 bin 里的真值。
   ⚠ 旧值 0x30/0x3A（把结构体偏移当成了"段内偏移"）是**错的** —— bin dump 逐字节核对：
     magic 0xABCD5432 在 0x20、version 在 0x30、project 在 0x50。HANDOVER 记着 C30 修过，
     但本文件里一直是旧值（那次修复没落地）⇒ 网页路版本号此前一直显示占位名「固件包」。 */
#define APP_DESC_OFF      0x20          /* app_desc 结构体在文件里的偏移 */
#define APP_DESC_VER_OFF  0x30          /* version[32]（结构体 +16） */
#define HDR_NEED          0x50          /* 0x30+32 = 0x50 */
static uint8_t s_hdr[HDR_NEED];
static int     s_hdr_n      = 0;
static bool    s_hdr_parsed = false;

bool ota_web_begin(const char *label, int size)
{
    if (s_busy && !s_web) { set_fail("正在升级中，请稍候"); return false; }   /* ★2026-10-23 修（M15）：TF卡刷写在跑时拒绝网页升级，避免两个写入方共用同一 OTA 句柄 */
    s_web = true;
    s_busy = true;
    s_cancel = false;
    S.reason[0] = 0;
    /* 先给占位名；s_hdr 凑够头部后由 esp_app_desc 的真版本号覆盖（见 web_hdr_parse） */
    /* ★ 2026-10-20：补精度限长。S.new_ver 是 char[16]（ota_up.h），label 是调用方传入的
       任意长度串 ⇒ "%s" 会静默截断（可能截出半个词）。同文件另两处写 S.new_ver 的地方
       （267 行 "v%.*s"、401 行 "%.15s"）都做了限长，只有这条漏了。 */
    snprintf(S.new_ver, sizeof(S.new_ver), "%.15s", (label && label[0]) ? label : "固件包");
    s_hdr_n = 0;
    s_hdr_parsed = false;
    S.pct = 0;
    S.st = OTA_DOWNLOADING;
    if (ota_batt_low()) { set_fail("电量太低，充好再升级"); s_busy = false; s_web = false; return false; }

    /* ★★★ 2026-10-19 用户令（双槽恢复后）：网页升级**直写非运行槽 ota_1**，不再落 TF卡中转。
       当年「整包先落 TF卡校验 + 无卡弹变砖风险」是**单槽时代**的防变砖设计（怕上传中断
       把唯一的固件槽写坏）。现在写的是**非运行槽** ⇒ 中断/断电都不变砖（otadata 仍指 ota_0，
       重启照旧跑老固件）⇒ TF 中转文件、/tf 预检、风险确认层**整套删除**（页面不再需要插卡）。 */
    if (!local_ota_begin(size)) { s_busy = false; s_web = false; return false; }
    ESP_LOGW(TAG, "★ 网页升级直写非运行槽（不再落卡），期望 %d B", size);
    return true;
}

static void web_hdr_parse(void)
{
    s_hdr_parsed = true;
    uint32_t magic = (uint32_t)s_hdr[APP_DESC_OFF] |
                     ((uint32_t)s_hdr[APP_DESC_OFF + 1] << 8) |
                     ((uint32_t)s_hdr[APP_DESC_OFF + 2] << 16) |
                     ((uint32_t)s_hdr[APP_DESC_OFF + 3] << 24);
    if (magic != 0xABCD5432u) {
        ESP_LOGW(TAG, "★ app_desc 魔数不对（%08lx）→ 版本号显示占位名", (unsigned long)magic);
        return;
    }
    char v[33];
    memcpy(v, s_hdr + APP_DESC_VER_OFF, 32); v[32] = 0;
    for (char *p = v; *p; ++p) { if (*p == ' ' || *p == '\n' || *p == '\r') { *p = 0; break; } }
    if (v[0]) { snprintf(S.new_ver, sizeof(S.new_ver), "%.15s", v);   /* new_ver[16]，精度限长消 -Werror=format-truncation */
                ESP_LOGI(TAG, "★ 上传固件内嵌版本 = %s", v); }
}

bool ota_web_feed(const uint8_t *data, size_t len)
{
    /* ★2026-10-19：只有一种模式了 —— 直写**非运行槽**（见 ota_web_begin 注释） */
    if (S.st != OTA_DOWNLOADING) return false;
    if (s_ota_h == 0) return false;
    if (s_cancel) { S.st = OTA_IDLE; s_busy = false; return false; }  /* 收尾统一走 ota_web_abort */
    esp_task_wdt_reset();
    if (!s_hdr_parsed) {
        while (s_hdr_n < HDR_NEED && len > 0) { s_hdr[s_hdr_n++] = *data++; --len; }
        if (s_hdr_n >= HDR_NEED) {
            /* ★★★ 2026-10-19 修根因：这 0x50 字节是从流里**摘出来**做样本的，必须**原样补写回槽**！
               否则镜像从 0x50 开始写 → esp_ota_write 校验首字节不是 0xE9（实测 saw 0x70 = bin[0x50]
               的 'p'）→ ESP_ERR_OTA_VALIDATE_FAILED → 上屏「上传中断」（网页路必现，TF 路无此步不受影响）。 */
            web_hdr_parse();
            if (!local_ota_feed(s_hdr, (size_t)s_hdr_n)) { ESP_LOGE(TAG, "补写头部失败"); return false; }
        }
    }
    if (len == 0) return true;
    if (!local_ota_feed(data, len)) { ESP_LOGE(TAG, "直写非运行槽失败"); return false; }
    return true;                       /* 进度由 local_ota_feed 内部按 s_exp_size 更新 */
}

bool ota_web_finalize(void)
{
    bool ok = false;
    if (s_ota_h) {
        ok = local_ota_finalize();     /* esp_ota_end 校验（不过不切分区）+ 切到非运行槽 → DONE */
    } else {
        set_fail("上传流异常，已取消");
    }
    s_busy = false;
    s_web  = false;                    /* ★R119 修：网页源结束必须清 s_web，
                                          否则 ota_start_download() 的 `if (s_web) return;`
                                          会永久挡住之后的 TF卡升级 */
    return ok;
}

void ota_web_abort(void)
{
    /* ★ 中断只丢弃本次写：otadata 未被触碰 ⇒ 重启照旧跑老固件（**不变砖**，双槽的意义所在） */
    local_ota_abort();
    set_fail("上传中断，已取消");
    s_busy = false;
    s_web  = false;                    /* ★R119 修：同上，abort 也要归位 */
}

/* ★2026-10-19：原 `ota_web_tf_ready()`（页面 /tf 预检用的只读探卡）随「落卡中转」一并删除 ——
   网页升级现在直写非运行槽，不需要卡，也不需要预检。 */


/* ══════════════════════════════════════════════════════════════════════
 * ★R218：在线升级回调（ota_net.c 调，复用本文件状态机与 local_ota_* 写槽）
 * ══════════════════════════════════════════════════════════════════════ */
void ota_net_newest(void)  { S.st = OTA_NEWEST; }
void ota_net_found(const char *ver, int size_kb)
{
    snprintf(S.new_ver, sizeof(S.new_ver), "%.15s", ver);
    S.size_kb = size_kb;
    S.st = OTA_FOUND;
    ESP_LOGW(TAG, "★ 在线发现新版 %s（%d KB）", ver, size_kb);
}
void ota_net_fail(const char *reason) { set_fail(reason); }
void ota_net_done(void) { S.pct = 100; S.st = OTA_DONE; }
void ota_net_progress(int pct) { S.pct = (pct > 100) ? 100 : pct; }
bool ota_net_begin(int size) { return local_ota_begin(size); }
bool ota_net_feed(const uint8_t *d, size_t n) { return local_ota_feed(d, n); }
bool ota_net_finalize(void) { return local_ota_finalize(); }
void ota_net_abort(void)
{
    if (s_ota_h) { esp_ota_abort(s_ota_h); s_ota_h = 0; }
    s_tgt = NULL;
}

/* ══════════════════════════════════════════════════════════════════════
 * 任务入口（TF卡检查 / 写入 + 在线检查 / 下载走任务；网页上传同步跑）
 * ══════════════════════════════════════════════════════════════════════ */
typedef enum { JOB_CHECK_SD, JOB_FLASH_SD, JOB_CHECK_NET, JOB_FLASH_NET } ota_job_t;   /* ★R218：+在线两任务 */

#define OTA_TASK_STACK  6144
#define OTA_TASK_PRIO   5

static void ota_task(void *arg)
{
    ota_job_t job = (ota_job_t)(intptr_t)arg;
    if      (job == JOB_CHECK_SD) do_check_sd();
    else if (job == JOB_FLASH_SD) do_flash_from_sd();
    else if (job == JOB_CHECK_NET) ota_net_check();     /* ★R218：在线检查清单 */
    else if (job == JOB_FLASH_NET) ota_net_download();  /* ★R218：在线下载写槽 */
    s_busy = false;
    ESP_LOGI(TAG, "任务结束（状态 %d）", (int)S.st);
    vTaskDelete(NULL);
}

static void ota_launch(ota_job_t job)
{
    if (s_busy) return;
    s_busy = true;
    s_cancel = false;
    s_web = false;
    if (xTaskCreate(ota_task, "ota_up", OTA_TASK_STACK, (void *)(intptr_t)job,
                    OTA_TASK_PRIO, NULL) != pdPASS) {
        s_busy = false;
        set_fail("起任务失败");
    }
}

void ota_net_start_check(void)
{
    if (s_busy) { set_fail("正在升级中"); return; }
    ota_reset();
    S.st = OTA_CHECKING;
    /* 在线检查要联网：若当前没连 WiFi，先让主循环补同步连上再点（UI 层面提示） */
    ota_launch(JOB_CHECK_NET);
}
void ota_net_start_download(void)
{
    if (s_web) return;
    S.st = OTA_DOWNLOADING;
    ota_launch(JOB_FLASH_NET);
}


void ota_start_check(void)
{
    if (s_busy) { set_fail("正在升级中"); return; }   /* ★2026-10-23 修：busy 时别 reset+置 CHECKING（否则 reset 卸卡与在跑任务竞态，且状态卡"检查中"） */
    ota_reset();
    S.st = OTA_CHECKING;
    ota_launch(JOB_CHECK_SD);
}

void ota_start_download(void)
{
    if (s_web) return;                 /* 网页源不走这里 */
    if (!s_mounted && S.st != OTA_FOUND) { set_fail("没有可升级的固件"); return; }
    S.st = OTA_DOWNLOADING;
    ota_launch(JOB_FLASH_SD);
}

void ota_cancel(void)
{
    s_cancel = true;                   /* 任务里判到就 abort；检查中同理 */
    if (!s_busy) { S.st = OTA_IDLE; s_cancel = false; }
}

void ota_reset(void)
{
    memset(&S, 0, sizeof(S));
    S.st = OTA_IDLE;
    s_cancel = false;
    s_demo = false;
    s_web = false;
    /* ★ 2026-10-20 补：s_tf_path 是"把哪个文件写进 OTA 分区"的唯一依据（do_check_sd 写入、
       do_flash_from_sd 读取），但它原本不在 reset 的清零名单里 ⇒ 上一轮选中的固件路径
       会残留。目前只是靠 ota_start_download() 的 `!s_mounted` 前置**间接**挡住
       （那是"挂载状态"而非"有没有选中文件"，属巧合不是设计）。清掉它，让
       do_flash_from_sd() 里 `if (!s_tf_path[0])` 成为真正自洽的守卫。 */
    s_tf_path[0] = 0;
    /* 离开 OTA 流程 → 卸卡（省电 + 防误触） */
    if (s_mounted) ota_sd_unmount();
}

const ota_status_t *ota_status(void) { return &S; }

/* ★★ 2026-10-17 补：新固件自检通过 → 确认（取消回滚）。
   sdkconfig 开了 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE ⇒ OTA 起的新固件处于
   ESP_OTA_IMG_PENDING_VERIFY；**不确认就等下一次复位时被 bootloader 回滚**。
   调用点 = 两版 app_main 的首屏渲染之后（能刷出主页 = 基本自检通过）。
   只有真处于 PENDING_VERIFY 才动作 —— 普通上电（VALID）走不到，可无条件调。 */
void ota_confirm_boot_ok(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (!run) return;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;      /* 普通上电：什么都不做 */
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "★ OTA 新固件自检通过 → 已确认（不再回滚）rc=%d", (int)e);
}

/* ── 巡检（测试菜单用）：只摆状态与示例值，**不联网 / 不读卡 / 不写分区** ────── */
bool ota_is_demo(void) { return s_demo; }

void ota_debug_set(int stage)
{
    ota_reset();
    s_demo = true;
    switch (stage) {
    case 0: S.st = OTA_CHECKING; break;
    case 1:
        S.st = OTA_FOUND;
        snprintf(S.new_ver, sizeof(S.new_ver), "v6.3");
        S.size_kb = 2400;
        break;
    case 2: S.st = OTA_NEWEST; break;
    case 3: S.st = OTA_DOWNLOADING; S.pct = 62; break;
    case 4: S.st = OTA_DONE;  snprintf(S.new_ver, sizeof(S.new_ver), "v6.2"); S.pct = 100; break;
    default: S.st = OTA_FAILED; snprintf(S.reason, sizeof(S.reason), "%s", "网络中断"); break;
    }
    ESP_LOGW(TAG, "★ 巡检模式：摆出第 %d 页状态（st=%d，**不读卡不写分区**）", stage, (int)S.st);
}
