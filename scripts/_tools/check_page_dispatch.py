#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""页面分派穷尽性闸门（★R184）。

为什么有它
----------
页面状态机现在有四份平行的 switch（page_render / touch_tap / 按键分派 / 各页内部）。
历史上「新增页面漏加一处」反复出问题（旅程页热区、测试页第二页热区、PG_BATT_LOW 死页…），
而 C 的 switch 缺 case 不报错 —— 只会静默走 default，上屏表现就是「点了没反应」。

本脚本把「漏一处」变成**构建失败**：
  ① page_render() 必须覆盖枚举里的**每一个** PG；
  ② touch_tap() 必须覆盖除 PG_GUIDE（按键版独有）之外的每一个 PG；
  ③ 任何 case 里出现的 PG_ 必须是枚举里有的（防手写错名）。

不是重构、不改任何行为 —— 只读源码 + 退出码。
用法：python scripts/_tools/check_page_dispatch.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MAIN = (ROOT / "common" / "main.c").read_text(encoding="utf-8", errors="replace")
API = (ROOT / "common" / "main_api.h").read_text(encoding="utf-8", errors="replace")


def enum_members(text: str, first_line_hint: str):
    """取 `typedef enum {` … `} page_t;` 之间的 PG_ 成员（去注释）。"""
    i = text.find(first_line_hint)
    if i < 0:
        return None
    b = text.find("{", i)
    e = text.find("} page_t;", b)
    if b < 0 or e < 0:
        return None
    body = re.sub(r"/\*.*?\*/", "", text[b:e], flags=re.S)
    return [m for m in re.findall(r"\b(PG_[A-Z0-9_]+)\b", body)]


lines = API.split("\n")
touch_enum = enum_members(API[API.find("#if TOUCH_DEMO"):
                               API.find("#else", API.find("#if TOUCH_DEMO"))], "typedef enum")
key_enum = enum_members(API[API.find("#else", API.find("#if TOUCH_DEMO")):
                            API.find("#endif", API.find("#else", API.find("#if TOUCH_DEMO")))],
                        "typedef enum")
if not touch_enum or not key_enum:
    sys.exit("!! 解析 page_t 枚举失败（main_api.h 结构变了？）")


def switch_cases(func_sig: str):
    i = MAIN.find(func_sig)
    if i < 0:
        return None
    j = MAIN.find("{", i)
    d, k = 0, j
    while k < len(MAIN):
        if MAIN[k] == "{":
            d += 1
        elif MAIN[k] == "}":
            d -= 1
            if d == 0:
                break
        k += 1
    body = MAIN[j:k]
    # 去掉注释再取 case（避免把注释里的 "case PG_XXX" 当真）
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    return set(re.findall(r"\bcase\s+(PG_[A-Z0-9_]+)\s*:", body))


pr = switch_cases("void page_render(bool in_page_sel)")
tap = switch_cases("static void touch_tap(int x, int y)")
if pr is None or tap is None:
    sys.exit("!! 找不到 page_render / touch_tap 的函数体（改名了？先同步本脚本）")

problems = []
miss_pr = [p for p in sorted(set(touch_enum) | set(key_enum)) if p not in pr]
if miss_pr:
    problems.append("page_render 缺 case：" + ", ".join(miss_pr))

touch_expect = [p for p in touch_enum if p != "PG_GUIDE"]
# 允许**不写进 switch** 的页：必须在 touch_tap 之前或别处已被处理，或本来就不可交互。
# 新增页面若确实不需要 tap，请在这里补一行并写清理由 —— 本表过期（页被删）也会报错。
NO_TAP_OK = {
    "PG_WIP":         "占位页，无入口（R147 已删 enter_wip）",
    "PG_PROV":        "手机配网流程驱动，设备端只显示进度",
    "PG_PROV_OK":     "配网成功页，自动返回",
    "PG_WIFI_DEL":    "删除成功结果页，定时自动回列表",
    "PG_BATT_DEAD":   "没电关机终屏，不可交互",
    "PG_POST_IMPORT": "手机传照片门户：返回箭头(TZ_BACK) + 手机端操作",
    "PG_OTA_CHECK":   "OTA 系列：按钮区由 ota_on_tap() 在 switch 之前统一接",
    "PG_OTA_FOUND":   "同上",
    "PG_OTA_NEWEST":  "同上",
    "PG_OTA_PROG":    "同上",
    "PG_OTA_OK":      "同上",
    "PG_OTA_FAIL":    "同上",
    "PG_OTA_SEL":     "同上",
    "PG_OTA_MODE":    "同上（★R218 二级：手动/在线，热区在 ota_on_tap）",
    "PG_OTA_WEB":     "同上",
    "PG_ACHV_DET":    "成就详情：按钮区由 achv_on_tap() 在 switch 之前接",
    "PG_ACHV_OK":     "成就完成页：同上",
}
miss_tap = [p for p in touch_expect if p not in tap and p not in NO_TAP_OK]
if miss_tap:
    problems.append("touch_tap 缺 case（且不在 NO_TAP_OK 白名单）：" + ", ".join(miss_tap))
stale = [p for p in NO_TAP_OK if p not in set(touch_enum) | set(key_enum)]
if stale:
    problems.append("NO_TAP_OK 里有枚举中不存在的页（名单过期）：" + ", ".join(sorted(stale)))

known = set(touch_enum) | set(key_enum)
typos = sorted((pr | tap) - known)
if typos:
    problems.append("case 里出现枚举中不存在的名字：" + ", ".join(typos))

print("页面枚举：触屏 %d 个 · 按键 %d 个" % (len(touch_enum), len(key_enum)))
print("page_render 覆盖 %d 个 case · touch_tap 覆盖 %d 个 case" % (len(pr), len(tap)))
if problems:
    for p in problems:
        print("!! " + p)
    print("!! 页面分派不穷尽 —— 已拦下构建（补 case 或同步本脚本的期望）")
    sys.exit(1)
print(">>> 页面分派穷尽性检查通过")
