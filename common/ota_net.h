/* ota_net.h —— 在线升级（R218）：由 ota_up.c 实现的回调 + 给 main.c 的入口 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 给 main.c：在线检查 / 下载（异步任务在 ota_up.c 里起，状态走 ota_status()） */
void ota_net_start_check(void);
void ota_net_start_download(void);

/* 由 ota_up.c 实现，ota_net.c 调用：状态机 + 写分区回调 */
void ota_net_check(void);
void ota_net_download(void);
void ota_net_newest(void);
void ota_net_found(const char *ver, int size_kb);
void ota_net_fail(const char *reason);
void ota_net_done(void);
void ota_net_progress(int pct);
bool ota_net_begin(int size);
bool ota_net_feed(const uint8_t *data, size_t len);
bool ota_net_finalize(void);
void ota_net_abort(void);

#ifdef __cplusplus
}
#endif
