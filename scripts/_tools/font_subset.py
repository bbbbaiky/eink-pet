#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# font_subset.py —— 按「固件实际会显示的字」裁剪 zfont.bin（2026-10-28 · R107）
# ----------------------------------------------------------------------------
# 背景（R106 事故）：
#   R106 的补字脚本判定条件是 `cp > 0x7F`（源字体里有这个字形就收），于是把
#   BoutiqueBitmap9x9 + ZLabsPixel12px 两套点阵字体的**全部非 ASCII 字形**搬进了
#   字库：7880 → 12023 字，zfont.bin 475 KB → 723 KB。其中 8201 个字在整个固件里
#   一次都不会出现（含 722 个 ¡¢£¤¥…ÿ 拉丁符号），白占约 480 KB —— 直接把
#   assets 顶过 mypost 预留界，用户明信片仓被禁用。
#
# 本脚本的做法：
#   1) 目标字符集 = 全部 .c/.h 源文件里出现的非 ASCII 字符（固件自带文案的唯一真源：
#      i18n 表、地名库、收藏品描述、明信片模板文案全都在 .h/.c 里）
#      + 可选 --gb1 追加 GB2312 一级汉字 3755（给手机端输入 / AI 文案兜底）
#   2) 与现有字库求交（保证 BODY / TITLE 两档都有位图 ⇒ 满足同集硬约束）
#   3) 按原顺序搬运位图字节，重建升序索引 —— **不重新渲染**，字形与 R106 逐位一致
#
# ⚠ 硬约束：body_idx 必须等于 title_idx（两档共用同一字符集），固件
#   zfont_idx_load() 与 Container.check_consistency() 都会校验。本脚本只取交集。
#
# 用法：
#   python scripts/_tools/font_subset.py                 # 预览（不写文件）
#   python scripts/_tools/font_subset.py --gb1           # 预览「源码用字 + GB2312一级」
#   python scripts/_tools/font_subset.py --write         # 写入 common/zfont.bin
#   python scripts/_tools/font_subset.py --gb1 --write
#   python scripts/_tools/font_subset.py --check         # 只做「源码用字是否全含」断言
#
# 输出 zfont.bin 后必须同步改 zfont.h 的 ZF_BODY_CJK_N / ZF_TITLE_CJK_N（本脚本会打印）。
# ============================================================================
import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ZFONT = ROOT / "common" / "zfont.bin"

N_ASCII = 95
ASCII_BODY_SZ = 11
ASCII_TITLE_SZ = 12
BODY_GLYPH_SZ = 24
TITLE_GLYPH_SZ = 28

# 源字体本身没有字形、因此永远补不出来的字符（实测：均不上屏，见 R107 核对）
# 保留登记以便 --check 报「已知豁免」而不是报错。
KNOWN_UNAVAILABLE = {
    0x015F, 0x0163, 0x1E29, 0x1EAD,  # ş ţ ḩ ậ  地名数据，fold 后落回 ASCII
    0x2039,                          # ‹        仅注释
    0x21C4, 0x220F, 0x2259,          # ⇄ ∏ ≙    仅注释
    0x226A, 0x226B, 0x22EF,          # ≪ ≫ ⋯    仅注释
    0x2550, 0x25B4, 0x25BE,          # ═ ▴ ▾    注释 / 手机网页 HTML
    0x26A0, 0x26D4, 0x2705,          # ⚠ ⛔ ✅   仅注释
    0x2713, 0x2714, 0x2717, 0x274C,  # ✓ ✔ ✗ ❌  仅注释
    0x3000,                          # 全角空格  仅日志
    0x7300, 0x92E5, 0x9962,          # 猀 鋥 饢  仅注释（bug 说明 / 字库变更记录）
    0xFEFF,                          # BOM
}


def load_zfont(path):
    d = path.read_bytes()
    if d[:4] != b"ZFNT":
        sys.exit("!! %s 不是 ZFNT 字库" % path)
    ver = struct.unpack_from("<I", d, 4)[0]
    nb, nt = struct.unpack_from("<II", d, 8)
    p = 16
    ascii_body = d[p:p + N_ASCII * ASCII_BODY_SZ]
    p += N_ASCII * ASCII_BODY_SZ
    ascii_title = d[p:p + N_ASCII * ASCII_TITLE_SZ]
    p += N_ASCII * ASCII_TITLE_SZ
    body_glyphs = d[p:p + nb * BODY_GLYPH_SZ]
    p += nb * BODY_GLYPH_SZ
    title_glyphs = d[p:p + nt * TITLE_GLYPH_SZ]
    p += nt * TITLE_GLYPH_SZ
    body_idx = list(struct.unpack_from("<%dI" % nb, d, p))
    p += nb * 4
    title_idx = list(struct.unpack_from("<%dI" % nt, d, p))
    return dict(ver=ver, nb=nb, nt=nt, ascii_body=ascii_body, ascii_title=ascii_title,
                body_glyphs=body_glyphs, title_glyphs=title_glyphs,
                body_idx=body_idx, title_idx=title_idx, raw=d)


def scan_source_used():
    """扫全部 .c/.h 里出现的非 ASCII 字符。"""
    used = {}
    files = [p for p in list(ROOT.rglob("*.c")) + list(ROOT.rglob("*.h"))
             if "_build" not in p.parts and p.parts[0] != "scripts"]
    for p in files:
        try:
            t = p.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        for i, ln in enumerate(t.splitlines(), 1):
            for ch in ln:
                cp = ord(ch)
                if cp > 0x7F:
                    used.setdefault(cp, (p.name, i))
    return used, len(files)


def gb2312_level1():
    out = set()
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            try:
                out.add(ord(bytes([hi, lo]).decode("gb2312")))
            except Exception:
                pass
    return out


def build_subset(zf, target):
    """按原顺序挑出 target 里两档都有的字，返回新 zfont bytes + 统计。"""
    avail = set(zf["body_idx"]) & set(zf["title_idx"])
    keep = sorted(target & avail)
    drop = sorted(target - avail)
    pos = {cp: i for i, cp in enumerate(zf["body_idx"])}
    new_body = b"".join(zf["body_glyphs"][pos[cp] * BODY_GLYPH_SZ:(pos[cp] + 1) * BODY_GLYPH_SZ]
                        for cp in keep)
    new_title = b"".join(zf["title_glyphs"][pos[cp] * TITLE_GLYPH_SZ:(pos[cp] + 1) * TITLE_GLYPH_SZ]
                         for cp in keep)
    n = len(keep)
    out = bytearray()
    out += b"ZFNT"
    out += struct.pack("<I", zf["ver"])
    out += struct.pack("<II", n, n)
    out += zf["ascii_body"]
    out += zf["ascii_title"]
    out += new_body
    out += new_title
    out += struct.pack("<%dI" % n, *keep)
    out += struct.pack("<%dI" % n, *keep)
    return bytes(out), keep, drop


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gb1", action="store_true", help="追加 GB2312 一级汉字 3755 做兜底")
    ap.add_argument("--write", action="store_true", help="写入 common/zfont.bin")
    ap.add_argument("--check", action="store_true", help="只断言源码用字是否全含")
    args = ap.parse_args()

    zf = load_zfont(ZFONT)
    have = set(zf["body_idx"])
    used, nfiles = scan_source_used()

    print("现有字库 : %d 字 · %d B" % (zf["nb"], len(zf["raw"])))
    print("扫描源文件: %d 个 .c/.h" % nfiles)
    print("源码用字 : %d 个唯一非 ASCII 字符" % len(used))

    missing = sorted(set(used) - have)
    hard_missing = [c for c in missing if c not in KNOWN_UNAVAILABLE]
    if missing:
        print("字库缺字 : %d 个（其中已知豁免 %d 个）"
              % (len(missing), len(missing) - len(hard_missing)))
        for c in missing:
            f, ln = used[c]
            tag = "豁免" if c in KNOWN_UNAVAILABLE else "★真缺口"
            print("   %s U+%04X %r  首现 %s:%d" % (tag, c, chr(c), f, ln))
    if hard_missing:
        print("!! 有 %d 个非豁免缺字，先补字再裁字库" % len(hard_missing))
        if args.check:
            return 1

    if args.check:
        print("✓ --check 通过：源码用字已全部被现有字库覆盖")
        return 0

    target = set(used)
    if args.gb1:
        g = gb2312_level1()
        print("GB2312 一级汉字: %d 个（已含 %d）" % (len(g), len(g & have)))
        target |= g

    out, keep, drop = build_subset(zf, target)
    print("\n目标集合 : %d 字 → 实际产出 %d 字" % (len(target), len(keep)))
    print("现有字库 : %d B → 新字库 %d B（省 %d B = %.0f KB）"
          % (len(zf["raw"]), len(out), len(zf["raw"]) - len(out),
             (len(zf["raw"]) - len(out)) / 1024))
    if drop:
        print("目标里但字库无位图（已剔除）: %d 个" % len(drop))
    print("★ zfont.h 需设 ZF_BODY_CJK_N = ZF_TITLE_CJK_N = %du" % len(keep))

    if args.write:
        ZFONT.write_bytes(out)
        print("\n✓ 已写入 %s" % ZFONT)
    else:
        print("\n（预览模式，未写入。加 --write 落盘）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
