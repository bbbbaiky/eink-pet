# -*- coding: utf-8 -*-
"""★R218：商店「名称 ↔ 图片」一致性守卫（构建闸门）。

背景：R131 把商店图标分流从 `item>=100` 改成 `item<N_LOOT`，结果把虚拟消耗品
102/103/104/105/106 当成真收藏品画 g_loot[item] 的图 —— 小鱼饼干画成火山石、
提神草画成冰川蓝冰。这个 bug Build 期查不出、看代码也不易发现，只有上屏才看得到。

本脚本把固件 draw_shop_page() 的分流逻辑**逐件复算**，核对：
  ① 每件虚拟消耗品的 shop_item_icon(item) 是否落在 0..N_ITEM_ICON-1 且指向的图语义正确；
  ② 每件真收藏品 (item<N_LOOT) 的 g_loot[item].name 是否 == SHOP_POOL 里的 name；
  ③ SHOP_POOL 行数 == SHOP_POOL_N；
  ④ 图标 bin 片数 == N_LOOT_ICON + N_ITEM_ICON。
任一不符 → 退出码 1，拦下构建。

用法：python scripts/_tools/check_shop_icons.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN_C = ROOT / "common" / "main.c"
LOOT_C = ROOT / "common" / "loot_system.c"
LOOT_H = ROOT / "common" / "loot_data.h"
API_H = ROOT / "common" / "main_api.h"
SHOP_C = ROOT / "common" / "shop_system.c"
ICON_BIN = ROOT / "common" / "loot_icons.bin"
ICON_H = ROOT / "fw_epaper_demo" / "main" / "assets" / "loot_icons.h"


def parse_macros(*paths: Path) -> dict[str, int]:
    out: dict[str, int] = {}
    for p in paths:
        for m in re.finditer(r"#define\s+([A-Z][A-Z0-9_]*)\s+(\d+)\b", p.read_text(encoding="utf-8", errors="replace")):
            out[m.group(1)] = int(m.group(2))
    return out


def parse_g_loot() -> list[str]:
    src = LOOT_C.read_text(encoding="utf-8", errors="replace")
    body = src[src.index("const loot_t g_loot[] = {"):]
    body = body[:body.index("\n};")]
    return [m.group(1) for m in re.finditer(r'\{\s*"([^"]+)",\s*"([^"]+)"', body)]


def parse_shop_pool(macros: dict[str, int]):
    """返回 [(item号, 名称)]，解析 SHOP_POOL 的 item 表达式（宏或数字）。"""
    src = MAIN_C.read_text(encoding="utf-8", errors="replace")
    body = src[src.index("const shop_pool_t SHOP_POOL[] = {"):]
    body = body[:body.index("\n};")]
    # 去掉注释，避免把注释里的 { } 当条目
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out = []
    for m in re.finditer(r'\{\s*([A-Za-z_][A-Z0-9_]*|\d+)\s*,\s*\d+\s*,\s*"([^"]+)"\s*\}', body):
        expr, name = m.group(1), m.group(2)
        idx = macros[expr] if expr in macros else (int(expr) if expr.isdigit() else None)
        out.append((idx, name, expr))
    return out


def main() -> int:
    macros = parse_macros(LOOT_H, API_H, ICON_H)
    n_loot = macros["N_LOOT"]
    n_item = macros["N_ITEM_ICON"]
    g_loot = parse_g_loot()
    pool = parse_shop_pool(macros)
    problems: list[str] = []

    print("i18n/shop 守卫：N_LOOT=%d g_loot=%d N_ITEM_ICON=%d SHOP_POOL=%d"
          % (n_loot, len(g_loot), n_item, len(pool)))

    # ① 表行数
    if len(pool) != macros.get("SHOP_POOL_N", -1):
        problems.append("SHOP_POOL 行数 %d != SHOP_POOL_N %d" % (len(pool), macros.get("SHOP_POOL_N", -1)))
    if len(g_loot) != n_loot:
        problems.append("g_loot 行数 %d != N_LOOT %d" % (len(g_loot), n_loot))

    # ② 图标 bin 片数
    if ICON_BIN.is_file():
        n_icon = ICON_BIN.stat().st_size // 968
        expect = macros.get("N_LOOT_ICON", n_loot) + n_item
        if n_icon != expect:
            problems.append("loot_icons.bin 片数 %d != 期望 %d" % (n_icon, expect))

    # ③ 逐件核对：真收藏品名 == g_loot[item].name
    #    虚拟消耗品必须在 shop_item_icon() 的显式映射里（不能有落 default return 0）
    shop_c = SHOP_C.read_text(encoding="utf-8", errors="replace")
    icon_fn = shop_c[shop_c.index("int shop_item_icon(int item)"):]
    icon_fn = icon_fn[:icon_fn.index("\n}")]
    explicit = set()
    for m in re.finditer(r"item\s*==\s*([A-Za-z_][A-Z0-9_]*|\d+)", icon_fn):
        e = m.group(1)
        explicit.add(macros[e] if e in macros else (int(e) if e.isdigit() else -1))

    # shop_is_consumable() 的显式虚拟件集合（固件 draw_shop_page 的真实分流判据）
    consumable_fn = shop_c[shop_c.index("bool shop_is_consumable"):]
    consumable_fn = consumable_fn[:consumable_fn.index("\n}")]
    virtuals: set[int] = set()
    for m in re.finditer(r"item\s*==\s*([A-Za-z_][A-Z0-9_]*|\d+)", consumable_fn):
        e = m.group(1)
        virtuals.add(macros[e] if e in macros else (int(e) if e.isdigit() else -1))

    for idx, name, expr in pool:
        if idx is None:
            problems.append("商品 %r 的 item 表达式无法解析" % expr)
            continue
        if idx in virtuals:
            # 虚拟消耗品：走 shop_item_icon()，必须在显式映射里，且不能落 default
            if idx not in explicit:
                problems.append("虚拟商品 %s(%d) 未在 shop_item_icon() 显式映射 → 落 default 画猫薄荷图"
                                % (expr, idx))
        elif 0 <= idx < n_loot:
            # 真收藏品：走 g_loot[item] 的图，名称必须一致
            real = g_loot[idx]
            if real != name:
                problems.append("商品 %s(%d) 名称'%s' 与图标 g_loot[%d]='%s' 不符"
                                % (expr, idx, name, idx, real))
        else:
            problems.append("商品 %s(%d) 越界（既非真品也非虚拟件）" % (expr, idx))

    if problems:
        print("\n!! 商店图标一致性检查失败 %d 处：" % len(problems))
        for p in problems:
            print("   -", p)
        return 1
    print(">>> 商店图标一致性检查通过：%d 件商品名称↔图片全部对得上" % len(pool))
    return 0


if __name__ == "__main__":
    sys.exit(main())
