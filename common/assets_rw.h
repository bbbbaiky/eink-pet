/* ══════════════════════════════════════════════════════════════════════════════
 * assets_rw.h —— assets 数据分区**读取层**（C1 拆分前置；2026-11-11 从 main.c 搬出）
 * ------------------------------------------------------------------------------
 * 原来这段（a_rd32 / 区域目录 / 影子缓冲 / 区域相对读 / 解压）埋在 main.c 里，
 * 它是所有内容装载器的**共同底座**：字库、明信片、猫、窗、房、动画、UI 容器……
 * 拆成独立 TU 后，后续抽 fb/绘制原语、抽 draw_* 时不必再把 main.c 的静态量拖来拖去。
 *
 * **行为逐字不变**（纯搬运）：区域目录由开机 assets_boot_check() 调
 * assets_dir_set() 填充；压缩区（'RLZ1'）先解到 PSRAM，assets_region_read()
 * 优先从影子 memcpy ⇒ 上层调用一行都不用改。
 *
 * 失败语义（与 pack_assets.py 的布局真源一致）：
 *   分区缺失 / 区域未登记 / 越界 → 一律 false，调用方各自兜底，绝不越界。
 * ════════════════════════════════════════════════════════════════════════════════ */
#ifndef ASSETS_RW_H
#define ASSETS_RW_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "assets.h"     /* AID_* / ASSETS_* / assets_read() */

/* 区域级压缩容器魔数（头 8B 一律 = magic + u32 解压后原长，见 pack_assets.py）
 *   'RLZ1' R182 老格式：RLE1 流。★新包已不产出，但**必须保留解码**：
 *          网页 OTA 只写 app 不写 assets，新固件要能读设备上的旧 assets。
 *   'ZLZ1' R196 新格式：zlib(deflate) 流，由 ESP32-S3 ROM 的 tinfl 解。
 *   其它 magic（含 UIRL / ZFNT / I18N / RLE1 等自带容器）= 该区未压缩，直读 flash。 */
#define RLZ_MAGIC   0x315A4C52u
#define ZLZ_MAGIC   0x315A4C5Au
/* 帧级压缩流魔数（'RLE1'：动画容器每帧的编码，见 scripts/anim_rle.py）*/
#define RLE1_MAGIC  0x31454C52u

/* 按字节拼 u32（xtensa 不保证非对齐访问，全工程统一走它）*/
uint32_t a_rd32(const uint8_t *p);

/* 开机登记一个区域的 off/len（由 assets_boot_check 的目录遍历调用）*/
void assets_dir_set(int id, uint32_t off, uint32_t len);

/* 取区域 off/len。len 对压缩区返回**解压后长度**（调用方看到的仍是原始语义）*/
bool assets_region(int id, uint32_t *off, uint32_t *len);

/* 区域内**相对偏移**读（带边界；压缩区走 PSRAM 影子）*/
bool assets_region_read(int id, uint32_t rel, void *dst, size_t n);

/* 把所有压缩区（'ZLZ1' 或旧 'RLZ1'）整区解到 PSRAM 影子。
 * **非致命**：某区解不开就跳过（该区退回直读 flash，上层各自兜底）。
 * 上层 assets_region_read() 一律优先 memcpy 影子 ⇒ 画面/字库/UI/动画代码零改动。 */
void assets_shadow_load_all(void);

/* RLE1 解压：成功（解出字节数正好 == dst_len）返回 0，否则 -1*/
int rle1_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

#endif /* ASSETS_RW_H */
