/* ══════════════════════════════════════════════════════════════════════
 * OTA 本地升级（fw/common/ota_up.h）—— 2026-10-19 改版
 * ══════════════════════════════════════════════════════════════════════
 * 放弃在线 OTA，改两个**本地**升级源（详见 ota_up.c 文件头）：
 *   ① TF 卡：点「检查版本」→ 读卡比版本 → 发现新版则读 bin 写 OTA 分区。
 *   ② 网页上传：net_prov.c /upgrade 路由收文件 → 调 ota_web_* 写 OTA 分区。
 *
 * 交互/失败口径与 main.c 的 26~31 页完全不变（状态机照旧驱动）。
 *
 * ★ 硬约束（永远是这三条）：
 *   ① 校验不过**绝不切启动分区**（esp_ota_end 内部做 image 校验）；
 *   ② 取消 / 失败 / 拔卡 → **旧固件照常**（不写 otadata = 还在原槽）；
 *   ③ 写入中**每循环喂一次看门狗**，别让 TG0WDT 把板子拽重启。
 * ══════════════════════════════════════════════════════════════════════ */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_IDLE = 0,      /* 没在跑（初始 / 已收尾） */
    OTA_CHECKING,      /* 26 正在检查 */
    OTA_FOUND,         /* 27 发现新版本（等用户按「下载并升级」） */
    OTA_NEWEST,        /* 28 已是最新 */
    OTA_DOWNLOADING,   /* 29 正在下载并写入另一槽 */
    OTA_DONE,          /* 30 校验通过 + 已切分区 → 等 3 秒自动重启 */
    OTA_FAILED,        /* 31 失败（看 reason） */
} ota_state_t;

typedef struct {
    ota_state_t st;
    int         pct;               /* 0..100（仅 OTA_DOWNLOADING 有意义） */
    char        new_ver[16];       /* 新版本号（两条路都取 bin 内嵌 esp_app_desc；TF 路 = 卡根 *.bin） */
    int         size_kb;           /* 安装包大小（KB；TF 路 = 文件实际字节 / 网页路 = 实收字节） */
    char        reason[48];        /* 失败原因（上屏用）。★2026-10-19 字库实测：**「，」正文档+标题档都有**，
                                    所以子句之间统一用「，」；而「。！？、」字库没有 → 继续避开；短语内部不留空格 */
} ota_status_t;

/* 启动一次「检查更新」（异步：内部起任务，UI 轮询 ota_status() 看状态）。 */
void ota_start_check(void);

/* 27 页按「下载并升级」→ 真正下载。前置检查（电量 / 空间 / WiFi）在这一步做，
   不通过直接置 OTA_FAILED 并把原因写进 reason（方案 §五：别等下载一半才失败）。 */
void ota_start_download(void);

/* 取消：检查中 = 放弃本次检查；下载中 = abort（旧固件照常）。 */
void ota_cancel(void);

/* 只读状态指针（UI 每次渲染取一次）。 */
const ota_status_t *ota_status(void);

/* 回到 OTA_IDLE（进页面前置一次，避免上一次的失败态被带进来）。 */
void ota_reset(void);

/* ★★ 2026-10-17 补：**新固件自检通过 → 确认，取消回滚**（app_main 首屏渲染之后各调一次）。
   为什么必须有：sdkconfig 开了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` —— 新固件起来后
   处于 PENDING_VERIFY，**不在下一次复位前确认，bootloader 就把 otadata 切回旧槽**
   （表现 =「升级成功、重启后还是旧版本」，最难查的一类 OTA bug）。
   本函数只在真的处于 PENDING_VERIFY 时才动作，普通上电是 no-op，可无条件调用。 */
void ota_confirm_boot_ok(void);

/* ★ 巡检用（测试菜单「OTA页面巡检」）：**服务端还没定时**，靠它把六页逐页摆出来验视觉。
   stage 0..5 = 检查中 / 发现新版 / 已是最新 / 下载中 / 完成 / 失败。
   ⚠ 只动**状态机与示例值**，不起任何读卡/下载/写分区动作。
   ⚠ 置位后 `ota_is_demo()` 为真 —— 结果页的「3 秒自动重启」会被跳过（巡检不能真重启）。 */
void ota_debug_set(int stage);
bool ota_is_demo(void);

/* ── ③ TF 卡公共层（★★2026-10-22 抽出来，mypost 满仓转存与 OTA 共用）────────
   挂载态带引用计数：ota_sd_mount() 已挂载 → ref+1 直接返 true；ota_sd_unmount()
   ref-1，归零才真卸卡。httpd 任务（收图回调）与 OTA 任务都可能调 ⇒ 内部串行化。
   ⚠ ota_sd_init() 只需在 app_main 里调一次（建信号量）；不调则首次用时懒初始化。 */
void ota_sd_init(void);
bool ota_sd_mount(void);            /* 挂载或 ref+1；失败（无卡/未格式化）= false */
void ota_sd_unmount(void);          /* ref-1；归零卸卡；ref=0 时调 = no-op */
bool ota_sd_ready(void);            /* 只看当前挂载态，不尝试挂载（探卡用）*/
const char *ota_sd_mount_point(void);   /* "/sdcard" */

/* ── ② 网页上传接口（net_prov.c 的 /upgrade 路由调用）─────────────────
   begin → 循环 feed → finalize；中途失败 → abort。三个函数**同步**在 http 处理
   任务里跑（不另起任务），写完即切分区。 */
/* ★★★ 2026-10-19（双槽恢复后 · 用户令）：网页升级**直写非运行槽**，不再需要 TF 卡。
   当年「整包先落 TF 卡中转校验 + 无卡弹变砖风险」是**单槽时代**的防变砖设计；现在写的是
   非运行槽 ⇒ 上传中断/断电都不变砖（otadata 仍指老固件所在的槽）⇒ 中转、/tf 预检、
   风险确认层**整套删除**（连带 risk_ok 参数与 ota_web_tf_ready()）。 */
bool ota_web_begin(const char *label, int size);    /* 准备写分区（size=期望字节，0=未知） */
bool ota_web_feed(const uint8_t *data, size_t len);/* 喂一块（≤4KB 为宜） */
bool ota_web_finalize(void);                        /* 收尾：校验 + 切分区 → DONE */
void ota_web_abort(void);                           /* 中止（旧固件照常） */

#ifdef __cplusplus
}
#endif
