# -*- coding: utf-8 -*-
"""科技房间（用户改稿版）→ room_tech.bin

源：<PET_INBOX 或仓库同级>/inbox/科技房间_房间图_{清晨,正午,傍晚,深夜}.png（200×200，窗户处 alpha=0）
产物：4 × 5000B 底图（1=白） + 1 × 5000B 掩膜（1=画，0=窗洞）
★ 用户已在自己的稿里处理过网点/暗部，这里**不再做任何二次网点加工**，原样转 1bpp。
★R217：允许「只放某一时段的图」——缺的时段沿用现有产物该帧并打醒目告警；
  加 --strict 则缺任一源图直接报错。窗洞掩膜取自清晨图（全缺时沿用现有掩膜）。
"""
import os
import sys

import numpy as np
from PIL import Image

BASE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(BASE, "..", ".."))
import pet_paths                      # ★R217：统一路径解析（不再写死 C:/epaper）
SRC_DIR = str(pet_paths.inbox())
OUT = str(pet_paths.assets_dir() / "room_tech.bin")
PERIODS = ["清晨", "正午", "傍晚", "深夜"]
SZ = 200
BIN_BYTES = 5000


def pack_bits(a01):
    b = np.packbits(a01.astype(np.uint8), axis=1, bitorder="big").tobytes()
    if len(b) != BIN_BYTES:
        raise SystemExit("打包后 %d 字节，应为 %d" % (len(b), BIN_BYTES))
    return b


def main():
    blob = bytearray()
    hole = None
    missing = []
    old = open(OUT, "rb").read() if os.path.exists(OUT) else b""
    for i, per in enumerate(PERIODS):
        p = os.path.join(SRC_DIR, "科技房间_房间图_%s.png" % per)
        if not os.path.exists(p):
            # ★R217：这是「只更新一个时段」的既定能力（用户常只放一张新图），
            #   但原来静默保留太隐蔽 —— 现在打醒目告警并汇总；--strict 可强制要求齐全。
            if "--strict" in sys.argv:
                raise SystemExit("缺源图 %s（--strict：不允许沿用现有帧）" % p)
            if len(old) >= 5 * BIN_BYTES:
                blob += old[i * BIN_BYTES:(i + 1) * BIN_BYTES]
                print("  ⚠ %-3s 源图缺失 → 沿用现有产物第 %d 帧" % (per, i))
                missing.append(per)
                continue
            raise SystemExit("缺源图 %s（且无现有产物可保留）" % p)
        a = np.asarray(Image.open(p).convert("RGBA"))
        if a.shape[:2] != (SZ, SZ):
            raise SystemExit("%s 尺寸不对：%s" % (p, a.shape))
        alpha = a[:, :, 3] > 0
        ink = alpha & (a[:, :, :3].mean(axis=2) < 128)
        if hole is None:
            hole = alpha
        elif not np.array_equal(hole, alpha):
            print("  ⚠ %s 的透明洞与清晨不一致（掩膜仍用清晨的）" % per)
        blob += pack_bits(~ink)                       # 1 = 白
        print("  %-3s 墨 %5.2f%%" % (per, 100 * ink.mean()))
    if hole is None:
        # ★R217 真 bug 修：4 张源图全缺时 hole 仍是 None，原代码 pack_bits(None) 直接崩。
        #   这里从现有产物的掩膜帧读回，保证「只靠现有 bin 重建」也能跑通。
        if len(old) >= 5 * BIN_BYTES:
            hole = np.unpackbits(
                np.frombuffer(old[4 * BIN_BYTES:5 * BIN_BYTES], dtype=np.uint8)
            ).reshape(SZ, SZ).astype(bool)
            print("  ⚠ 全部源图缺失 → 窗洞掩膜沿用现有产物")
        else:
            raise SystemExit("缺全部源图，且没有现有掩膜可沿用")
    blob += pack_bits(hole)
    with open(OUT, "wb") as f:
        f.write(blob)
    if missing:
        print("  ⚠ 本次 %d 个时段沿用旧帧：%s（要禁止请加 --strict）"
              % (len(missing), "/".join(missing)))
    print(">>> %s（4×%d + 掩膜）" % (OUT, BIN_BYTES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
