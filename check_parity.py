#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""★ 两版接线守卫（2026-11-12 就地复活，替掉 R144 起的「已废弃」空壳）

为什么要复活
------------
R144 单源重构后本脚本打印一句「已废弃」就 sys.exit(0)，后面 200 余行检查
全部不可达。此后真正在起作用的两版守卫只剩「两版都编译过」+
check_page_dispatch.py；verify_variant_merge.py 的口径是「两版各自 vs 基线
md5」，**不是两版互比**，且依赖仓库外的 compile_commands.json。

R151 事故形状：friend_net_msg_mark_read() 被塞进 #if TOUCH_DEMO 分支，
按键版静默失效（R152 修复，friend_net.c:498 有复盘注释）。本脚本纯源码、
不依赖构建产物。

两道硬拦（任一不过 → 退出码 1）
--------------------------------
  1. 两版 main/CMakeLists.txt 的 SRCS 集合互比（差集只许触屏驱动）
  2. 公共 common/ 不许在某个工程目录里出现副本（又分叉了）

另有**软提示**：#if TOUCH_DEMO 分支里出现 MUST_SHARED 名单里的调用、或
关键函数定义时，列出坐标供人工复核，**不拦构建**。
⚠ 为什么不能硬拦：main.c 本来就是单源 + #if TOUCH_DEMO 分流，
   `void app_main(void)` 在 20955 / 23379 各一份、`post_load()` 在
   21060 / 22168 各一份 —— 两版各有自己的入口是设计，不是分叉。
   触屏专属路径调用公共函数同样是常态（测试菜单 case 13 的 post_save()、
   配网成功的 clock_rebase()）。硬判「出现在触屏块里」会天天误报。
   ⇒ R151 那类「按键版完全没有对应动作」的错位，仍需两版编译 + 人工复核，
     本脚本不冒充能自动拦住它。

用法
----
    python check_parity.py            # 体检并拦截
    python check_parity.py --strict   # 同义（保留旧参数兼容，默认即拦）

退出码：0 = 通过；1 = 发现接线错位；2 = 环境/文件缺失。
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
COMMON = os.path.join(ROOT, "common")
KEY_CM = os.path.join(ROOT, "fw_epaper_demo", "main", "CMakeLists.txt")
TCH_CM = os.path.join(ROOT, "fw_touch", "main", "CMakeLists.txt")

# 触屏版独有驱动：允许出现在差集里，多出/少掉别的源文件都算接线漏了
ALLOW_TOUCH_ONLY = {"touch_ft6336.c"}

# ★ 必须两版共用的业务/持久化/同步函数（软提示名单）。
#   新增公共业务钩子时把它加进来，便于人工复核触屏分支里的调用。
MUST_SHARED = {
    "friend_net_msg_mark_read", "post_save", "post_slow_tick",
    "post_slow_enqueue", "post_mark_unread", "post_load",
    "ws_intimacy_add", "ws_intimacy_add_auto",
    "loot_trip_mail_tick", "exp_day_mail", "clock_rebase",
    "assets_shadow_load_all", "i18n_asset_load", "mypost_load",
}

# ★ 硬拦名单：这些函数的**定义**绝不可能只属于触屏版。
MUST_SHARED_DEFS = {
    "app_main", "post_load", "post_save", "post_slow_tick",
    "clock_rebase", "assets_shadow_load_all", "i18n_asset_load",
}


def srcs_of(path):
    """取 idf_component_register(SRCS ...) 里的 .c 基名集合。"""
    txt = open(path, encoding="utf-8-sig", errors="replace").read()
    m = re.search(r"\bSRCS\s+(.*?)\s*INCLUDE_DIRS", txt, re.S)
    if not m:
        raise RuntimeError("没找到 SRCS 段：" + path)
    return {os.path.basename(x) for x in re.findall(r'"([^"]+\.c)"', m.group(1))}


def touch_branch_checks():
    """返回 ({软提示函数名: 文件:行号}, {硬拦函数名: 文件:行号})。

    行级预处理状态机：记录 TOUCH_DEMO 块所在嵌套深度；遇到同深度的
    #else / #elif 就切出 then 分支，配对 #endif 时退出该块。
    ★ 去块注释必须保留等量换行，否则 enumerate 的行号会整体前移。
    """
    soft, hard = {}, {}
    for f in sorted(glob.glob(os.path.join(COMMON, "*.c"))):
        src = re.sub(r"/\*.*?\*/",
                     lambda m: "\n" * m.group(0).count("\n"),
                     open(f, encoding="utf-8", errors="replace").read(), flags=re.S)
        # ★ 用配对栈而不是「depth 整数」：main.c 有 169 个条件块，
        #   靠计数器一旦某处失衡就会把后面整段（连 app_main 都算进去）误判成触屏分支。
        stack = []                 # 每项 = [是否 TOUCH_DEMO 块, 是否还在 then 分支]
        for no, ln in enumerate(src.split("\n"), 1):
            code = re.sub(r"//[^\n]*", "", ln).strip()
            if re.match(r"#\s*if(def|ndef)?\b", code):
                is_touch = ("TOUCH_DEMO" in code and not any(s[0] for s in stack)
                            and not re.search(r"#\s*if\w*\s*!\s*TOUCH_DEMO", code))
                stack.append([is_touch, True])
                continue
            if re.match(r"#\s*(else|elif)\b", code):
                if stack:
                    stack[-1][1] = False
                continue
            if re.match(r"#\s*endif\b", code):
                if stack:
                    stack.pop()
                continue
            if not any(s[0] and s[1] for s in stack):
                continue
            pos = "%s:%d" % (os.path.basename(f), no)
            for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code):
                n = m.group(1)
                if n in MUST_SHARED and n not in soft:
                    soft[n] = pos
            d = re.match(r"(?:static\s+)?(?:inline\s+)?"
                         r"(?:void|int|bool|uint\w+_t|int32_t|char)\s+(\w+)\s*\(", code)
            if d and d.group(1) in MUST_SHARED_DEFS and d.group(1) not in hard:
                hard[d.group(1)] = pos
    return soft, hard


def dup_copies():
    """某个工程目录下又出现一份 common/ 里的 .c = 分叉回潮。"""
    bad = []
    for proj in ("fw_epaper_demo", "fw_touch"):
        d = os.path.join(ROOT, proj, "main")
        for f in glob.glob(os.path.join(d, "*.c")):
            b = os.path.basename(f)
            if b != "touch_ft6336.c" and os.path.exists(os.path.join(COMMON, b)):
                bad.append(os.path.relpath(f, ROOT))
    return bad


def main():
    problems = []
    try:
        k, t = srcs_of(KEY_CM), srcs_of(TCH_CM)
    except Exception as e:                               # noqa: BLE001
        print("!! 环境错误：", e)
        return 2

    print("两版 SRCS：按键 %d 个 · 触屏 %d 个" % (len(k), len(t)))
    extra_t = sorted(t - k - ALLOW_TOUCH_ONLY)
    extra_k = sorted(k - t)
    if extra_t:
        problems.append("触屏版多出未登记的源文件：" + ", ".join(extra_t))
    if extra_k:
        problems.append("按键版多出源文件（触屏没接）：" + ", ".join(extra_k))

    soft, hard = touch_branch_checks()
    print("TOUCH_DEMO 分支扫描：共用业务调用 %d 处 · 关键函数定义 %d 处（均仅提示）"
          % (len(soft), len(hard)))
    for n, where in sorted(soft.items()):
        print("  · %s() 调用 @ %s —— 若按键版没有对应动作，人工复核（R151 形状）"
              % (n, where))
    for n, where in sorted(hard.items()):
        print("  · %s() 定义 @ %s —— 两版各一份入口属设计，仅留痕"
              % (n, where))

    dups = dup_copies()
    if dups:
        problems.append("公共目录出现工程副本：" + ", ".join(dups))

    if problems:
        for p in problems:
            print("  !! " + p)
        print("!! 两版接线守卫拦下")
        return 1
    print(">>> 两版接线守卫通过：SRCS 对齐 · 无分叉副本（TOUCH_DEMO 分支命中见上方提示，需人工复核）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
