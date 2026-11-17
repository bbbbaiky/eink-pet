#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# check_zfont_glyphs.py —— zfont.bin 字形有效性闸门（R154 · 2026-11-09）
# ----------------------------------------------------------------------------
# 为什么会有这个文件（用户问「为什么查过很多次缺字，还是不停冒出来」）：
#
#   ① 字库是 R107 按「当时的源码用字」裁出来的**静态快照**。之后每加一条文案、
#      一个地名、一条译文，都可能带进字库里没有的新字；而 build2*.sh 原来只跑
#      check_page_hints / check_i18n_full / pack_assets --verify 三个闸，
#      **完全不查字库** ⇒ 新字悄悄漏过去。
#   ② font_subset.py --check 只查「码点索引在不在」，不查「位图是不是真字形」。
#      用缺字的 BODY 字体补字时 PIL 会渲染 .notdef（方框 + 问号），
#      9 个码点共用同一张假字形，索引齐全、--check 全绿，上屏却是怪图。
#   ③ 正文档源字体 BoutiqueBitmap9x9_1.9_R.ttf 本身缺一批生僻字；
#      标题档 ZLabsPixel12px.ttf（= 资料库 ★源头_ZLabs点阵字12px.ttf，SHA256 一致）
#      覆盖它们，可用同一点阵字体 @10px 渲 BODY，字形正确、风格最接近。
#
# 本脚本把三层一起补上：
#   - 默认（--check）：源码用字全覆盖 + 位图不是 .notdef/空白；异常 exit 1，
#     已挂进 build2.sh / build2_touch.sh，新增文案带新字会直接拦下构建。
#   - --fix：自动补/替换位图并写回 common/zfont.bin（只增不减、两档同集、升序）。
#   - 修完必须跑 pack_assets.py --write 重打 assets 并烧 0x400000 分区。
#
# 用法：
#   python check_zfont_glyphs.py            # 只读检查（构建闸门）
#   python check_zfont_glyphs.py --list     # 打印问题明细
#   python check_zfont_glyphs.py --fix      # 修复并写回
# ============================================================================
from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
HDR_FILES = [ROOT / "fw_epaper_demo" / "main" / "assets" / "zfont.h",
             ROOT / "fw_touch" / "main" / "assets" / "zfont.h"]

import _zfont_to_bin as Z      # noqa: E402  Container / render / find_font / 配方常量
import font_subset as FS       # noqa: E402  scan_source_used / KNOWN_UNAVAILABLE

BIN = Z.BIN
SHARE_LIMIT = 5                # 同一张 BODY 位图被 >=5 个码点共用 ⇒ .notdef 嫌疑


def header_status(n: int):
    """两版 zfont.h 的字数常量必须与 bin 一致，否则开机 zfont_idx_load() 拒载。"""
    bad = []
    for hp in HDR_FILES:
        if not hp.is_file():
            bad.append((str(hp.relative_to(ROOT)), "文件缺失"))
            continue
        t = hp.read_text(encoding="utf-8")
        for macro in ("ZF_BODY_CJK_N", "ZF_TITLE_CJK_N"):
            m = re.search(r"#define\s+" + macro + r"\s+(\d+)u", t)
            val = int(m.group(1)) if m else None
            if val != n:
                bad.append((f"{hp.parent.parent.name}:{macro}", str(val)))
    return bad


def sync_headers(n: int) -> None:
    """--fix 后自动同步两版常量，杜绝「补完字忘了改 zfont.h → 开机兜底页」。"""
    for hp in HDR_FILES:
        if not hp.is_file():
            continue
        t = hp.read_text(encoding="utf-8")
        t2 = re.sub(r"(#define\s+ZF_BODY_CJK_N\s+)\d+u",
                    lambda m: f"{m.group(1)}{n}u", t)
        t2 = re.sub(r"(#define\s+ZF_TITLE_CJK_N\s+)\d+u",
                    lambda m: f"{m.group(1)}{n}u", t2)
        if t2 != t:
            with open(hp, "w", encoding="utf-8", newline="") as f:
                f.write(t2)
            print(f"同步 {hp.relative_to(ROOT)} → {n}u")


def collect(c):
    """返回 (shared, blank, missing)。shared = [(位图, [码点...]), ...]"""
    by_bmp = collections.defaultdict(list)
    blank = []
    for cp, bm in c.body.items():
        if cp < 0x80:
            continue
        by_bmp[bm].append(cp)
        if not any(bm):
            blank.append(cp)
    shared = [(bm, sorted(cps)) for bm, cps in by_bmp.items()
              if len(cps) >= SHARE_LIMIT]
    shared.sort(key=lambda x: -len(x[1]))

    used, _nfiles = FS.scan_source_used()
    have = set(c.body)
    missing = sorted(cp for cp in used
                     if cp not in have and cp not in FS.KNOWN_UNAVAILABLE)
    return shared, blank, missing


def check(c, verbose: bool) -> int:
    shared, blank, missing = collect(c)
    n_shared = sum(len(cps) for _bm, cps in shared)
    hdr_bad = header_status(len(c.body))
    bad = n_shared + len(blank) + len(missing) + len(hdr_bad)

    if verbose:
        print(f"字库 {len(c.body)} 字 · {BIN.stat().st_size} B")
        if shared:
            print(f"★ .notdef 嫌疑：{len(shared)} 张共用位图 / {n_shared} 个码点"
                  f"（同一张假字形被 {SHARE_LIMIT} 个以上字共用）")
            for bm, cps in shared:
                print("   " + " ".join(f"U+{cp:04X}({chr(cp)})" for cp in cps[:12])
                      + (" …" if len(cps) > 12 else ""))
        if blank:
            print(f"★ 全空位图：{len(blank)} 个 "
                  + " ".join(f"U+{cp:04X}({chr(cp)})" for cp in blank[:12]))
        if missing:
            print(f"★ 索引缺失：{len(missing)} 个 "
                  + " ".join(f"U+{cp:04X}({chr(cp)})" for cp in missing[:12]))
        if hdr_bad:
            print(f"★ zfont.h 字数常量不符（bin={len(c.body)}）："
                  + " ".join(f"{k}={v}" for k, v in hdr_bad))

    if bad:
        print(f"!! 共 {bad} 处字形问题 —— 先跑 check_zfont_glyphs.py --fix，"
              f"再 pack_assets.py --write 重打 assets")
        return 1
    print("✓ 字库检查通过：源码用字全覆盖 · 无共用假字形 · 无全空位图")
    return 0


def fix(c) -> int:
    from PIL import ImageFont

    shared, blank, missing = collect(c)
    bf = ImageFont.truetype(str(Z.find_font("body")), Z.BODY_SIZE)
    tf = ImageFont.truetype(str(Z.find_font("title")), Z.TITLE_SIZE)
    tf10 = ImageFont.truetype(str(Z.find_font("title")), Z.BODY_SIZE)

    def body10(cp):
        bm = Z.render(tf10, chr(cp), Z.BODY_H, Z.BODY_RB, Z.BODY_WCOLS,
                      Z.BODY_THR, Z.BODY_DX, Z.BODY_DY)
        if not any(bm):
            sys.exit(f"!! U+{cp:04X} {chr(cp)} 备用字体渲出空字形，拒绝写入")
        return bm

    def title12(cp):
        bm = Z.render(tf, chr(cp), Z.TITLE_H, Z.TITLE_RB, Z.TITLE_WCOLS,
                      Z.TITLE_THR, Z.TITLE_DX, Z.TITLE_DY)
        if not any(bm):
            sys.exit(f"!! U+{cp:04X} {chr(cp)} 标题档渲出空字形，拒绝写入")
        return bm

    fixed_fake = []
    for _bm, cps in shared:
        for cp in cps:
            c.body[cp] = body10(cp)      # TITLE 档原本就是 ZLabs 原生、已正确，不动
            fixed_fake.append(cp)
    for cp in blank:
        if cp not in fixed_fake:
            c.body[cp] = body10(cp)
            fixed_fake.append(cp)

    added = []
    notdef_bmps = {bm for bm, _cps in shared}
    for cp in missing:
        bm = Z.render(bf, chr(cp), Z.BODY_H, Z.BODY_RB, Z.BODY_WCOLS,
                      Z.BODY_THR, Z.BODY_DX, Z.BODY_DY)
        # 源字体没有该字时 PIL 给 .notdef；退回备用点阵字体
        if not any(bm) or bm in notdef_bmps:
            bm = body10(cp)
        c.body[cp] = bm
        c.title[cp] = title12(cp)
        added.append(cp)

    c.body_idx = sorted(c.body)
    c.title_idx = sorted(c.title)
    # 新增码点后字数字段也要同步，否则 check_consistency() 会拿旧 n_body 比对
    c.n_body = len(c.body)
    c.n_title = len(c.title)
    c.check_consistency()
    out = c.build()
    BIN.write_bytes(out)

    print(f"修复：替换假字形 {len(fixed_fake)} 个（"
          + "".join(chr(x) for x in sorted(fixed_fake)) + "）")
    print(f"      新增缺字 {len(added)} 个（"
          + "".join(chr(x) for x in added) + "）")
    print(f"写入 {BIN}（{len(out)} B · {len(c.body)} 字）")
    sync_headers(len(c.body))
    print("⚠ 下一步：pack_assets.py --write 重打 assets，并烧 0x400000 分区")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="zfont.bin 字形有效性闸门")
    ap.add_argument("--fix", action="store_true", help="修复并写回 zfont.bin")
    ap.add_argument("--list", action="store_true", help="打印问题明细")
    a = ap.parse_args()

    c = Z.Container(BIN.read_bytes())
    c.check_consistency()
    if a.fix:
        rc = fix(c)
        c2 = Z.Container(BIN.read_bytes())
        c2.check_consistency()
        return rc or check(c2, verbose=True)
    return check(c, verbose=True)


if __name__ == "__main__":
    sys.exit(main())
