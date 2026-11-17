#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_page_hints.py —— 按键版「底部提示带」白名单冻结检查（2026-10-26 R75）

背景（用户第 N 次报「明信片分类页底部提示又出现了」）：
    fb_hint_unify() 里已有 hint_page_approved() 白名单护栏，但护栏**本身**
    （hint_page_approved 的 case 列表）和 PAGE_HINT 文案是同一批人顺手一起加的 ——
    PG_POST_CAT 就是这么进白名单的，所以"有护栏还复发"。
    ⇒ 本脚本把白名单**冻结**成 FROZEN 常量：构建时若发现代码里的白名单多了/少了页，
      直接失败。想给新页加提示 = 必须先得到用户明确要求，再同时改三处：
        ① PAGE_HINT[PG_X] 文案；② hint_page_approved() 的 case；③ 本文件 FROZEN。
      三者缺一，build2.sh / build2_touch.sh 会在编译前报错。

只由构建脚本调用：python scripts/_tools/check_page_hints.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
SRC = ROOT / "common" / "main.c"

# ★ 冻结名单 = 用户明确认可过、且当前确实会出底部提示的页面。
#   任何增删都必须有人在这里改一行 —— 这是防止"顺手加提示"的最后一道闸。
FROZEN = {
    # ★R118 重建（用户第三次点名「按键版成就详情底下还有 Power领取/长按返回」）：
    # 只留「状态类」文案 + 唯一该写键位的操作提示页。
    "PG_PROV_OK",
    "PG_WIFI_CONN_OK", "PG_WIFI_CONN_FAIL",
    "PG_SYNC_OK", "PG_SYNC_FAIL",
    "PG_BATT_DEAD", "PG_PET_M4",
    "PG_GUIDE",
    "PG_OTA_CHECK",
    # ★R170：用户 2026-11-10 点名恢复的两页（R118 整版清空时被误伤）
    "PG_FOOD", "PG_POST_ITEM",
}
# ★R118 新增：**按内容**拦 —— 允许写键位的只有操作提示页。
# 白名单"改三处"那套防不住"顺手又加一页"，所以再加一道与名单无关的闸：
# 任何页面（PG_GUIDE 除外）的提示串里出现下列键位词 ⇒ 直接构建失败。
KEY_WORDS = ("Boot", "BOOT", "Power", "POWER", "长按", "短按", "按住", "任意键")
# 例外：**结果页**那句「按任意键返回，3 秒后自动」是"3 秒会自动走"的说明，不是键位说明书；
#   用户从未对这几页提过意见（2026-09 起就在），故保留。若哪天要一并去掉，改文案后从本表删。
KEY_WORD_OK = {"PG_GUIDE",
               "PG_PROV_OK", "PG_WIFI_CONN_OK", "PG_WIFI_CONN_FAIL",
               "PG_SYNC_OK", "PG_SYNC_FAIL", "PG_PET_M4",
               # ★R170：用户点名要留的键位提示（喂食页 / 明信片详情页）
               "PG_FOOD", "PG_POST_ITEM"}


def main() -> int:
    s = SRC.read_text(encoding="utf-8", errors="ignore")

    m = re.search(r"static bool hint_page_approved\(page_t p\)\s*\{(.*?)\n\}", s, re.S)
    if not m:
        print("!! check_page_hints: 找不到 hint_page_approved() —— 函数被改名/删了？")
        return 1
    approved = set(re.findall(r"case\s+(PG_[A-Z0-9_]+)\s*:", m.group(1)))

    extra = sorted(approved - FROZEN)
    missing = sorted(FROZEN - approved)
    if extra or missing:
        print("!! 底部提示白名单被改动 —— 构建已拦下（用户令：默认不出底部提示）")
        if extra:
            print("   擅自新增（未获用户点名）：", " ".join(extra))
            print("   · 新页要出提示，必须先取得用户明确要求，再同步改 PAGE_HINT / 本函数 / FROZEN")
        if missing:
            print("   被移除（若是有意删除，请同步更新 FROZEN）：", " ".join(missing))
        return 1

    # 提示表里写了文案但没进白名单的页 = 只擦不写（符合"默认拒绝"），仅提示，不拦构建。
    # ★R170 全检：rows 原来只在 hm 命中时定义，下面 bad 却在 if 之外用 → 表被改名时
    #   脚本自己 NameError 崩掉（护栏失效）。先给空列表，并让「找不到表」直接失败。
    rows = []
    hm = re.search(r"static const char \*const PAGE_HINT\[\]\s*=\s*\{(.*?)\n\};", s, re.S)
    if not hm:
        print("!! check_page_hints: 找不到 PAGE_HINT[] 表 —— 内容闸无法执行")
        return 1
    if hm:
        rows = re.findall(r"\[(PG_[A-Z0-9_]+)\]\s*=\s*\"([^\"]*)\"", hm.group(1))
        dormant = sorted(p for p, t in rows if p not in approved and t)
        if dormant:
            print("   · 注意：这些页写了提示文案但未进白名单（当前只擦不写）：", " ".join(dormant))

    # ★R118 内容闸：非 PG_GUIDE 的页，提示串里不许出现键位词。
    bad = sorted(p for p, t in rows if p not in KEY_WORD_OK and any(w in t for w in KEY_WORDS))
    if bad:
        print("!! 底部提示带里出现键位文案 —— 构建已拦下（用户令：按键版不要键位提示）")
        for p in bad:
            print("   ·", p, "->", dict(rows)[p])
        print("   · 只有 PG_GUIDE（操作提示页）允许写键位；其它页请改成状态文案或置空")
        return 1

    print(">>> 底部提示白名单检查通过（%d 页）" % len(approved))
    return 0


if __name__ == "__main__":
    sys.exit(main())
