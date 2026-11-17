/* ══════════════════════════════════════════════════════════════════════════════
 * assets.h —— **assets 数据分区**读取（2026-10-16 拆分重构 · 方案文档 §8.6/§11）
 * ------------------------------------------------------------------------------
 * 为什么有它：原来所有内容（明信片/UI/字库/图标/动画/猫姿/窗/房/食盆/贴片）都用
 *   EMBED_FILES 内嵌进 app ⇒ A/B 两个 OTA 槽各存一份（≈1.9MB 纯重复），且明信片
 *   继续长会顶大 app、把两个槽一起顶大。现在所有**内容**搬进独立 assets 分区
 *   （单份），app 只留纯代码 ⇒ 明信片可单独 OTA，不加固件。
 *
 * 分区内布局（**唯一真源 = scripts/_tools/pack_assets.py**；改布局必同步改那里）：
 *   0x000000 .. 0x0FFF  分区头 4KB：魔数 + 版本 + 目录 + CRC32（见下）
 *   0x001000 ..          各区域（4KB 对齐），布局由**目录**描述，固件不写死偏移
 *
 * 分区头格式（全 LE）：
 *   0x00 u32 magic 'AATS'   0x04 u32 version   0x08 u32 n_entries
 *   0x0C u32 payload_crc32  0x10 u32 payload_end
 *   0x18 起 目录 n × {u32 id, u32 off, u32 len}
 *
 * ⚠ 读失败（没迁移 / 分区坏 / 区域缺失）→ 一律返回 false，调用方画空框/画白，
 *   **不许崩**。开机由 main.c::assets_boot_check() 统一校验（魔数 + 全量 CRC +
 *   各区域长度自检），不过则显示「资产缺失」页停住（见方案文档 §11 ⑧）。
 * ⚠ 区域 ID 的顺序必须与 pack_assets.py 的 REGIONS 一致（启动时会打串口日志核对）。
 * ════════════════════════════════════════════════════════════════════════════════ */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_partition.h"

#define ASSETS_MAGIC       0x53544141u   /* "AATS" */
#define ASSETS_VER         1             /* 分区头版本（pack_assets.py 同值）*/
#define ASSETS_HDR_LEN     0x1000        /* 分区头长度；内容从 0x1000 起 */

/* 区域 ID —— 顺序 = pack_assets.py REGIONS 顺序，**两边必须同步** */
enum {
    AID_POST = 0,      /* 明信片照片 N × 2268B（164×108 1bpp，行 21B）*/
    AID_UI_TOUCH,      /* 触屏 UI RLE 容器（'UIRL'）*/
    AID_UI_BTN,        /* 按键 UI RLE 容器（'UIRL'）*/
    AID_LOOT,          /* 藏品+消耗品图标 83 × 968B（88×88 1bpp，行 11B）*/
    AID_ZFONT,         /* 字库（'ZFNT'：两张 ASCII 表 + 两张 CJK 表 + 两张码点索引）*/
    AID_ANIM_REJECT,   /* 交互动画 RLE1 容器 ×8（anim_reject + anim1..7）*/
    AID_ANIM1, AID_ANIM2, AID_ANIM3, AID_ANIM4, AID_ANIM5, AID_ANIM6, AID_ANIM7,
    AID_CAT,           /* 猫精灵仓：u32 数量 + u32 偏移表 + 精灵（8B 头 + S/W/SH 三层）*/
    AID_WIN,           /* ★R202 退役：窗外改「天气层+风景层」，固件不再读取（区里是 16B 哨兵）*/
    AID_ROOM,          /* ★R202 退役：房间改 room_plain/room_tech + 窗洞掩膜（16B 哨兵）*/
    AID_OFF,           /* ★R205 关机页 **两页 × 5000B**（A 旧图 / B 新图，power_off 轮替）*/
    AID_BOWL,          /* 食盆贴片仓（同 cat 容器格式，A/K 两层）*/
    AID_PATCH,         /* 状态贴片仓（同 cat 容器格式，A/K 两层）*/
    AID_ICON,          /* 功能页图标 11 × 231B（56×33，行 7B）*/
    AID_MOON,          /* 月相贴片 8 × 108B（27×27，1=亮面）*/
    AID_I18N,          /* i18n 三语表（'I18N'：zh 键 + EN/JA/ZHT 翻译，UTF-8 池）*/
    AID_WEBUI,         /* ★2026-10-26 重构 2e：web 资源仓（10 段 HTML：配网名页×4/上传页/升级页/API 页×4）*/
    AID_ROOM_TECH,     /* ★R194 科技房间：4 × 5000B 底图（1=白）+ 5000B 掩膜（1=画，0=透出窗景）*/
    AID_WIN_TECH,      /* ★R202 退役：同 AID_WIN（16B 哨兵）*/
    AID_ROOM_PLAIN,    /* ★R194 默认房间（挖空版）：4 × 5000B + 5000B 掩膜（1=画）*/
    AID_LAND_WEATHER,  /* ★R195 天气层（最底，整屏通用）：4 时段 × 7 天气 = 28 × 5000B
                          （与地标无关：窗洞/房间/风景怎么换都不用重画）*/
    AID_LAND_BASE,     /* ★R194 风景层：12 × 5000B（3 地标 × 4 时段）+ 3 × 5000B 掩膜 */
    AID_N
};

static inline const esp_partition_t *assets_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, "assets");
}

/* 分区内**绝对偏移**读（区域读走 main.c 的目录表，别直接用这个）*/
static inline bool assets_read(uint32_t off, void *dst, size_t n)
{
    const esp_partition_t *p = assets_part();
    if (!p || !dst) return false;
    return esp_partition_read(p, off, dst, n) == ESP_OK;
}

/* ★2026-10-20 用户明信片仓 mypost：assets 分区内**运行期**写/擦（mypost 预留区专用，
   不经 AATS 目录、不在开机 CRC 覆盖内）。任何失败返回 false。 */
static inline bool assets_write(uint32_t off, const void *src, size_t n)
{
    const esp_partition_t *p = assets_part();
    if (!p || !src) return false;
    return esp_partition_write(p, off, src, n) == ESP_OK;
}
static inline bool assets_erase(uint32_t off, size_t n)
{
    const esp_partition_t *p = assets_part();
    if (!p) return false;
    return esp_partition_erase_range(p, off, n) == ESP_OK;
}
