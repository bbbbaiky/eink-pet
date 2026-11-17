#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两版分叉「纯重构」的形式化验证（★R147 新增）。

用途
----
`common/main.c` 用 `#if TOUCH_DEMO` 分出按键版 / 触屏版。历史上很多函数被**整份复制**
到两个分支里，改一处必须记得改另一处。把这类重复合并成一份时，最容易出的错是
「某版少了一句 / 多了一句」—— 肉眼看 500 行 diff 根本看不住。

本工具的做法：拿两套 build 目录里 `compile_commands.json` 的**真实编译命令**，
对同一份 `main.c` 分别跑 `gcc -E -P`（完整预处理，含两版各自的 variant.h），
比对预处理输出的 md5。**逐字节一致 ⇒ 两版的编译输入完全没变，合并语义零改动。**

用法（在改代码**之前**存基线，改完**立刻**比对）::

    python scripts/_tools/verify_variant_merge.py --save
    ... 做纯重构（不改任何行为）...
    python scripts/_tools/verify_variant_merge.py --check

⚠ 任何**刻意改行为**的提交（修 bug / 改文案 / 升 FW_BUILD）都会让 md5 变化，
  这是正常的 —— 那种改动本来就不该用本工具验收，改完记得重新 `--save`。
⚠ Windows cmd 命令行上限 8191 字符，而 IDF 的编译命令约 8.5K
  ⇒ 必须走 gcc 的 `@response-file`，并把反斜杠路径统一成正斜杠（否则会被吞掉）。
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BASELINE = os.path.join(HERE, "variant_baseline.json")
# 两套构建目录：名字 → compile_commands.json
import pet_paths                     # ★R217：统一路径解析
_BUILD = pet_paths.build_base()
CFG = {
    "KEY":   os.environ.get("PET_KEY_CC",   str(_BUILD / "base_key" / "compile_commands.json")),
    "TOUCH": os.environ.get("PET_TOUCH_CC", str(_BUILD / "base_touch" / "compile_commands.json")),
}
# 预处理产物写到仓库外，避免污染 git（默认系统临时目录）
OUTDIR = os.environ.get("PET_PP_OUT", str(pet_paths.tmp_dir())).replace("\\", "/")


def preprocess(name, cfgpath):
    data = json.load(open(cfgpath, encoding="utf-8"))
    ent = next(e for e in data
               if e["file"].replace("\\", "/").endswith("common/main.c"))
    cmd = ent["command"]
    gcc, rest = cmd.split(" ", 1)
    out = OUTDIR + "/_pp_%s.i" % name
    m = re.search(r"\s+-o\s+\S+\s+-c\s+(\S+)\s*$", rest)
    if not m:
        raise RuntimeError("%s：编译命令尾部不是预期的「-o obj -c src」" % name)
    src = m.group(1).replace("\\", "/")
    rest = rest[:m.start()] + ' -E -P -o "%s" "%s"' % (out, src)
    rest = rest.replace("\\", "/")          # @response-file 会吞反斜杠
    rsp = OUTDIR + "/_pp_%s.rsp" % name
    open(rsp, "w", encoding="utf-8").write(rest)
    r = subprocess.run([gcc, "@" + rsp], shell=False, cwd=ent["directory"],
                       capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError("%s：预处理失败 rc=%d\n%s" % (name, r.returncode,
                                                        (r.stderr or "")[-800:]))
    raw = open(out, "rb").read()
    return {"bytes": len(raw), "md5": hashlib.md5(raw).hexdigest()}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--check"
    if mode not in ("--save", "--check"):
        raise SystemExit("用法：verify_variant_merge.py --save | --check")
    cur = {n: preprocess(n, p) for n, p in CFG.items()}
    for n, v in cur.items():
        print("%-6s %9d B  md5=%s" % (n, v["bytes"], v["md5"]))
    if mode == "--save":
        cur["_note"] = ("纯重构基线；改行为的提交后需重新 --save。"
                        "改前 --save，改完 --check，md5 必须全等。")
        json.dump(cur, open(BASELINE, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)
        print(">>> 基线已写入", BASELINE)
        return 0
    if not os.path.exists(BASELINE):
        raise SystemExit("!! 没有基线，先跑 --save：" + BASELINE)
    old = json.load(open(BASELINE, encoding="utf-8"))
    bad = [n for n in cur if old.get(n, {}).get("md5") != cur[n]["md5"]]
    if bad:
        print("!! 语义发生变化（或忘了重新 --save）：", ", ".join(bad))
        for n in bad:
            print("   %s  old=%s  new=%s" % (n, old.get(n, {}).get("md5"), cur[n]["md5"]))
        return 1
    print(">>> 两版预处理输出与基线逐字节一致 ✅ 合并未改变任何一版的语义")
    return 0


if __name__ == "__main__":
    sys.exit(main())
