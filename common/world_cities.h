#pragma once
/* ---------------------------------------------------------------------------
 * 海外城市解耦查找（2026-10-14 · 海外城市国际化 P2；2026-10-14 加繁体列 zht）
 *   键 = "CC:CityId"（海外，如 "US:NewYorkCity"）-> 精确经纬度 / 四语名 / 国家码。
 *   中国城市键 = 中文名（如 "河北省石家庄市"），不在此表 => wc_find 返回 NULL，
 *   调用方据此走 city_coords.h 旧路（中国零回归）。
 *   数据表 WORLD_CITIES[] / N_WORLD_CITIES 由 scripts/gen_world_cities.py 生成到
 *   world_cities_data.h，仅 net_sync.c 包含（一份副本）；wc_find 定义在 net_sync.c。
 * --------------------------------------------------------------------------- */
typedef struct {
    const char *key;   /* 稳定键 "CC:CityId" */
    const char *en;    /* 英文名（GeoNames） */
    const char *ja;    /* 日文名（Open-Meteo，无则回退 en） */
    const char *zh;    /* 简体中文名（Open-Meteo zh + OpenCC 繁转简，无则回退 en） */
    const char *zht;   /* 繁體中文名（Open-Meteo zh 原始串即繁体，无则回退 en）★ 2026-10-14 */
    float lat, lon;    /* GeoNames 权威经纬度 */
    const char *cc;    /* 国家码，如 "US" */
} wc_entry_t;

/* 返回 NULL = 非海外键（中国城走旧路）。定义在 net_sync.c。 */
const wc_entry_t *wc_find(const char *key);
