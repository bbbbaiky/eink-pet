/* 自动生成，勿手改 —— 重跑 scripts/_tools/_gen_loot_append_1016.py（2026-10-16 扩编）
 * 收藏品详情页的大图标，88×88 1bpp（bit 7 在左，1 = 黑），与 ui.bin 同一打包约定。
 * 画在 (LOOT_ICON_L, LOOT_ICON_T) = (56, 42) —— 详情页图标槽；
 * 固件侧 fb_blit_loot_icon(i) 会**整块覆盖**该槽（含底色）。
 * 张数：76 件收藏品（顺序与 main.c 的 g_loot[] 一致）+ 7 件消耗品单独一张表。
 *   前 21 张 = 旧头文件逐字节保真；后 43 张 = 2026-10-16 AI 铜版蚀刻（4×4 网格切分）。
 *   5 件消耗品（猫薄荷/小鱼饼干/提神草/晴天符/顺风符）**不进 g_loot[]**，
 *   留给「小卖部 / 背包」页（有数量 ×N，不是收藏品）。
 */
#pragma once
#include <stdint.h>

#define N_LOOT_ICON  178   /* ★2026-10-24 B7：172 + 6 环境件（图标待我方，bin 172..177 空白占位；消耗品 178..184） */   /* ★2026-10-25 阶段1：128 + 44（消耗品 7 张仍在 bin 尾 172..178） */
#define LOOT_ICON_S  88
#define LOOT_ICON_BYTES_PER_ICON  968



/* 消耗品图标 7 件（不进 g_loot[]，无 owning 概念） */
#define N_ITEM_ICON  7


/* 图标张数必须与 main.c 的 g_loot[] 相等（错位会显示成别的收藏品） */
#if defined(N_LOOT)
_Static_assert(N_LOOT == N_LOOT_ICON, "g_loot[] 与 loot_icons.h 张数不一致");
#endif
