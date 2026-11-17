# -*- coding: utf-8 -*-
"""关机页 off.bin 生成器（2026-11-13：两张轮替）。

输出格式：**两页 5000B 串联**（共 10000B），固件 power_off() 按 NVS `off_alt` 选一页：
  · 第 0 页 = A（旧的 TRAVEL CAT 白底图）—— 源 = assets/off_a.bin（5000B，已入库）
  · 第 1 页 = B（用户 2026-11-13 给的新图）—— 源 = 下面 SRC 的 PNG

每页都是 200×200 1bpp、每行 25B、MSB 在前、**1 = 白**。
判据：亮度 < 128 → 墨，不做网点/抖动（这张图本身就是线稿，二次网点会糊成灰块）。

换 B 图步骤：改 SRC → python scripts/_tools/gen_off_page.py → pack_assets.py --write
换 A 图步骤：直接替换 assets/off_a.bin（必须正好 5000B）后重跑本脚本。
"""
import os
import sys
import tempfile

import numpy as np
from PIL import Image

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(BASE, "..", ".."))
ASSETS = os.path.join(ROOT, "fw_epaper_demo", "main", "assets")
import pet_paths                      # ★R217：统一路径解析（不再写死 C:/epaper）
SRC_B = str(pet_paths.inbox() / "图片节点 13 (9).png")     # ★用户 2026-11-13 给的新关机页
A_BIN = os.path.join(ASSETS, "off_a.bin")          # A 页（旧图）已入库
OUT = os.path.join(ASSETS, "off.bin")

SZ = 200
PAGE = 5000


def pack_png(path):
    im = Image.open(path).convert("RGB")
    if im.size != (SZ, SZ):
        raise SystemExit("%s 不是 %dx%d（实际 %s）" % (path, SZ, SZ, im.size))
    ink = np.asarray(im).mean(axis=2) < 128          # True = 墨
    buf = np.packbits((~ink).astype(np.uint8), axis=1, bitorder="big").tobytes()
    if len(buf) != PAGE:
        raise SystemExit("打包 %d != %d" % (len(buf), PAGE))
    return buf, float(ink.mean())


def main():
    a = open(A_BIN, "rb").read()
    if len(a) != PAGE:
        raise SystemExit("%s 应为 %d B，实际 %d" % (A_BIN, PAGE, len(a)))
    b, ink_b = pack_png(SRC_B)
    open(OUT, "wb").write(a + b)
    print(">>> off.bin  A(旧) %d B + B(新) %d B = %d B（B 墨 %.1f%%）"
          % (len(a), len(b), len(a) + len(b), 100.0 * ink_b))
    # 预览：A 上 / B 下，并排放大 2 倍，便于肉眼比对
    pa = np.unpackbits(np.frombuffer(a, dtype=np.uint8)).reshape(SZ, SZ)
    pb = np.unpackbits(np.frombuffer(b, dtype=np.uint8)).reshape(SZ, SZ)
    top = np.where(pa, 255, 0).astype(np.uint8)
    bot = np.where(pb, 255, 0).astype(np.uint8)
    canvas = np.full((SZ * 2 + 8, SZ), 128, np.uint8)
    canvas[0:SZ] = top
    canvas[SZ + 8:] = bot
    preview = os.path.join(tempfile.gettempdir(), "off_preview.png")   # ★R216：原写到已删的 C:/epaper/_tmp/，脚本必抛异常
    Image.fromarray(canvas).resize((SZ * 3, (SZ * 2 + 8) * 3), Image.NEAREST).save(preview)
    print(f">>> {preview}（上=A 旧图，下=B 新图）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
