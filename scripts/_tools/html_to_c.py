# -*- coding: utf-8 -*-
"""★R217：明信片导入页 HTML → common/postcard_page.h（补回已丢失的生成链）。

原生成器 _postcard_proto/html_to_c.py 随目录清理删除，资料库里也没有同名节点；
本脚本按现有 postcard_page.h 的写法重建同样的 C 字符串常量，格式逐字一致。

用法：
  python scripts/_tools/html_to_c.py                      # 用 <inbox>/postcard_editor.html，dry-run
  python scripts/_tools/html_to_c.py 某.html --apply      # 写回 common/postcard_page.h
  python scripts/_tools/html_to_c.py 某.html 输出.h --apply

★ 默认只对比、不写盘：现有 .h 是上机资产，别让一次手滑覆盖掉。
  HTML 源缺失时只打印提示并正常退出（此时 .h 作为冻结资产继续用，固件不受影响）。
"""
from __future__ import annotations

import sys
from pathlib import Path

import pet_paths

HEADER = """/* 由 scripts/_tools/html_to_c.py 生成 —— 源：%s。改页面改 HTML 后重跑本脚本。 */
#pragma once
static const char POST_PAGE_HTML[] =
"""
FOOTER = '    ""\n;\n'


def c_line(s: str) -> str:
    return '    "' + s.replace('\\', '\\\\').replace('"', '\\"') + '\\n"\n'


def main() -> int:
    apply = "--apply" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--apply"]
    src = Path(args[0]) if args else pet_paths.inbox() / "postcard_editor.html"
    dst = Path(args[1]) if len(args) > 1 else pet_paths.REPO / "common" / "postcard_page.h"
    if not src.is_file():
        print("!! 找不到 HTML 源：%s" % src)
        print("   现有 postcard_page.h 是冻结资产，固件照常编译；")
        print("   需要改页面请把 postcard_editor.html 放回素材目录，或把路径作为第一个参数传入。")
        return 0
    lines = src.read_text(encoding="utf-8").splitlines()
    out = HEADER % src.name + "".join(c_line(ln) for ln in lines) + FOOTER
    if dst.is_file() and dst.read_text(encoding="utf-8") == out:
        print("✓ 与现有 %s 一致，无需改动" % dst)
        return 0
    print("生成 %d 行 HTML → %s（%d 字节）" % (len(lines), dst, len(out.encode("utf-8"))))
    if apply:
        dst.write_text(out, encoding="utf-8")
        print("✓ 已写回（记得重新编译两版固件）")
    else:
        print("（dry-run，未写盘；加 --apply 写回）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
