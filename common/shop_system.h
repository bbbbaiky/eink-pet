/* shop_system.h —— 商店系统（每日货架/购买/售罄/确认页） 公开接口（重构 2a 生成） */
#ifndef SHOP_SYSTEM_H
#define SHOP_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
#include "loot_data.h"
#include "main_api.h"

bool shop_buy(int k);
bool shop_is_sold(int k);
/* ★R218：暴露给商店页做「图标分流」判据 —— 虚拟消耗品必须走 shop_item_icon()，
   不能按 item<N_LOOT 当真收藏品画 g_loot[item]（104 小鱼饼干会被画成火山石）。 */
bool shop_is_consumable(const shop_pool_t *it);
bool shop_owned(const shop_pool_t *it);
const char *shop_note(const shop_pool_t *it);
const shop_pool_t *shop_item(int k);
int shop_item_icon(int item);
void shop_btn_band(const char *txt);
void shop_conf_btn(int which, const char *txt, bool sel);
void shop_next(void);
void shop_open(void);

#endif
