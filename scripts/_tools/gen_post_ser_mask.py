#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_post_ser_mask.py —— 由「city → 去向归类」生成 <common/post_ser_mask.h>

背景（接线核查报告 2026-10-26 第②③条）：
    近郊寄信原先是 `esp_random() % N_POST_BASE` 纯随机抽基础明信片池（298 张），
    但 g_loot_fx[] 里已有「LFX_MAIL_PP + LFX_SC_WILD/SEA/CITY」按去向加成的行 ——
    概率按去向算了，抽到的片却不按去向选 ⇒ 山野寄信可能寄回一张都市片。

修法：给每张片打一个「适用去向」bitmask，抽片时先按本次 dest 过滤候选池。
    bit0 = LOOT_SER_CITY(0) 城市
    bit1 = LOOT_SER_WILD(1) 山野
    bit2 = LOOT_SER_SEA (2) 海滨

⚠ 归类本身是**设计口径**，必须由用户拍板。本脚本里的 SER_* 三个集合是 2026-10-26 的
  **初稿**（规则见下），状态标记在 base_wired/_REFACTOR_STATUS.md「待用户确认」段。
  改归类 → 改本脚本顶部的集合 → 重跑 → 重编即可，不用手改 586 行表。

初稿归类规则：
    SEA  = 风景主体是「海」（海岸 / 海岛 / 渔港 / 滩涂）
    WILD = 风景主体是「野外」（山 / 高原 / 湖 / 草原 / 沙漠 / 田园乡村）
    CITY = 城市 / 古镇 / 人文名胜（含沿海大都会 —— 它们的主体是城，不是海）
    未列出的城市一律落 CITY；笔友/虚构邮局与「中转站」= 通配（任何去向都可抽到）。

用法：python scripts/_tools/gen_post_ser_mask.py
    读 common/post_system.c 的 g_posts[] → 写 common/post_ser_mask.h
"""
import io
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(ROOT, "common", "post_system.c")
OUT = os.path.join(ROOT, "common", "post_ser_mask.h")

M_CITY, M_WILD, M_SEA = 0x01, 0x02, 0x04
ALL = M_CITY | M_WILD | M_SEA

# ── 归类（2026-10-26 用户拍板口径，替代 10-26 初稿）─────────────────────
# 用户口径（原话要点）：
#   · 沿海大都会 = **又算城市又算海滨**（两套物品都能掉），不硬归一边；
#   · 大理/洱海：算城市也行，但山野会太少 → 归山野（平衡考虑）；
#   · 塞外古城（喀什/嘉峪关/吐鲁番）：归山野；
#   · 其余有争议的按「哪边池子薄就往哪边补」平衡。
WILD = """
拉萨 敦煌 青海湖 喀纳斯 长白山 九寨沟 张家界 黄山 呼伦贝尔 额济纳 茶卡 稻城 张掖
香格里拉 武当山 泰安 承德 九华山 婺源 桂林 阳朔 龙胜 龙脊 安顺 吉县 楠溪江 武夷山
兴化 龙井 嘉峪关 吐鲁番 甘孜 浑源 大理 洱海 昆仑 草原 山区 湖区
喀什 黟县 西递 宏村 平遥 凤凰 丽江 束河
""".split()

SEA = """
秦皇岛 蓬莱 霞浦 舟山 三亚 涠洲岛 东山 南澳 枸杞岛 蜈支洲 海上
""".split()

# ★沿海大都会 = 城市 + 海滨 双归（CITY|SEA）：两边的明信片都能抽到，
#   实际占比由两边候选池大小自然分摊（= 用户说的「设一个概率」）。
COASTAL_METRO = """
上海 香港 澳门 深圳 广州 天津 泉州 宁波 福州 温州 汕头 湛江 海口 北海 珠海
厦门 青岛 大连
""".split()

# 笔友/虚构邮局 + 中性地点 = 通配
WILDCARD = "小鸟邮局 蝴蝶邮局 中转站 远方 家".split()

# 远行 10 站（不参与近郊池，但同样归类，避免将来池边界调整时落到通配）
FAR2YOND = {              # city -> mask
    "冰岛": M_WILD, "撒哈拉": M_WILD, "新西兰": M_WILD, "秘鲁": M_WILD,
    "埃及": M_WILD, "摩洛哥": M_WILD, "威尼斯": M_SEA,
    "京都": M_CITY, "伊斯坦布尔": M_CITY, "土耳其": M_CITY, "印度": M_CITY,
}

DEFAULT_MASK = M_CITY   # 其余城市（北京/古镇/人文名胜…）= 城市


def city_mask(city: str) -> int:
    if city in WILDCARD:
        return ALL
    if city in COASTAL_METRO:
        return M_CITY | M_SEA          # ★沿海大都会：城市/海滨都能抽到
    if city in WILD:
        return M_WILD
    if city in SEA:
        return M_SEA
    if city in FAR2YOND:
        return FAR2YOND[city]
    return DEFAULT_MASK


def main() -> None:
    s = io.open(SRC, encoding="utf-8").read()
    i = s.find("const post_t g_posts[]")
    if i < 0:
        raise SystemExit("找不到 g_posts[] —— 路径或表结构变了？")
    j = s.find("\n};", i)
    body = s[i:j]
    entries = re.findall(r'\{\s*"(.*?)"\s*,\s*"(.*?)"\s*,\s*"(.*?)"\s*,\s*"(.*?)"\s*,', body, re.S)
    if not entries:
        raise SystemExit("g_posts 解析失败（结构体字段顺序改了？）")
    cities = [e[3] for e in entries]

    masks = [city_mask(c) for c in cities]

    # 统计（打印给人看）
    def cnt(m, lo=0, hi=None):
        hi = len(masks) if hi is None else hi
        return sum(1 for k in range(lo, hi) if masks[k] & m)

    lines = []
    lines.append("/* post_ser_mask.h —— 明信片「适用去向」bitmask")
    lines.append(" *")
    lines.append(" * ⚠ 本文件由 scripts/_tools/gen_post_ser_mask.py **自动生成**，请勿手改。")
    lines.append(" *   改归类 → 改脚本顶部的 SEA/WILD/WILDCARD 集合 → 重跑脚本 → 重编。")
    lines.append(" *")
    lines.append(" * 每条 = g_posts[] 同下标那一张明信片能出现在哪些去向的近郊寄信里：")
    lines.append(" *   bit0 = LOOT_SER_CITY 城市 / bit1 = LOOT_SER_WILD 山野 / bit2 = LOOT_SER_SEA 海滨")
    lines.append(" * 笔友邮局与中性地点 = 0x07（通配，任何去向都能抽到）。")
    lines.append(" *")
    lines.append(" * ★口径 = 2026-10-26 用户拍板：沿海大都会 CITY|SEA 双归（上海/香港/澳门/深圳/广州/")
    lines.append(" *   天津/泉州/宁波/福州/温州/汕头/湛江/海口/北海/珠海/厦门/青岛/大连）；")
    lines.append(" *   大理·洱海与塞外古城（喀什/嘉峪关/吐鲁番）归山野（平衡山野池子）。")
    lines.append(" */")
    lines.append("#ifndef POST_SER_MASK_H")
    lines.append("#define POST_SER_MASK_H")
    lines.append("")
    lines.append("#define POST_SER_M_CITY 0x01u")
    lines.append("#define POST_SER_M_WILD 0x02u")
    lines.append("#define POST_SER_M_SEA  0x04u")
    lines.append("")
    lines.append("/* 定义为 static + 只被 post_system.c 包含 ⇒ 不产生外部符号，不用动 CMakeLists */")
    lines.append("static const uint8_t POST_SER_MASK[%d] = {" % len(masks))
    for k in range(0, len(masks), 16):
        chunk = masks[k:k + 16]
        comment = "  /* %3d..%3d */" % (k, k + len(chunk) - 1)
        lines.append("    " + ", ".join("0x%02X" % v for v in chunk) + "," + comment)
    lines.append("};")
    lines.append("")
    lines.append("#endif /* POST_SER_MASK_H */")

    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    print("写出 %s：%d 张" % (OUT, len(masks)))
    print("  全池  城市 %d / 山野 %d / 海滨 %d（含通配重叠）"
          % (cnt(M_CITY), cnt(M_WILD), cnt(M_SEA)))
    print("  近郊池 [0,294) 城市 %d / 山野 %d / 海滨 %d"
          % (294, cnt(M_CITY, 0, 294), cnt(M_WILD, 0, 294), cnt(M_SEA, 0, 294)))
    for lo, hi in ((0, 294), (294, len(masks))):
        if cnt(M_CITY, lo, hi) and cnt(M_WILD, lo, hi) and cnt(M_SEA, lo, hi):
            continue
        print("  ⚠ [%d,%d) 有去向抽不到片" % (lo, hi))
    # 落到默认（CITY）的城市清单 —— 给人复核
    seen = {}
    for c, m in zip(cities, masks):
        seen.setdefault((c, m), 0)
        seen[(c, m)] += 1
    dflt = sorted(c for (c, m) in seen if m == DEFAULT_MASK)
    print("  落默认「城市」的 %d 种：%s" % (len(dflt), " ".join(dflt)))


if __name__ == "__main__":
    main()
