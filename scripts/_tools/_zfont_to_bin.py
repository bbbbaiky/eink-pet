# -*- coding: utf-8 -*-
"""zfont.bin 生成/增补工具（★2026-10-28 重建，原 _preview/gen_zfont.py 已失传）

为什么有这个文件
----------------
`common/zfont.bin` 是固件运行时字形容器（'ZFNT'）。它的两个字形来源是两套点阵字体：
    正文档 BODY  = 精品点阵体9x9 常规体  @ size=10  格 10×10 / 行 2 字节 / 前进 10
    标题档 TITLE = ZLabs 点阵字 12px     @ size=12  格 12×12 / 行 2 字节 / 前进 12
容器布局（与 common/main.c 的 zfont_idx_load() 严格对应，全小端）：
    0x00 'ZFNT' · 0x04 ver · 0x08 n_body_cjk · 0x0C n_title_cjk
    0x10 body_ascii  95×11（[0]=前进宽，后 10B 位图）
         title_ascii 95×12
         body_cjk    n×24（u32 码点 + 20B 位图，10 行 × 2 字节）
         title_cjk   n×28（u32 码点 + 24B 位图，12 行 × 2 字节）
         body_idx    n×4（码点，升序）
         title_idx   n×4（与 body_idx 逐字节相同 —— 两档共用同一字符集）
    长度必须**恰好**等于上面累加值，否则固件 zfont_idx_load() 直接返回 false（开机报资产错）。

两种用法
--------
1) 自检（推荐每次改前先跑，证明当前 bin 与字体源逐位一致）：
       python scripts/_tools/_zfont_to_bin.py --verify
   —— 会把 7455 个 BODY + 7455 个 TITLE 全部重渲一遍逐字节比对，然后不写任何文件。

2) 增补字符（只增不减，已有字形一律原样搬运，绝不重渲）：
       python scripts/_tools/_zfont_to_bin.py --add "。、・【】込曇転…"
   —— 新字用同一套字体/尺寸/阈值/落点渲染，插到码点升序的正确位置，
      两档同时增补（保持 body_idx == title_idx）。

字体源（两个都必须在，缺了脚本会直接报错并给出路径）
    正文档：BoutiqueBitmap9x9_1.9（常规体）.ttf
           ⚠ 不是「粗体」！粗体是另一款字重，逐位对不上（实测 0/400）。
    标题档：★源头_ZLabs点阵字12px.ttf
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
BIN = ROOT / "common" / "zfont.bin"

# ── 已验证的渲染配方（2026-10-28 用 400 字抽样 × 两档全量对账得出，逐位 400/400）──
BODY_SIZE, BODY_THR, BODY_DX, BODY_DY = 10, 128, 0, 1
TITLE_SIZE, TITLE_THR, TITLE_DX, TITLE_DY = 12, 128, 0, 0

BODY_H, BODY_RB, BODY_WCOLS = 10, 2, 10
TITLE_H, TITLE_RB, TITLE_WCOLS = 12, 2, 12
ASCII_LO, ASCII_HI = 0x20, 0x7E
ASCII_N = ASCII_HI - ASCII_LO + 1

import pet_paths                     # ★R217：统一路径解析，去掉本机绝对路径
FONT_CANDIDATES = {
    "body": [
        ROOT / "scripts" / "fonts" / "BoutiqueBitmap9x9_1.9_R.ttf",
        pet_paths.inbox() / "BoutiqueBitmap9x9_1.9_R.ttf",
        pet_paths.inbox() / "BoutiqueBitmap9x9_1.9（常规体）.ttf",
    ],
    "title": [
        ROOT / "scripts" / "fonts" / "ZLabsPixel12px.ttf",
        pet_paths.inbox() / "★源头_ZLabs点阵字12px.ttf",
    ],
}

MAGIC = b"ZFNT"
# ★R183：容器版本
#   ver 1 = 旧布局（两条码点索引：body_idx + title_idx，两者逐字节相同）
#   ver 2 = 只留一条索引（title_idx 是 body_idx 的逐字节副本，开机只拿它做一致性校验，
#           校验完就没用了）⇒ assets 省 12306×4 = 49,224 B
VER = 2


def find_font(kind: str) -> Path:
    for p in FONT_CANDIDATES[kind]:
        if p.is_file():
            return p
    msg = "\n".join(f"    {p}" for p in FONT_CANDIDATES[kind])
    sys.exit(f"!! 找不到 {kind} 字体源，依次找过：\n{msg}\n"
             f"   正文档必须是「精品点阵体9x9 常规体」(不是粗体)，标题档是 ZLabs 12px。")


class Container:
    """把 zfont.bin 拆成 ASCII 位图 + 两档 {码点: 位图}。"""

    def __init__(self, raw: bytes):
        if raw[:4] != MAGIC:
            sys.exit("!! zfont.bin magic 不是 'ZFNT'")
        self.ver = struct.unpack_from("<I", raw, 4)[0]
        self.n_body = struct.unpack_from("<I", raw, 8)[0]
        self.n_title = struct.unpack_from("<I", raw, 12)[0]

        o = 16
        self.body_ascii = raw[o:o + ASCII_N * 11]; o += ASCII_N * 11
        self.title_ascii = raw[o:o + ASCII_N * 12]; o += ASCII_N * 12
        body_off = o; o += self.n_body * 24
        title_off = o; o += self.n_title * 28
        bidx_off = o; o += self.n_body * 4
        if self.ver >= 2:
            tidx_off = bidx_off                     # ★R183：ver2 只有一条索引
        else:
            tidx_off = o; o += self.n_title * 4
        if o != len(raw):
            sys.exit(f"!! bin 长度 {len(raw)} ≠ 布局推算 {o}（版本不匹配）")

        self.body = {}
        for i in range(self.n_body):
            p = body_off + i * 24
            self.body[struct.unpack_from("<I", raw, p)[0]] = raw[p + 4:p + 24]
        self.title = {}
        for i in range(self.n_title):
            p = title_off + i * 28
            self.title[struct.unpack_from("<I", raw, p)[0]] = raw[p + 4:p + 28]
        self.body_idx = [struct.unpack_from("<I", raw, bidx_off + i * 4)[0] for i in range(self.n_body)]
        self.title_idx = [struct.unpack_from("<I", raw, tidx_off + i * 4)[0] for i in range(self.n_title)]

    def check_consistency(self) -> None:
        if len(self.body) != self.n_body or len(self.title) != self.n_title:
            sys.exit("!! 表内出现重复码点")
        if self.body_idx != sorted(self.body_idx) or self.title_idx != sorted(self.title_idx):
            sys.exit("!! 索引不是码点升序（固件靠二分查找，必须升序）")
        if self.body_idx != self.title_idx:
            sys.exit("!! body_idx 与 title_idx 不一致 —— 两档必须共用同一字符集"
                     "（固件只在开机校验，不一致时标题会错字）")

    def build(self) -> bytes:
        cps = sorted(self.body)
        out = bytearray()
        out += MAGIC
        out += struct.pack("<III", VER, len(cps), len(cps))
        out += self.body_ascii
        out += self.title_ascii
        for cp in cps:
            out += struct.pack("<I", cp) + self.body[cp]
        for cp in cps:
            out += struct.pack("<I", cp) + self.title[cp]
        for cp in cps:                       # ★R183：只留一条索引（ver2）
            out += struct.pack("<I", cp)
        return bytes(out)


def render(font, ch: str, h: int, rb: int, wcols: int, thr: int, dx: int, dy: int) -> bytes:
    """按已验证配方把单字渲成 h 行 × wcols 列的位图（每行 rb 字节，bit7 = 最左列）。"""
    from PIL import Image, ImageDraw

    img = Image.new("L", (64, 64), 255)
    ImageDraw.Draw(img).text((0, 0), ch, font=font, fill=0)
    buf, W = img.tobytes(), img.width
    out = bytearray(rb * h)
    for r in range(h):
        for c in range(wcols):
            p = (dy + r) * W + dx + c
            if 0 <= p < len(buf) and buf[p] < thr:
                out[r * rb + (c >> 3)] |= 0x80 >> (c & 7)
    return bytes(out)


def fonts():
    from PIL import ImageFont
    return (ImageFont.truetype(str(find_font("body")), BODY_SIZE),
            ImageFont.truetype(str(find_font("title")), TITLE_SIZE))


def verify(c: Container) -> int:
    from PIL import ImageFont
    body_f, title_f = fonts()
    # ★R154：正文档源字体 BoutiqueBitmap9x9 缺一批生僻字，补字工具
    # check_zfont_glyphs.py 会用标题档同一点阵字体 ZLabs @10px 渲 BODY。
    # 自检对这类字自动改用备用字体逐位比对，不再误报「BODY 不符」。
    body_f10 = ImageFont.truetype(str(find_font("title")), BODY_SIZE)
    bad_b = bad_t = blank = alt = 0
    for cp, bm in c.body.items():
        if cp < 0x80:
            continue
        nb = render(body_f, chr(cp), BODY_H, BODY_RB, BODY_WCOLS, BODY_THR, BODY_DX, BODY_DY)
        if nb != bm and render(body_f10, chr(cp), BODY_H, BODY_RB, BODY_WCOLS,
                               BODY_THR, BODY_DX, BODY_DY) == bm:
            alt += 1                    # 备用字体补的字，算通过
        elif nb != bm:
            bad_b += 1
            if bad_b <= 8:
                print(f"   BODY 不符 U+{cp:04X} {chr(cp)}")
        if not any(bm):
            blank += 1
    for cp, bm in c.title.items():
        if cp < 0x80:
            continue
        nt = render(title_f, chr(cp), TITLE_H, TITLE_RB, TITLE_WCOLS, TITLE_THR, TITLE_DX, TITLE_DY)
        if nt != bm:
            bad_t += 1
            if bad_t <= 8:
                print(f"   TITLE 不符 U+{cp:04X} {chr(cp)}")
    n_b = sum(1 for cp in c.body if cp >= 0x80)
    print(f"自检：BODY {n_b - bad_b}/{n_b} 一致（备用字体补字 {alt} 个），"
          f"TITLE {n_b - bad_t}/{n_b} 一致，全空字形 {blank} 个")
    return bad_b + bad_t


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--add", default="", help="要增补的字符（一串即可，会自动去重/排序）")
    ap.add_argument("--verify", action="store_true", help="只做全量逐位自检，不写文件")
    ap.add_argument("--compact", action="store_true",
                    help="★R183：只把现有 bin 重打成 ver2 布局（去掉重复的 title_idx），不重渲任何字形")
    ap.add_argument("--out", default=str(BIN))
    a = ap.parse_args()

    c = Container(BIN.read_bytes())
    c.check_consistency()
    print(f"当前：{len(c.body)} 字（ver{c.ver}，body_idx/title_idx 一致）")
    if a.compact:
        out = c.build()
        Path(a.out).write_bytes(out)
        print(f"重打为 ver{VER}：{len(BIN.read_bytes())} → {len(out)} 字节"
              f"（省 {len(BIN.read_bytes()) - len(out)} B）")
        print("⚠ 字体/字形一个字节都没重渲，只删了重复索引")
        return
    if a.verify or not a.add:
        n_bad = verify(c)
        if not a.add:
            print("（未给 --add，未写文件）")
        return

    add = sorted({ord(ch) for ch in a.add if ord(ch) >= 0x80} - set(c.body))
    if not add:
        print("没有需要增补的字")
        return
    body_f, title_f = fonts()
    for cp in add:
        bm_b = render(body_f, chr(cp), BODY_H, BODY_RB, BODY_WCOLS, BODY_THR, BODY_DX, BODY_DY)
        bm_t = render(title_f, chr(cp), TITLE_H, TITLE_RB, TITLE_WCOLS, TITLE_THR, TITLE_DX, TITLE_DY)
        if not any(bm_b) or not any(bm_t):
            sys.exit(f"!! U+{cp:04X} {chr(cp)} 渲出来是空字形（字体里可能只有空格位）")
        c.body[cp] = bm_b
        c.title[cp] = bm_t
    out = c.build()
    Path(a.out).write_bytes(out)
    print(f"增补 {len(add)} 字 → 共 {len(c.body)} 字；写入 {a.out}（{len(out)} 字节）")
    print("增补清单：" + "".join(chr(x) for x in add))
    print(f"⚠ 记得同步改 zfont.h 的 ZF_BODY_CJK_N / ZF_TITLE_CJK_N → {len(c.body)}u")


if __name__ == "__main__":
    main()
